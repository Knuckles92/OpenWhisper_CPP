#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "../dictation/automation_support.h"
#include "../dictation/runtime_support.h"
#include "automation/action_runtime.h"
#include "automation/catalog.h"
#include "automation/dictation_bridge.h"
#include "automation/event_bus.h"
#include "automation/filters.h"
#include "automation/routing.h"
#include "automation/secrets.h"
#include "automation/signing.h"
#include "automation/webhooks.h"
#include "base/identity.h"
#include "credentials/testing/memory_backend.h"
#include "dictation/messages.h"
#include "llm/testing/fake_judge.h"
#include "pyport/text.h"
#include "support/action_support.h"
#include "support/hook_support.h"
#include "support/loopback_receiver.h"

// DictationAutomation, the port the transcription runtime calls, joining M1.14's real
// TranscriptionRuntime (a fake recorder and speech engine, History on a temporary database, an
// upper-casing cleaner) to the real action runtime, text filters, Route by voice and webhooks:
// tests/test_automation_actions_pipeline.py, test_automation_hooks_pipeline.py and
// test_automation_r3_lease_routing.py, then the package's acceptance, "Home, turn on the porch
// light". Only the blocking work of actions, routing's judge and the cleaner are fakes; filters
// and webhooks talk to a receiver on 127.0.0.1.

using namespace fv;
using namespace fv::automation;
using namespace std::chrono_literals;
namespace dt = fv::dictation::testing;
using automation::test::Answer;
using automation::test::ReceivedRequest;
using automation::test::Receiver;
using dictation::DictationJob;
using dictation::JobMode;

namespace {

const std::string kHeard = "ask claude what time it is";

focus::AppIdentity editor() {
    return automation::test::app_identity("code.exe", "Code", 7, "0x7");
}

// The app's ActionHost over the rig: the status line and notices recorded, the clipboard's
// writes recorded, a paste through the transcription runtime.
class BridgeHost final : public ActionHost {
public:
    void set_status(std::string text) override {
        std::lock_guard lock(mutex_);
        statuses.push_back(std::move(text));
    }
    void show_notice(std::string text, bool) override {
        std::lock_guard lock(mutex_);
        notices.push_back(std::move(text));
    }
    bool show_in_scratchpad(std::string text) override {
        std::lock_guard lock(mutex_);
        scratchpad.push_back(std::move(text));
        return true;
    }
    bool copy_to_clipboard(std::string text, std::string) override {
        std::lock_guard lock(mutex_);
        copied.push_back(std::move(text));
        return true;
    }
    bool paste_text_now(std::string text, const DictationJob* job) override {
        return runtime->paste_text_now(std::move(text), job);
    }
    dictation::PasteTarget paste_target(const DictationJob& job) override {
        return dictation::paste_target(&job, capture);
    }
    void discard_clipboard_prefetch() override {}

    std::vector<std::string> statuses_now() const {
        std::lock_guard lock(mutex_);
        return statuses;
    }

    dictation::TranscriptionRuntime* runtime = nullptr;
    focus::CaptureService* capture = nullptr;
    std::vector<std::string> statuses;
    std::vector<std::string> notices;
    std::vector<std::string> scratchpad;
    std::vector<std::string> copied;

private:
    mutable std::mutex mutex_;
};

struct BridgeOptions {
    Json settings = Json{{"auto_paste", true},
                         {"copy_clipboard", true},
                         {"restore_clipboard", false},
                         {"transcript_cleanup_enabled", true},
                         {"actions", Json::array({dt::ask_action(), dt::raw_action()})}};
    std::string heard = kHeard;
};

// The foundation pipeline harness (test_automation_foundation_pipeline.py h) with the real
// automation behind the transcription runtime: every event on one EventBus.
struct Bridge {
    explicit Bridge(BridgeOptions options = {})
        : settings(options.settings), recorder(root.path() / "recorded_audio.wav", trace),
          engine(
              std::make_shared<speech::FakeEngine>(dt::AutomationRig::script_for(options.heard))),
          speech(std::make_unique<speech::SpeechService>(engine->factory(), notify)),
          database(std::make_unique<Database>(Database::open_or_create(root.path() / "test.db"))),
          sync(trace),
          history(std::make_unique<HistoryManager>(*database, root.path() / "recordings",
                                                   RecordingRetention{}, history_services())),
          stats(std::make_unique<dictation_stats::StatsRepository>(*database)), clipboard(trace),
          cleaner([](const std::string& text) { return dt::AutomationRig::upper(text); }),
          credentials(std::make_shared<MemoryCredentialBackend>(), identity()) {
        speech->select(speech::Slot::Dictation, dt::fake_parakeet_config());
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::minutes(2);
        while (speech->readiness(speech::Slot::Dictation).state == speech::ReadyState::Loading) {
            if (std::chrono::steady_clock::now() > deadline) {
                throw std::runtime_error("the fake engine did not load");
            }
            std::this_thread::sleep_for(2ms);
        }
        capture.snapshot = [](bool, bool) {
            return focus::FocusSnapshot{editor(), std::nullopt};
        };
        capture.identity = editor();
        keys.on_send = [this](const std::string& what) {
            trace.add(what);
        };
        provider.set_judge(judge);

        ActionServices services;
        services.http = http;
        services.run_once = [this](const std::vector<std::string>& argv, std::string_view input,
                                   double timeout_s, const CancelToken& cancel,
                                   const std::map<std::string, std::string>& extra_env) {
            {
                std::lock_guard lock(mutex);
                stdins.emplace_back(std::string(input), std::this_thread::get_id());
            }
            const auto fn = run_once.get();
            if (!fn) {
                throw std::logic_error("run_once was not expected");
            }
            return fn(argv, input, timeout_s, cancel, extra_env);
        };
        services.open_uri = [this](std::string_view uri) {
            std::lock_guard lock(mutex);
            opened.emplace_back(uri);
        };
        services.subscribers = [this](std::string_view label) {
            return bus.subscribers(label);
        };
        actions = std::make_unique<ActionRuntime>(host, settings, credentials, bus, action_ui,
                                                  std::move(services));
        automation = std::make_unique<DictationAutomation>(*actions, leases, provider, bus, http);

        dictation::TranscriptionServices transcription;
        transcription.settings = &settings;
        transcription.recorder = &recorder;
        transcription.speech = speech.get();
        transcription.history = history.get();
        transcription.stats = stats.get();
        transcription.delivery.clipboard = &clipboard;
        transcription.delivery.keys = &keys;
        transcription.activity = &activity;
        transcription.ui = &ui;
        transcription.jobs = &jobs;
        transcription.persistence = &persistence;
        transcription.events = &bus;
        transcription.capture = &capture;
        transcription.hooks.stop_playback = [] {
        };
        runtime = std::make_unique<dictation::TranscriptionRuntime>(std::move(transcription));
        runtime->set_automation(*automation);
        runtime->set_cleaner(&cleaner);
        runtime->set_command_runtime(&rewriter);
        host.runtime = runtime.get();
        host.capture = &capture;
        links.push_back(runtime->status_update.connect(
            ui, [this](std::string status) { trace.add("status:" + status); }));
    }
    ~Bridge() {
        links.clear();
        actions->close();
        runtime.reset();
        automation.reset();
        actions.reset();
        speech.reset();
        stats.reset();
        history.reset();
        database->close();
        bus.close();
    }
    Bridge(const Bridge&) = delete;
    Bridge& operator=(const Bridge&) = delete;

    HistoryServices history_services() {
        HistoryServices services;
        services.record_sync = &sync;
        return services;
    }
    void hears(const std::string& text) { engine->set_script(dt::AutomationRig::script_for(text)); }
    void with_actions(const std::vector<Json>& rows) { settings.update(Json{{"actions", rows}}); }

    // The jobs and History saves queued, and what they queue, until none is left.
    void run_workers() {
        while (jobs.run_all() + persistence.run_all() > 0) {
        }
    }
    void run_jobs() {
        while (jobs.run_all() > 0) {
        }
    }
    void run_saves() {
        while (persistence.run_all() > 0) {
        }
    }
    // _claim_job(job) and transcribe_audio_file(audio), then the workers.
    void dictate(std::shared_ptr<const DictationJob> job) {
        REQUIRE(runtime->claim_job(std::move(job)));
        const auto path = root.path() / "speech.wav";
        std::filesystem::copy_file(dt::audio_fixture("tone-16k-mono.wav"), path,
                                   std::filesystem::copy_options::overwrite_existing);
        runtime->transcribe_audio_file(path);
        run_workers();
    }
    // Runs the action runtime's results until `predicate` holds.
    bool pump(const std::function<bool()>& predicate, std::chrono::milliseconds timeout = 10s) {
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        for (;;) {
            action_ui.run_all();
            run_workers();
            if (predicate()) {
                return true;
            }
            if (std::chrono::steady_clock::now() > deadline) {
                return false;
            }
            std::this_thread::sleep_for(5ms);
        }
    }

    std::vector<Json> events(std::string_view type) const {
        std::vector<Json> found;
        for (const Event& event : bus.recent(500, std::vector<std::string>{std::string(type)})) {
            found.push_back(event.data);
        }
        return found;
    }
    std::vector<std::string> stages() const {
        std::vector<std::string> texts;
        for (const auto& stage : clipboard.inner.stages) {
            texts.push_back(stage.text);
        }
        return texts;
    }
    std::size_t pastes() const { return keys.sent().size(); }
    std::vector<std::string> statuses() const { return trace.with_prefix("status:"); }
    std::vector<std::string> opened_links() const {
        std::lock_guard lock(mutex);
        return opened;
    }

    testkit::TempDataRoot root;
    dt::Trace trace;
    MemorySettingsStore settings;
    dt::ScriptedRecorder recorder;
    dt::InlineExecutor notify;
    std::shared_ptr<speech::FakeEngine> engine;
    std::unique_ptr<speech::SpeechService> speech;
    std::unique_ptr<Database> database;
    dt::TracingSync sync;
    std::unique_ptr<HistoryManager> history;
    std::unique_ptr<dictation_stats::StatsRepository> stats;
    dt::TracingClipboard clipboard;
    focus::FakeSyntheticKeys keys;
    dt::AskedCapture capture;
    ActivityState activity;
    dt::HoldableExecutor ui;
    ManualExecutor jobs;
    ManualExecutor persistence;
    EventBus bus;
    dt::FakeCleaner cleaner;
    dt::FakeRewriter rewriter;

    ManualExecutor action_ui;
    BridgeHost host;
    CredentialStore credentials;
    FilterLeases leases;
    std::shared_ptr<http::Client> http = automation::test::loopback_client();
    llm::FakeJudgeProvider provider;
    std::shared_ptr<llm::FakeJudge> judge = std::make_shared<llm::FakeJudge>();
    automation::test::Swappable<RunOnceFn> run_once;
    mutable std::mutex mutex;
    std::vector<std::pair<std::string, std::thread::id>> stdins;
    std::vector<std::string> opened;
    std::unique_ptr<ActionRuntime> actions;
    std::unique_ptr<DictationAutomation> automation;
    std::unique_ptr<dictation::TranscriptionRuntime> runtime;
    std::vector<Subscription> links;
};

std::shared_ptr<const DictationJob> job_of(JobMode mode, std::string action_id = {},
                                           std::string source = {}, std::string route = {}) {
    return dt::make_job(mode, std::move(action_id), std::move(source), std::move(route));
}

} // namespace

// -- the port itself ------------------------------------------------------------------------------

TEST_CASE("the bridge answers the pipeline's questions from the saved actions",
          "[dictation_bridge]") {
    Bridge b;
    const Json settings{
        {"actions", Json::array({dt::action_json("ch", "Tell my session", "channel"),
                                 dt::action_json("l", "Link", "open_link",
                                                 Json{{"template", "https://x.test/{text}"}}, "raw",
                                                 false, "ctrl+alt+l")})}};

    const auto found = b.automation->find_action(settings, "l");
    REQUIRE(found);
    CHECK(found->id == "l");
    CHECK(found->kind == "open_link");
    CHECK(found->shaping == "raw");
    CHECK_FALSE(found->enabled);
    CHECK_FALSE(b.automation->find_action(settings, "gone"));

    CHECK_FALSE(
        b.automation->program_words_allowed(*b.automation->find_action(settings, "ch"), settings));
    CHECK(b.automation->program_words_allowed(*found, settings));
    Json allowed = settings;
    allowed["api_agent_prompts"] = true;
    CHECK(b.automation->program_words_allowed(*b.automation->find_action(allowed, "ch"), allowed));

    const auto shortcuts = b.automation->action_hotkeys(settings);
    REQUIRE(shortcuts.size() == 2);
    CHECK(shortcuts[1].id == "l");
    CHECK(shortcuts[1].name == "Link");
    CHECK(shortcuts[1].hotkey == load_actions(settings)[1].hotkey);
    CHECK_FALSE(shortcuts[1].hotkey.empty());
    CHECK_FALSE(shortcuts[1].enabled);
    CHECK(b.automation->accepting());
    b.actions->close();
    CHECK_FALSE(b.automation->accepting());
}

TEST_CASE("the bridge takes the spec's four arguments and makes its own http client",
          "[dictation_bridge]") {
    Bridge b;
    // DictationAutomation(ActionRuntime&, FilterLeases&, llm::JudgeProvider&, EventSink&).
    DictationAutomation automation(*b.actions, b.leases, b.provider, b.bus);
    const Json settings{{"actions", Json::array({dt::action_json("ch", "Tell", "channel")})}};

    CHECK(automation.find_action(settings, "ch"));
    CHECK(automation.accepting());
    DictationJob job;
    job.mode = JobMode::Dictation;
    job.source = "hotkey";
    const auto stage = automation.filter_stage(dictation::BEFORE_CLEANUP, settings, {});
    REQUIRE(stage);
    const dictation::FilterResult kept = stage->apply("some words", job);
    CHECK(kept.text == "some words");
    CHECK_FALSE(kept.changed);
    CHECK(b.events(catalog::FILTER_FAILED).empty());
}

TEST_CASE("the bridge routes through the Fast judgments judge and says why it could not",
          "[dictation_bridge]") {
    Bridge b;
    const Json settings{
        {"actions", Json::array({dt::action_json("ch", "Tell my session", "channel")})},
        {"voice_routing_enabled", true},
        {"typesafe_enabled", true}};
    b.provider.set_judge(nullptr); // Fast judgments without a key at this moment

    const dictation::RouteDecision decision =
        b.automation->route("tell my session hello", settings, "hotkey", {}, "0123456789abcdef");

    CHECK_FALSE(decision.action_id);
    CHECK(decision.failure == "unreachable");
    const auto decided = b.events(catalog::ROUTE_DECIDED);
    REQUIRE(decided.size() == 1);
    CHECK(decided[0]["fallback"] == true);
    CHECK(decided[0]["job_id"] == "0123456789abcdef");
}

// -- an action job from its words to its answer (test_automation_actions_pipeline.py) -----------

namespace {

Json fix_action() {
    return dt::action_json("fix", "Fix grammar", "program",
                           Json{{"program", "C:\\fix.exe"}, {"output", "paste"}}, "raw");
}

BridgeOptions fix_options() {
    BridgeOptions options;
    options.settings = Json{{"auto_paste", false},
                            {"restore_clipboard", false},
                            {"actions", Json::array({fix_action()})}};
    return options;
}

} // namespace

TEST_CASE("test_a_quick_answer_pastes_while_the_job_still_saves_its_history",
          "[test_automation_actions_pipeline]") {
    Bridge b(fix_options());
    b.run_once.set(automation::test::answers(0, "Fixed my grammar."));

    // The user's run: only theirs paste.
    const std::string job_id = b.runtime->run_action_text("fix", "fix my grammer", "ui");
    b.run_jobs(); // shaping on the jobs strand, then delivery

    REQUIRE(automation::test::wait_until(
        [&] {
            b.action_ui.run_all();
            return !b.events(catalog::ACTION_RAN).empty();
        },
        10s));
    CHECK(b.runtime->has_active_job()); // history not saved yet
    CHECK(b.persistence.queued() > 0);
    {
        std::lock_guard lock(b.mutex);
        REQUIRE(b.stdins.size() == 1);
        CHECK(b.stdins[0].first == "fix my grammer");
        CHECK(b.stdins[0].second != std::this_thread::get_id());
    }
    CHECK(b.stages() == std::vector<std::string>{"Fixed my grammar."});
    CHECK(b.pastes() == 1);
    const Json ran = b.events(catalog::ACTION_RAN).at(0);
    CHECK(ran["job_id"] == job_id);
    CHECK(ran["ok"] == true);
    CHECK(ran["output"] == "paste");

    b.run_saves();
    CHECK_FALSE(b.runtime->has_active_job());
    const auto entries = b.history->get_history();
    REQUIRE(entries.size() == 1);
    CHECK(entries[0].text == "fix my grammer");
    CHECK(entries[0].entry_kind == "action");
    CHECK(entries[0].source_name == "Action \u00b7 Fix grammar");
}

TEST_CASE("test_the_job_slot_frees_while_a_slow_action_still_runs",
          "[test_automation_actions_pipeline]") {
    Bridge b(fix_options());
    auto release = std::make_shared<automation::test::Hold>();
    b.run_once.set([release](const std::vector<std::string>&, std::string_view, double,
                             const CancelToken&, const std::map<std::string, std::string>&) {
        release->wait({}, 5s);
        return automation::test::program_result(0, "late answer");
    });

    b.runtime->run_action_text("fix", "words", "ui");
    b.run_jobs();
    b.run_saves();

    CHECK_FALSE(b.runtime->has_active_job());
    CHECK(b.host.statuses_now().back() == "Fix grammar \u00b7 working\u2026");
    release->release();
    CHECK(b.pump([&] { return b.stages() == std::vector<std::string>{"late answer"}; }));
}

TEST_CASE("test_a_programs_answer_is_shown_in_the_scratchpad_never_pasted",
          "[test_automation_actions_pipeline]") {
    Bridge b(fix_options());
    b.run_once.set(automation::test::answers(0, "Fixed my grammar."));

    b.runtime->run_action_text("fix", "fix my grammer", "api");
    b.run_jobs();
    REQUIRE(b.pump([&] { return !b.events(catalog::ACTION_RAN).empty(); }));

    CHECK(b.stages().empty());
    CHECK(b.host.copied.empty());
    CHECK(b.pastes() == 0);
    CHECK(b.host.scratchpad == std::vector<std::string>{"Fixed my grammar."});
    CHECK(b.events(catalog::ACTION_RAN).at(0)["output"] == "scratchpad");
}

// -- real filters and a real webhook (test_automation_hooks_pipeline.py) ------------------------

namespace {

Answer reply_text(std::function<std::string(const std::string&)> transform) {
    Answer answer;
    answer.reply = [transform](const ReceivedRequest& request) {
        return Json{{"text", transform(request.json()["text"].get<std::string>())}}.dump();
    };
    return answer;
}

std::string replace_all(std::string text, const std::string& from, const std::string& to) {
    for (std::size_t at = text.find(from); at != std::string::npos;
         at = text.find(from, at + to.size())) {
        text.replace(at, from.size(), to);
    }
    return text;
}

std::string title_case(const std::string& text) {
    std::string out = text;
    bool start = true;
    for (char& c : out) {
        if (std::isalpha(static_cast<unsigned char>(c))) {
            c = static_cast<char>(start ? std::toupper(static_cast<unsigned char>(c))
                                        : std::tolower(static_cast<unsigned char>(c)));
            start = false;
        } else {
            start = true;
        }
    }
    return out;
}

Json http_filter(const std::string& stage, const std::string& url, Json extra = Json::object()) {
    Json row{{"id", stage}, {"name", stage}, {"kind", "http"}, {"stage", stage}, {"url", url}};
    for (const auto& [key, value] : extra.items()) {
        row[key] = value;
    }
    return row;
}

} // namespace

TEST_CASE("test_filters_change_the_text_on_both_sides_of_cleanup",
          "[test_automation_hooks_pipeline][platform]") {
    Receiver receiver;
    receiver.answer("/before", {reply_text([](const std::string& text) {
                        return replace_all(text, "claude", "Claude Code");
                    })});
    receiver.answer("/after",
                    {reply_text([](const std::string& text) { return text + " (via filter)"; })});
    Bridge b;
    b.settings.update(Json{{"automation_filters",
                            Json::array({http_filter("before_cleanup", receiver.url("/before")),
                                         http_filter("after_cleanup", receiver.url("/after"))})}});
    const auto job = job_of(JobMode::Dictation);
    b.dictate(job);

    const auto before = receiver.at("/before");
    const auto after = receiver.at("/after");
    REQUIRE(before.size() == 1);
    REQUIRE(after.size() == 1);
    CHECK(before[0].json()["text"] == "ask claude what time it is");
    CHECK(after[0].json()["text"] == "ASK CLAUDE CODE WHAT TIME IT IS");
    CHECK(before[0].json()["job_id"] == job->id);
    CHECK(after[0].json()["job_id"] == job->id);
    CHECK(b.stages() == std::vector<std::string>{"ASK CLAUDE CODE WHAT TIME IT IS (via filter)"});
    CHECK(b.pastes() == 1);
    const auto completed = b.events(catalog::TRANSCRIPTION_COMPLETED);
    REQUIRE(completed.size() == 1);
    CHECK(completed[0]["text"] == "ASK CLAUDE CODE WHAT TIME IT IS (via filter)");
}

TEST_CASE("test_an_action_job_is_filtered_too", "[test_automation_hooks_pipeline][platform]") {
    Receiver receiver;
    receiver.answer("/before", {reply_text(title_case)});
    Bridge b;
    b.settings.update(
        Json{{"automation_filters",
              Json::array({http_filter("before_cleanup", receiver.url("/before"))})}});
    b.dictate(job_of(JobMode::Action, "a1"));

    REQUIRE(receiver.at("/before").size() == 1);
    CHECK(receiver.at("/before")[0].json()["mode"] == "action");
    // The action (Ask Claude, a link) got the filtered words, cleaned.
    REQUIRE(b.pump([&] { return !b.opened_links().empty(); }));
    CHECK(b.opened_links() == std::vector<std::string>{
                                  "claude://claude.ai/new?q=ASK%20CLAUDE%20WHAT%20TIME%20IT%20IS"});
}

TEST_CASE("test_a_filter_cancel_saves_the_words_and_pastes_nothing",
          "[test_automation_hooks_pipeline][platform]") {
    Receiver receiver;
    receiver.answer("/after", {Answer{.body = R"({"cancel": true})"}});
    Bridge b;
    b.settings.update(Json{{"automation_filters",
                            Json::array({http_filter("after_cleanup", receiver.url("/after"))})}});
    b.dictate(job_of(JobMode::Dictation));

    CHECK(b.pastes() == 0);
    const auto statuses = b.statuses();
    CHECK(std::ranges::find(statuses, dictation::FILTER_CANCELED_MESSAGE) != statuses.end());
    const auto entries = b.history->get_history();
    REQUIRE(entries.size() == 1);
    CHECK(entries[0].text == "ASK CLAUDE WHAT TIME IT IS");
}

TEST_CASE("test_a_stalled_filter_costs_its_time_limit_and_keeps_the_text",
          "[test_automation_hooks_pipeline][platform]") {
    Receiver receiver;
    receiver.answer("/before", {Answer{.delay = 10s}});
    Bridge b;
    b.settings.update(Json{
        {"automation_filters", Json::array({http_filter("before_cleanup", receiver.url("/before"),
                                                        Json{{"timeout_ms", 400}})})}});
    const auto started = std::chrono::steady_clock::now();
    b.dictate(job_of(JobMode::Dictation));

    CHECK(automation::test::seconds_since(started) < 1.5);
    CHECK(b.stages() == std::vector<std::string>{"ASK CLAUDE WHAT TIME IT IS"});
    const auto failed = b.events(catalog::FILTER_FAILED);
    REQUIRE(failed.size() == 1);
    CHECK(failed[0] == Json{{"filter_id", "before_cleanup"},
                            {"name", "before_cleanup"},
                            {"stage", "before_cleanup"},
                            {"reason", "No answer within 0.4 s"}});
}

TEST_CASE("test_a_webhook_receives_the_completed_dictation",
          "[test_automation_hooks_pipeline][platform]") {
    Receiver receiver;
    Bridge b;
    b.settings.update(Json{
        {"automation_webhooks",
         Json::array({Json{{"id", "w"},
                           {"name", "Notes"},
                           {"url", receiver.url("/hook")},
                           {"events", Json::array({"transcription.completed", "history.saved"})},
                           {"enabled", true}}})}});
    WebhookEngine webhooks(b.settings, b.credentials, b.http, b.bus);
    webhooks.start();
    const auto job = job_of(JobMode::Dictation);
    std::vector<ReceivedRequest> got;
    try {
        b.dictate(job);
        got = receiver.wait_for(2, "/hook");
    } catch (...) {
        webhooks.stop();
        throw;
    }
    webhooks.stop();
    const std::string secret = webhooks.ensure_secret("w");

    REQUIRE(got.size() == 2);
    CHECK(got[0].json()["type"] == "transcription.completed");
    CHECK(got[1].json()["type"] == "history.saved");
    for (const auto& request : got) {
        CHECK(signing::verify(secret,
                              signing::Received(request.headers.begin(), request.headers.end()),
                              request.body));
    }
    CHECK(got[0].json()["data"]["text"] == "ASK CLAUDE WHAT TIME IT IS");
    CHECK(got[0].json()["data"]["job_id"] == job->id);
    CHECK(got[1].json()["data"]["job_id"] == job->id);
}

// -- leased filters run once routing has decided (test_automation_r3_lease_routing.py) ----------

namespace {

Json channel_row() {
    Json row = dt::action_json("ch", "Tell my session", "channel");
    row["spoken_names"] = Json::array({"Claude"});
    return row;
}

// route_when_claude_is_named: the judge routes words starting with "claude" to the channel, with
// the words after the comma, and nothing else.
void route_when_claude_is_named(Bridge& b) {
    b.judge->set_handler([](const Json& state, const Json& questions) -> std::optional<Json> {
        const std::string text = state.value("dictation", "");
        const bool named = py::lower(text).starts_with("claude");
        const std::string choice = named ? "ch" : std::string(ROUTE_NONE);
        Json probabilities = Json::object();
        for (const auto& [key, value] : questions.at("route").at("criteria").items()) {
            probabilities[key] = key == choice ? 0.9 : 0.1;
        }
        Json answers{{"route", Json{{"type", "choice"},
                                    {"choice", choice},
                                    {"confidence", 0.9},
                                    {"probabilities", probabilities}}}};
        if (questions.contains("payload")) {
            const std::string wanted = py::strip(text.substr(text.find(',') + 1));
            std::string picked;
            Json weights = Json::object();
            for (const auto& [key, value] : questions.at("payload").at("criteria").items()) {
                if (picked.empty() && key == wanted) {
                    picked = key;
                }
            }
            for (const auto& [key, value] : questions.at("payload").at("criteria").items()) {
                if (picked.empty()) {
                    picked = key;
                }
                weights[key] = key == picked ? 0.9 : 0.1;
            }
            answers["payload"] = Json{{"type", "choice"},
                                      {"choice", picked},
                                      {"confidence", 0.9},
                                      {"probabilities", weights}};
        }
        return answers;
    });
}

// The pipe: the channel action, a channel session listening, leases allowed, Route by voice on.
struct Pipe {
    explicit Pipe(const std::string& heard)
        : b([&] {
              BridgeOptions options;
              options.heard = heard;
              options.settings = Json{{"auto_paste", true},
                                      {"restore_clipboard", false},
                                      {"transcript_cleanup_enabled", true},
                                      {"actions", Json::array({channel_row()})},
                                      {"api_text_filters", true},
                                      {"voice_routing_enabled", true},
                                      {"voice_routing_on_dictation", true},
                                      {"typesafe_enabled", true}};
              return options;
          }()) {
        listener = b.bus.subscribe([](const Event&) {},
                                   std::vector<std::string>{std::string(catalog::CHANNEL_MESSAGE)},
                                   std::string(runtime::CHANNEL_LISTENER_LABEL));
        route_when_claude_is_named(b);
    }
    ~Pipe() { listener->close(); }

    // run: the dictation through the pipeline; the action.ran its delivery published.
    Json run(std::shared_ptr<const DictationJob> job) {
        b.dictate(std::move(job));
        REQUIRE(b.pump([&] { return !b.events(catalog::ACTION_RAN).empty(); }));
        return b.events(catalog::ACTION_RAN).back();
    }
    void lease(Receiver& receiver, std::function<std::string(const std::string&)> transform) {
        receiver.answer("/lease", {reply_text(std::move(transform))});
        b.leases.register_lease(receiver.url("/lease"), "before_cleanup");
    }

    Bridge b;
    std::shared_ptr<EventSubscription> listener;
};

std::string capitalizes(const std::string& text) {
    return replace_all(text, "claude", "Claude");
}

} // namespace

TEST_CASE("test_a_lease_never_sees_the_users_words_routed_to_claude",
          "[test_automation_r3_lease_routing][platform]") {
    const std::string route = GENERATE("hotkey", "auto");
    Receiver receiver;
    Pipe pipe("claude, what time is it");
    pipe.lease(receiver, capitalizes);

    const Json ran = pipe.run(job_of(JobMode::Dictation, "", "hotkey", route));

    // Routed to the channel action, which takes no program's words without "Send words to my
    // coding agents": the lease never saw them.
    CHECK(receiver.requests().empty());
    CHECK(ran["action_id"] == "ch");
    CHECK(ran["ok"] == true);
    const auto messages = pipe.b.events(catalog::CHANNEL_MESSAGE);
    REQUIRE(messages.size() == 1);
    CHECK(messages[0]["text"] == "WHAT TIME IS IT");
    CHECK(messages[0]["origin"] == "dictated");
    CHECK(pipe.b.host.copied.empty());
}

TEST_CASE("test_with_the_permission_a_lease_sees_the_job_as_routed",
          "[test_automation_r3_lease_routing][platform]") {
    Receiver receiver;
    Pipe pipe("claude, what time is it");
    pipe.b.settings.save_setting("api_agent_prompts", true);
    pipe.lease(receiver, [](const std::string& text) { return text + " please"; });

    CHECK(pipe.run(job_of(JobMode::Dictation, "", "hotkey", "hotkey"))["ok"] == true);

    const auto requests = receiver.requests();
    REQUIRE(requests.size() == 1);
    const Json request = requests[0].json();
    CHECK(request["mode"] == "action");
    CHECK(request["action_id"] == "ch");
    // The words after the name, as routing proposes the span (with its first letter capitalized).
    CHECK(py::lower(request["text"].get<std::string>()) == "what time is it");
    const auto messages = pipe.b.events(catalog::CHANNEL_MESSAGE);
    REQUIRE(messages.size() == 1);
    CHECK(messages[0]["text"] == "WHAT TIME IS IT PLEASE");
    CHECK(messages[0]["origin"] == "program");
}

TEST_CASE("test_the_users_saved_filters_still_run_before_routing",
          "[test_automation_r3_lease_routing][platform]") {
    // A saved filter fixing a mishearing decides where the words go, as before.
    Receiver receiver;
    Pipe pipe("cloud, what time is it");
    receiver.answer("/mine", {reply_text([](const std::string& text) {
                        return replace_all(text, "cloud", "claude");
                    })});
    pipe.b.settings.save_setting("automation_filters",
                                 Json::array({Json{{"id", "mine"},
                                                   {"name", "Filter mine"},
                                                   {"kind", "http"},
                                                   {"stage", "before_cleanup"},
                                                   {"url", receiver.url("/mine")}}}));
    pipe.lease(receiver, capitalizes);

    const Json ran = pipe.run(job_of(JobMode::Dictation, "", "hotkey", "hotkey"));

    std::vector<std::string> paths;
    for (const auto& request : receiver.requests()) {
        paths.push_back(request.path);
    }
    CHECK(paths == std::vector<std::string>{"/mine"});
    CHECK(ran["action_id"] == "ch");
    CHECK(ran["ok"] == true);
    const auto messages = pipe.b.events(catalog::CHANNEL_MESSAGE);
    REQUIRE(messages.size() == 1);
    CHECK(messages[0]["origin"] == "dictated");
}

namespace {

// A stage whose saved filter "mine" is a program that answers "keep" after `mine_delay` (run_once
// replaced, so no process or socket adds to its time), and whose lease on a receiver answers
// "keep" at once.
struct Stage {
    explicit Stage(std::chrono::milliseconds mine_delay) {
        receiver.answer("/lease", {Answer{.body = "{}"}});
        leases.register_lease(receiver.url("/lease"), "before_cleanup");
        settings = Json{
            {"automation_filters", Json::array({Json{{"id", "mine"},
                                                     {"name", "Filter mine"},
                                                     {"kind", "program"},
                                                     {"stage", "before_cleanup"},
                                                     {"program", automation::test::hook_child()},
                                                     {"args", Json::array()},
                                                     {"timeout_ms", 5000}}})},
            {"api_text_filters", true}};
        job.mode = JobMode::Dictation;
        job.source = "hotkey";
        run_once = [this, mine_delay](const std::vector<std::string>&, std::string_view, double,
                                      const CancelToken& cancel) {
            ++mine_runs;
            cancel.wait_for(mine_delay);
            return automation::test::program_result(0, "{}");
        };
    }
    ~Stage() { bus.close(); }

    std::unique_ptr<StageRun> run() {
        return std::make_unique<StageRun>("before_cleanup",
                                          std::optional<Json>(std::in_place, settings),
                                          FilterContext{leases, http, bus, run_once});
    }
    std::vector<Json> failures() const {
        std::vector<Json> found;
        for (const Event& event :
             bus.recent(50, std::vector<std::string>{std::string(catalog::FILTER_FAILED)})) {
            found.push_back(event.data);
        }
        return found;
    }

    Receiver receiver;
    FilterLeases leases;
    std::shared_ptr<http::Client> http = automation::test::loopback_client();
    EventBus bus;
    Json settings;
    DictationJob job;
    std::atomic<int> mine_runs{0};
    FilterRunOnce run_once;
};

} // namespace

TEST_CASE("test_routing_between_the_parts_takes_none_of_the_filters_time",
          "[test_automation_r3_lease_routing][platform]") {
    Stage stage(0ms);
    const auto run = stage.run();
    run->saved("some words", stage.job);
    // Routing waits longer than the whole stage's budget.
    std::this_thread::sleep_for(
        std::chrono::milliseconds(static_cast<int>(filters::STAGE_BUDGET_S * 1000) + 300));
    run->leased("some words", stage.job);

    CHECK(stage.mine_runs.load() == 1);
    CHECK(stage.receiver.arrivals() == std::vector<std::string>{"/lease"});
    CHECK(stage.failures().empty());
}

TEST_CASE("test_the_parts_share_the_stages_time", "[test_automation_r3_lease_routing][platform]") {
    // The saved filter answers 30 ms before the stage's 3 s run out, less than a filter needs.
    Stage stage(std::chrono::milliseconds(static_cast<int>(filters::STAGE_BUDGET_S * 1000) - 30));
    const auto run = stage.run();
    run->saved("some words", stage.job);
    run->leased("some words", stage.job);

    CHECK(stage.mine_runs.load() == 1);
    CHECK(stage.receiver.arrivals().empty());
    REQUIRE(automation::test::wait_until([&] { return stage.failures().size() == 1; }, 5s));
    CHECK(stage.failures()[0]["reason"] == "Skipped: this stage's 3 s for filters ran out");
}
// -- acceptance ----------------------------------------------------------------------------------

TEST_CASE("Home, turn on the porch light: a routed dictation becomes a signed webhook POST",
          "[dictation_bridge][platform]") {
    Receiver receiver;
    BridgeOptions options;
    options.heard = "Home, turn on the porch light";
    Json webhook = dt::action_json("hook1", "Webhook", "webhook",
                                   Json{{"url", receiver.url("/hook")}, {"output", "notice"}});
    webhook["spoken_names"] = Json::array({"Home"});
    options.settings = Json{{"auto_paste", true},
                            {"restore_clipboard", false},
                            {"transcript_cleanup_enabled", true},
                            {"actions", Json::array({webhook})},
                            {"voice_routing_enabled", true},
                            {"typesafe_enabled", true}};
    Bridge b(options);
    // The cleaner writes the words as a sentence.
    b.cleaner.answer([](const std::string& text) {
        std::string sentence = text;
        if (!sentence.empty()) {
            sentence[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(sentence[0])));
        }
        return sentence;
    });
    // The judge chooses the webhook action and the words after its name.
    b.judge->set_handler([](const Json&, const Json& questions) -> std::optional<Json> {
        Json probabilities = Json::object();
        for (const auto& [key, value] : questions.at("route").at("criteria").items()) {
            probabilities[key] = key == "hook1" ? 0.95 : 0.05;
        }
        Json answers{{"route", Json{{"type", "choice"},
                                    {"choice", "hook1"},
                                    {"confidence", 0.95},
                                    {"probabilities", probabilities}}}};
        if (questions.contains("payload")) {
            std::string picked;
            Json weights = Json::object();
            for (const auto& [key, value] : questions.at("payload").at("criteria").items()) {
                if (picked.empty() && key == "turn on the porch light") {
                    picked = key;
                }
            }
            for (const auto& [key, value] : questions.at("payload").at("criteria").items()) {
                if (picked.empty()) {
                    picked = key;
                }
                weights[key] = key == picked ? 0.9 : 0.01;
            }
            answers["payload"] = Json{{"type", "choice"},
                                      {"choice", picked},
                                      {"confidence", 0.9},
                                      {"probabilities", weights}};
        }
        return answers;
    });
    receiver.answer("/hook", {Answer{.status = 204}});

    const auto job = job_of(JobMode::Dictation, "", "hotkey", "hotkey");
    b.dictate(job);
    const auto got = receiver.wait_for(1, "/hook");
    REQUIRE(b.pump([&] { return !b.events(catalog::ACTION_RAN).empty(); }));

    const std::string secret = ensure_action_secret(b.credentials, "hook1");
    CHECK(signing::verify(secret, signing::Received(got[0].headers.begin(), got[0].headers.end()),
                          got[0].body));
    const Json body = got[0].json();
    CHECK(body["v"] == 1);
    CHECK(body["action"] == Json{{"id", "hook1"}, {"name", "Webhook"}});
    CHECK(body["text"] == "Turn on the porch light");
    CHECK(body["job_id"] == job->id);
    CHECK(b.pastes() == 0);
    CHECK(b.host.statuses_now() ==
          std::vector<std::string>{"Webhook \u00b7 working\u2026",
                                   "Sent to Webhook \u2014 nothing came back"});
    const auto decided = b.events(catalog::ROUTE_DECIDED);
    REQUIRE(decided.size() == 1);
    CHECK(decided[0]["action_id"] == "hook1");
    CHECK(decided[0]["fallback"] == false);
}
