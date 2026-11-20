#pragma once

// The harness of OpenWhisper's automation pipeline tests (tests/test_automation_foundation_
// pipeline.py `h`): the real TranscriptionRuntime over a scripted recorder, the real speech
// service on a fake engine, History on a temporary database, a cleaner that upper-cases (so a
// test sees whether cleanup ran), and a fake AutomationPort standing in for the action runtime,
// the text filters and Route by voice's judge. Saved actions live in the settings, as in
// Python, and the fake port reads them from there.

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <deque>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "cleanup_support.h"
#include "dictation/automation_port.h"
#include "dictation/command_selection.h"
#include "dictation/rewrite_jobs.h"
#include "runtime_support.h"
#include "settings/keys.h"

namespace fv::dictation::testing {

// An action as the settings store it (Action.to_dict).
inline Json action_json(std::string id, std::string name, std::string kind,
                        Json params = Json::object(), std::string shaping = "cleanup",
                        bool enabled = true, std::string hotkey = "") {
    return Json{{"id", std::move(id)},
                {"name", std::move(name)},
                {"kind", std::move(kind)},
                {"params", std::move(params)},
                {"hotkey", std::move(hotkey)},
                {"shaping", std::move(shaping)},
                {"spoken_names", Json::array()},
                {"description", ""},
                {"enabled", enabled}};
}

// The foundation's two actions.
inline Json ask_action() {
    return action_json("a1", "Ask Claude", "open_link",
                       Json{{"template", "claude://claude.ai/new?q={text}"}});
}
inline Json raw_action() {
    return action_json("a2", "Raw notes", "scripts", Json::object(), "raw");
}

// Runs a posted task at once on the posting thread, as Qt's direct slot calls do, until the test
// holds it: then tasks wait until release() (a worker's result still on its way to the Qt
// thread). Timers wait for fire_timers().
// Affinity: thread-safe.
class HoldableExecutor final : public Executor {
public:
    void post(Task task) override {
        {
            std::lock_guard lock(mutex_);
            if (held_) {
                queued_.push_back(std::move(task));
                return;
            }
        }
        task();
    }
    TimerHandle post_after(std::chrono::milliseconds delay, Task task) override {
        auto state = std::make_shared<TimerState>();
        std::lock_guard lock(mutex_);
        timers_.push_back({delay, state, std::move(task)});
        return TimerHandle(state);
    }
    bool is_current() const override { return true; }

    void hold() {
        std::lock_guard lock(mutex_);
        held_ = true;
    }
    // Runs later tasks at once again, and hands back what waited, in order, for the test to run
    // when it chooses.
    std::deque<Task> take() {
        std::deque<Task> due;
        std::lock_guard lock(mutex_);
        held_ = false;
        due.swap(queued_);
        return due;
    }
    std::size_t fire_timers() {
        std::vector<Timer> due;
        {
            std::lock_guard lock(mutex_);
            due.swap(timers_);
        }
        std::size_t count = 0;
        for (auto& timer : due) {
            if (timer.state->fire()) {
                timer.task();
                ++count;
            }
        }
        return count;
    }

private:
    struct Timer {
        std::chrono::milliseconds delay;
        std::shared_ptr<TimerState> state;
        Task task;
    };
    std::mutex mutex_;
    bool held_ = false;
    std::deque<Task> queued_;
    std::vector<Timer> timers_;
};

// The recorder surfaces the Python tests script (their Recorder classes): a start opens a
// capture, a stop keeps recording through its post-roll until the worker waits it out (unless
// stop_ends_capture), and a canceled capture still reads as recording until its stream closes
// (closed()) unless cancel_ends_capture. A save copies a real WAV the speech service decodes.
// Affinity: thread-safe for what the runtime calls; set the knobs before use.
class ScriptedRecorder final : public RecorderPort {
public:
    ScriptedRecorder(std::filesystem::path output, Trace& trace)
        : output_(std::move(output)), trace_(trace) {}

    bool is_recording() const override { return recording.load(); }
    bool recorder_busy() const override { return false; }
    bool capture_canceled() const override { return canceled.load(); }
    bool stream_open() const override { return recording.load(); }

    bool start_recording() override {
        trace_.add("recorder_start");
        recording = true;
        canceled = false;
        if (on_start) {
            // The device is opening: the capture already counts as recording.
            on_start();
        }
        recording = start_ok.load();
        return start_ok.load();
    }
    std::string last_start_error() const override { return "No audio device available"; }
    bool stop_recording() override {
        trace_.add("recorder_stop");
        if (stop_ends_capture) {
            recording = false;
        }
        return true;
    }
    bool wait_for_stop_completion() override {
        trace_.add("waited");
        recording = false;
        return true;
    }
    bool has_recording_data() const override { return true; }
    bool save_recording() override {
        trace_.add("saved");
        std::filesystem::copy_file(audio_fixture("tone-16k-mono.wav"), output_,
                                   std::filesystem::copy_options::overwrite_existing);
        return true;
    }
    double get_recording_duration() const override { return 2.5; }
    std::string last_capture_error() const override { return {}; }
    void fail_capture(const std::string& message) override {
        trace_.add("fail_capture:" + message);
    }
    void cancel_recording() override {
        trace_.add("recorder_cancel");
        canceled = true;
        if (cancel_ends_capture) {
            recording = false;
        }
    }
    void acknowledge_recording() override {
        trace_.add("acknowledged");
        ++acknowledged;
    }
    const std::filesystem::path& output_file() const override { return output_; }
    Signal<std::string>& capture_error() override { return capture_error_; }

    // The canceled capture's stream closed.
    void closed() { recording = false; }

    std::atomic<bool> recording{false};
    std::atomic<bool> canceled{false};
    std::atomic<bool> start_ok{true};
    std::atomic<int> acknowledged{0};
    bool stop_ends_capture = false;
    bool cancel_ends_capture = false;
    // Runs inside start_recording, while the device opens.
    std::function<void()> on_start;

private:
    std::filesystem::path output_;
    Trace& trace_;
    Signal<std::string> capture_error_;
};

// Route by voice's judge, the text filters and the action runtime, as the Python tests
// monkeypatch routing.decide, filters.apply and filters.StageRun and mock action_runtime.
// Affinity: thread-safe for what the runtime calls; set the knobs before use.
class FakeAutomationPort final : public AutomationPort {
public:
    // One filter call: "before_cleanup saved", "before_cleanup leased", or the stage alone for a
    // whole stage (filters.apply).
    struct FilterCall {
        std::string stage;
        std::string text;
        DictationJob job;
        CancelToken cancel;
    };
    struct RouteCall {
        std::string text;
        std::string route;
        std::string job_id;
        CancelToken cancel;
    };
    struct Delivery {
        std::shared_ptr<const DictationJob> job;
        ActionDelivery delivery;
    };
    using Filter = std::function<FilterResult(const std::string& stage, const std::string& text,
                                              const DictationJob& job)>;
    using Decide = std::function<RouteDecision(const std::string& text, const std::string& route)>;

    std::unique_ptr<FilterStageRun> filter_stage(std::string_view stage, const Json& settings,
                                                 const CancelToken& cancel) override {
        (void)settings;
        if (!filter) {
            return nullptr;
        }
        return std::make_unique<Stage>(*this, std::string(stage), cancel);
    }
    RouteDecision route(std::string_view text, const Json& settings, std::string_view route,
                        const CancelToken& cancel, std::string_view job_id) override {
        (void)settings;
        {
            std::lock_guard lock(mutex_);
            route_calls_.push_back(
                {std::string(text), std::string(route), std::string(job_id), cancel});
        }
        return decide ? decide(std::string(text), std::string(route)) : RouteDecision{};
    }
    std::optional<RecordingAction> find_action(const Json& settings, std::string_view id) override {
        const Json actions = settings.value("actions", Json::array());
        for (const Json& row : actions) {
            if (row.value("id", "") != id) {
                continue;
            }
            RecordingAction action;
            action.id = row.value("id", "");
            action.name = row.value("name", "");
            action.kind = row.value("kind", "");
            action.params = row.value("params", Json::object());
            action.shaping = row.value("shaping", "cleanup");
            action.enabled = row.value("enabled", true);
            return action;
        }
        return std::nullopt;
    }
    // runtime.program_words_allowed: agent and channel actions need api_agent_prompts.
    bool program_words_allowed(const RecordingAction& action, const Json& settings) override {
        return (action.kind != "agent" && action.kind != "channel") ||
               keys::api_agent_prompts.read(settings);
    }
    bool accepting() override { return taking.load(); }
    void deliver(std::shared_ptr<const DictationJob> job, ActionDelivery delivery) override {
        std::lock_guard lock(mutex_);
        deliveries_.push_back({std::move(job), std::move(delivery)});
    }
    std::vector<ActionShortcut> action_hotkeys(const Json& settings) override {
        std::vector<ActionShortcut> shortcuts;
        for (const Json& row : settings.value("actions", Json::array())) {
            shortcuts.push_back({row.value("id", ""), row.value("name", ""),
                                 row.value("hotkey", ""), row.value("enabled", true)});
        }
        return shortcuts;
    }

    std::vector<FilterCall> filter_calls() const {
        std::lock_guard lock(mutex_);
        return filter_calls_;
    }
    std::vector<RouteCall> route_calls() const {
        std::lock_guard lock(mutex_);
        return route_calls_;
    }
    std::vector<Delivery> deliveries() const {
        std::lock_guard lock(mutex_);
        return deliveries_;
    }

    Filter filter;
    Decide decide;
    std::atomic<bool> taking{true};

private:
    class Stage final : public FilterStageRun {
    public:
        Stage(FakeAutomationPort& port, std::string stage, CancelToken cancel)
            : port_(port), stage_(std::move(stage)), cancel_(std::move(cancel)) {}
        FilterResult saved(std::string text, const DictationJob& job) override {
            return port_.run(stage_ + " saved", text, job, cancel_);
        }
        FilterResult leased(std::string text, const DictationJob& job) override {
            return port_.run(stage_ + " leased", text, job, cancel_);
        }
        FilterResult apply(std::string text, const DictationJob& job) override {
            return port_.run(stage_, text, job, cancel_);
        }

    private:
        FakeAutomationPort& port_;
        std::string stage_;
        CancelToken cancel_;
    };

    FilterResult run(const std::string& stage, const std::string& text, const DictationJob& job,
                     const CancelToken& cancel) {
        {
            std::lock_guard lock(mutex_);
            filter_calls_.push_back({stage, text, job, cancel});
        }
        return filter(stage, text, job);
    }

    mutable std::mutex mutex_;
    std::vector<FilterCall> filter_calls_;
    std::vector<RouteCall> route_calls_;
    std::vector<Delivery> deliveries_;
};

// CommandRuntime's rewrite_text and complete_recording, as the Python tests monkeypatch them.
// Affinity: thread-safe.
class FakeRewriter final : public CommandCompletion {
public:
    RewriteResult complete_recording(const std::string& raw,
                                     const std::shared_ptr<const DictationJob>& job) override {
        (void)job;
        if (refuse_recording) {
            throw CommandRefused(*refuse_recording);
        }
        return RewriteResult{raw, std::nullopt, std::nullopt};
    }
    RewriteResult rewrite_text(const std::string& text, const std::string& instruction) override {
        {
            std::lock_guard lock(mutex_);
            calls_.emplace_back(text, instruction);
        }
        if (rewrite) {
            return rewrite(text, instruction);
        }
        return RewriteResult{text, std::nullopt, std::nullopt};
    }
    std::vector<std::pair<std::string, std::string>> calls() const {
        std::lock_guard lock(mutex_);
        return calls_;
    }

    std::function<RewriteResult(const std::string& text, const std::string& instruction)> rewrite;
    std::optional<std::string> refuse_recording;

private:
    mutable std::mutex mutex_;
    std::vector<std::pair<std::string, std::string>> calls_;
};

// A capture service that records what each capture asked for and answers at once with
// `snapshot(include_text, include_selection)` (an empty snapshot when unset).
// Affinity: thread-safe.
class AskedCapture final : public focus::CaptureService {
public:
    std::shared_ptr<focus::FocusRequest> request(bool include_text,
                                                 bool include_selection) override {
        {
            std::lock_guard lock(mutex_);
            asked_.emplace_back(include_text, include_selection);
        }
        return focus::FocusRequest::resolved(snapshot ? snapshot(include_text, include_selection)
                                                      : focus::FocusSnapshot{});
    }
    std::optional<focus::AppIdentity> current_identity() override { return identity; }
    void reread(const std::optional<focus::AppIdentity>&,
                std::function<void(std::optional<focus::TextContext>)> callback) override {
        callback(std::nullopt);
    }
    void shutdown() override {}

    std::vector<std::pair<bool, bool>> asked() const {
        std::lock_guard lock(mutex_);
        return asked_;
    }

    std::function<focus::FocusSnapshot(bool include_text, bool include_selection)> snapshot;
    std::optional<focus::AppIdentity> identity;

private:
    mutable std::mutex mutex_;
    std::vector<std::pair<bool, bool>> asked_;
};

// The foundation's settings: auto-paste without keeping the clipboard, cleanup on, ASK and RAW.
inline Json automation_settings() {
    return Json{{"auto_paste", true},
                {"copy_clipboard", true},
                {"restore_clipboard", false},
                {"transcript_cleanup_enabled", true},
                {"actions", Json::array({ask_action(), raw_action()})}};
}

struct AutomationRigOptions {
    Json settings = automation_settings();
    std::string heard = "ask claude what time it is";
    // Another capture service than the null one.
    focus::CaptureService* capture = nullptr;
};

// h: the runtime over the fakes, in a temporary data root, with the fake port, an upper-casing
// cleaner and a fake rewriter installed.
struct AutomationRig {
    explicit AutomationRig(AutomationRigOptions options = {})
        : settings(options.settings), recorder(root.path() / "recorded_audio.wav", trace),
          engine(std::make_shared<speech::FakeEngine>(script_for(options.heard))),
          speech(std::make_unique<speech::SpeechService>(engine->factory(), notify)),
          database(std::make_unique<Database>(Database::open_or_create(root.path() / "test.db"))),
          sync(trace),
          history(std::make_unique<HistoryManager>(*database, root.path() / "recordings",
                                                   RecordingRetention{}, history_services())),
          stats(std::make_unique<dictation_stats::StatsRepository>(*database)), clipboard(trace),
          cleaner([](const std::string& text) { return upper(text); }) {
        speech->select(speech::Slot::Dictation, fake_parakeet_config());
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::minutes(2);
        while (speech->readiness(speech::Slot::Dictation).state == speech::ReadyState::Loading) {
            if (std::chrono::steady_clock::now() > deadline) {
                throw std::runtime_error("the fake engine did not load");
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        keys.on_send = [this](const std::string& what) {
            trace.add(what);
        };
        TranscriptionServices services;
        services.settings = &settings;
        services.recorder = &recorder;
        services.speech = speech.get();
        services.history = history.get();
        services.stats = stats.get();
        services.delivery.clipboard = &clipboard;
        services.delivery.keys = &keys;
        services.activity = &activity;
        services.ui = &ui;
        services.jobs = &jobs;
        services.persistence = &persistence;
        services.events = &events;
        services.capture = options.capture != nullptr ? options.capture : &null_capture;
        services.hooks.stop_playback = [] {
        };
        runtime = std::make_unique<TranscriptionRuntime>(std::move(services));
        runtime->set_automation(port);
        runtime->set_cleaner(&cleaner);
        runtime->set_command_runtime(&rewriter);
        links.push_back(runtime->status_update.connect(
            ui, [this](std::string status) { trace.add("status:" + status); }));
        links.push_back(
            runtime->overlay_state_update.connect(ui, [this](appcore::OverlayState state) {
                trace.add("overlay:" + std::string(appcore::to_string(state)));
            }));
        links.push_back(runtime->notice_requested.connect(ui, [this](std::string text, bool done) {
            trace.add("notice:" + text + (done ? " (done)" : ""));
        }));
    }
    ~AutomationRig() {
        links.clear();
        runtime.reset();
        speech.reset();
        stats.reset();
        history.reset();
        database->close();
    }
    AutomationRig(const AutomationRig&) = delete;
    AutomationRig& operator=(const AutomationRig&) = delete;

    static std::string upper(const std::string& text) {
        std::string result = text;
        for (char& c : result) {
            if (c >= 'a' && c <= 'z') {
                c = static_cast<char>(c - 'a' + 'A');
            }
        }
        return result;
    }
    static speech::FakeScript script_for(const std::string& heard) {
        speech::FakeScript script;
        script.texts = {heard};
        return script;
    }
    // What the engine hears from now on.
    void hears(const std::string& text) { engine->set_script(script_for(text)); }

    HistoryServices history_services() {
        HistoryServices services;
        services.record_sync = &sync;
        return services;
    }
    // The saved actions: ASK and RAW, then `more`.
    void with_actions(std::vector<Json> more) {
        Json actions = Json::array({ask_action(), raw_action()});
        for (auto& action : more) {
            actions.push_back(std::move(action));
        }
        settings.update(Json{{"actions", std::move(actions)}});
    }

    // The jobs and History saves queued, and what they queue, until none is left.
    void run_workers() {
        while (jobs.run_all() + persistence.run_all() > 0) {
        }
    }
    // A fixture WAV at `name` in the data root.
    std::filesystem::path wav(const std::string& name = "speech.wav") {
        const auto path = root.path() / name;
        std::filesystem::copy_file(audio_fixture("tone-16k-mono.wav"), path,
                                   std::filesystem::copy_options::overwrite_existing);
        return path;
    }
    // _claim_job(job) and transcribe_audio_file(audio): the final pass of a job holding the slot,
    // run on this thread as the Python tests call it.
    void transcribe(std::shared_ptr<const DictationJob> job) {
        REQUIRE(runtime->claim_job(std::move(job)));
        runtime->transcribe_audio_file(wav());
    }
    // The job's audio is the recorder's own WAV, as for a real recording.
    void live_recording() {
        std::filesystem::copy_file(audio_fixture("tone-16k-mono.wav"), recorder.output_file(),
                                   std::filesystem::copy_options::overwrite_existing);
        PendingAudio pending;
        pending.audio_path = recorder.output_file();
        runtime->set_pending(pending);
    }

    std::vector<std::string> statuses() const { return trace.with_prefix("status:"); }
    std::string last_status() const {
        const auto all = statuses();
        return all.empty() ? std::string() : all.back();
    }
    std::vector<std::string> notices() const { return trace.with_prefix("notice:"); }
    std::vector<std::string> overlays() const { return trace.with_prefix("overlay:"); }
    // The texts staged for a paste.
    std::vector<std::string> stages() const {
        std::vector<std::string> texts;
        for (const auto& stage : clipboard.inner.stages) {
            texts.push_back(stage.text);
        }
        return texts;
    }
    std::size_t pastes() const { return keys.sent().size(); }
    std::vector<HistoryEntry> entries() { return history->get_history(); }
    // The event types published so far, a failure with its category.
    std::vector<std::string> kinds() const {
        std::vector<std::string> found;
        for (const auto& event : events.all()) {
            if (event.type == "transcription.failed") {
                found.push_back(event.type + "(" + event.data.value("error", "") + ")");
            } else {
                found.push_back(event.type);
            }
        }
        return found;
    }

    testkit::TempDataRoot root;
    Trace trace;
    MemorySettingsStore settings;
    ScriptedRecorder recorder;
    InlineExecutor notify;
    std::shared_ptr<speech::FakeEngine> engine;
    std::unique_ptr<speech::SpeechService> speech;
    std::unique_ptr<Database> database;
    TracingSync sync;
    std::unique_ptr<HistoryManager> history;
    std::unique_ptr<dictation_stats::StatsRepository> stats;
    TracingClipboard clipboard;
    focus::FakeSyntheticKeys keys;
    focus::NullCaptureService null_capture;
    ActivityState activity;
    HoldableExecutor ui;
    ManualExecutor jobs;
    ManualExecutor persistence;
    RecordingEvents events;
    FakeCleaner cleaner;
    FakeAutomationPort port;
    FakeRewriter rewriter;
    std::unique_ptr<TranscriptionRuntime> runtime;
    std::vector<Subscription> links;
};

// A job as the Python tests build one: DictationJob(mode=..., action_id=..., source=...,
// route=...).
inline std::shared_ptr<const DictationJob> make_job(JobMode mode, std::string action_id = {},
                                                    std::string source = {},
                                                    std::string route = {}) {
    auto job = std::make_shared<DictationJob>();
    job->mode = mode;
    job->action_id = std::move(action_id);
    job->source = std::move(source);
    job->route = std::move(route);
    return job;
}

} // namespace fv::dictation::testing
