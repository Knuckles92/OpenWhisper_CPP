// The cleanup stage of the transcription runtime: tests/test_personalize_foundation_runtime.py
// (per-job state, the cleanup stage, delivery and History), tests/test_personalize_s5_pipeline.py
// (snippets through the stage), the cleanup and profile tests of
// tests/test_application_controller.py, and one test each of test_cleanup_deadline.py,
// test_cancel_after_stop.py, test_strict_switch_reads.py and test_personalize_coordinator.py.
// Where OpenWhisper calls _maybe_cleanup_transcript, the C++ test calls run_cleanup, its port;
// where it patches TranscriptCleanup's methods, FakeCleaner stands in for the cleaner, and the
// real TranscriptCleanup runs against an http::FakeHttpClient or a provider that never answers.

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <chrono>
#include <future>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "base/testing/fake_clock.h"
#include "base/utf8.h"
#include "cleanup_support.h"
#include "dictation/cleanup_prompt.h"
#include "dictation/cleanup_stage.h"
#include "dictation/delivery.h"
#include "dictation/messages.h"
#include "dictation/pipeline.h"
#include "dictation/text_preparer.h"
#include "focus/testing/fake_platform.h"
#include "pyport/text.h"
#include "runtime_support.h"
#include "settings/keys.h"
#include "settings/testing/memory_store.h"
#include "text/cleanup_config.h"
#include "text/cleanup_profiles.h"
#include "text/cleanup_prompts.h"
#include "uploads_support.h"

using namespace fv::dictation;
using namespace fv::dictation::testing;
using namespace std::chrono_literals;
using fv::Json;
using fv::focus::AppIdentity;
using fv::focus::FocusSnapshot;

namespace {

const std::string kBusy =
    "A transcription is already in progress — wait before starting another recording";

AppIdentity outlook() {
    return app("outlook.exe", "Outlook", 10, "0x1");
}

// A runtime over the fakes with FakeCleaner as its cleaner. The cleaner outlives the runtime.
struct CleanupRig {
    explicit CleanupRig(
        RigOptions options = {}, std::function<std::string(const std::string&)> answer =
                                     [](const std::string& text) { return text; })
        : cleaner(std::move(answer)), rig(std::move(options)) {
        rig.runtime->set_cleaner(&cleaner);
    }
    ~CleanupRig() { rig.runtime->set_cleaner(nullptr); }

    // Claims the slot for `job` (none: an upload's) and hands the runtime a finished
    // transcript, as the worker's emit does; History saves run.
    void complete(std::shared_ptr<const DictationJob> job, std::string transcript,
                  std::optional<std::string> raw = std::nullopt,
                  std::optional<CleanupInfo> info = std::nullopt) {
        REQUIRE(rig.runtime->claim_job(std::move(job)));
        rig.runtime->on_transcription_complete(std::move(transcript), std::move(raw),
                                               std::move(info));
        rig.run_workers();
    }
    // Claims the slot for `job` and transcribes a file in which the engine hears `heard`.
    void transcribe(std::shared_ptr<const DictationJob> job, std::string heard) {
        say(*rig.engine, std::move(heard));
        if (job) {
            REQUIRE(rig.runtime->claim_job(std::move(job)));
        }
        rig.runtime->transcribe_audio_file(rig.wav("clip.wav"));
        rig.run_workers();
    }
    std::size_t pastes() const {
        std::size_t count = 0;
        for (const auto& sent : rig.keys.sent()) {
            count += sent == "paste" ? 1 : 0;
        }
        return count;
    }
    std::optional<fv::HistoryEntry> entry_with(const std::string& text) {
        for (auto& entry : rig.entries()) {
            if (entry.text == text) {
                return entry;
            }
        }
        return std::nullopt;
    }

    FakeCleaner cleaner;
    RuntimeRig rig;
};

RigOptions with(Json extra) {
    RigOptions options;
    options.settings.update(extra);
    return options;
}

std::shared_ptr<const DictationJob> dictation() {
    return shared(job_in(std::nullopt, std::nullopt, JobMode::Dictation, false));
}

// _maybe_cleanup_transcript(raw) for `job`, through `cleaner`.
CleanupOutcome cleanup(std::string_view raw, const Json& settings, TranscriptCleaner& cleaner,
                       const DictationJob* job = nullptr,
                       std::optional<fv::text::CleanupProfile> profile = std::nullopt,
                       const TextPreparer* preparer = nullptr) {
    CleanupStageInput input;
    input.job = job;
    input.settings = settings;
    input.profile = std::move(profile);
    CleanupStageServices services;
    services.cleaner = &cleaner;
    services.preparer = preparer;
    return run_cleanup(raw, input, services);
}

std::string capitalize_and_punctuate(const std::string& text) {
    std::string out = fv::py::upper(fv::py::slice(text, 0, 1)) +
                      fv::py::rstrip(fv::py::slice(text, 1, std::nullopt), ".");
    return out + ".";
}

const fv::text::CleanupProfile& starter(std::string_view id) {
    for (const auto& profile : fv::text::starter_profiles()) {
        if (profile.id == id) {
            return profile;
        }
    }
    throw std::out_of_range("no starter profile");
}

} // namespace

// ---- tests/test_personalize_foundation_runtime.py: job lifetime ----

TEST_CASE("recording job starts before the preview and is claimed by the stop",
          "[test_personalize_foundation_runtime]") {
    // from
    // tests/test_personalize_foundation_runtime.py::test_recording_job_starts_before_the_preview_and_is_claimed_by_the_stop
    // The rig records without a live preview; the early decode starts with the job's context.
    CleanupRig c;
    auto& rig = c.rig;

    REQUIRE(rig.runtime->start_recording());
    const auto job = rig.runtime->current_job();

    REQUIRE(job);
    CHECK(job->mode == JobMode::Dictation);
    CHECK(rig.trace.has("stop_playback"));
    CHECK(rig.trace.with_prefix("overlay:").back() == "recording");
    CHECK(rig.trace.count("prefetch") == 1);
    CHECK_FALSE(rig.runtime->has_active_job());

    rig.runtime->stop_recording();
    CHECK(rig.runtime->current_job() == job);
    CHECK(rig.runtime->has_active_job());

    rig.run_workers();
    CHECK_FALSE(rig.runtime->current_job());
    CHECK_FALSE(rig.runtime->has_active_job());
}

TEST_CASE("cancel and failed start leave no job", "[test_personalize_foundation_runtime]") {
    // from tests/test_personalize_foundation_runtime.py::test_cancel_and_failed_start_leave_no_job
    CleanupRig c;
    auto& rig = c.rig;

    REQUIRE(rig.runtime->start_recording());
    rig.runtime->cancel();
    CHECK_FALSE(rig.runtime->current_job());

    rig.recorder.start_ok = false;
    CHECK_FALSE(rig.runtime->start_recording());
    CHECK_FALSE(rig.runtime->current_job());
}

TEST_CASE("a busy recording or walkthrough start shows by the pointer",
          "[test_personalize_foundation_runtime]") {
    // from
    // tests/test_personalize_foundation_runtime.py::test_a_busy_recording_or_walkthrough_start_shows_by_the_pointer
    CleanupRig c;
    auto& rig = c.rig;
    REQUIRE(rig.runtime->claim_job());

    CHECK_FALSE(rig.runtime->start_recording());
    // Refused before the session is looked at.
    CHECK_FALSE(rig.runtime->start_walkthrough(nullptr));
    rig.runtime->toggle_recording();

    CHECK_FALSE(rig.recorder.is_recording());
    CHECK(rig.notices() == std::vector<std::string>{kBusy, kBusy, kBusy});
    CHECK(rig.last_status() == kBusy);
}

TEST_CASE("a failed stop drops the job and an upload gets none",
          "[test_personalize_foundation_runtime]") {
    // from
    // tests/test_personalize_foundation_runtime.py::test_a_failed_stop_drops_the_job_and_an_upload_gets_none
    CleanupRig c;
    auto& rig = c.rig;
    REQUIRE(rig.runtime->start_recording(StartOptions{.profile_id = "email"}));
    CHECK(rig.runtime->recording_profile() == "email");
    rig.recorder.stop_ok = false;

    rig.runtime->stop_recording();
    CHECK_FALSE(rig.runtime->current_job());
    CHECK(rig.runtime->recording_profile().empty());

    REQUIRE(rig.runtime->upload_audio_file(rig.wav("upload.wav")));
    CHECK(rig.runtime->has_active_job());
    CHECK_FALSE(rig.runtime->current_job());
    CHECK(rig.runtime->recording_profile().empty());
    rig.run_workers();
}

TEST_CASE("an upload clears a job left by a recording", "[test_personalize_foundation_runtime]") {
    // from
    // tests/test_personalize_foundation_runtime.py::test_an_upload_clears_a_job_left_by_a_recording
    // The recorder stops on its own, leaving the recording's job and profile behind.
    CleanupRig c;
    auto& rig = c.rig;
    REQUIRE(rig.runtime->start_recording(StartOptions{.profile_id = "email"}));
    rig.recorder.recording = false;

    REQUIRE(rig.runtime->upload_audio_file(rig.wav("upload.wav")));

    CHECK_FALSE(rig.runtime->current_job());
    CHECK(rig.runtime->recording_profile().empty());
    rig.run_workers();
    const auto entries = rig.entries();
    REQUIRE(entries.size() == 1);
    CHECK(entries[0].source_name == "upload.wav");
}

// ---- final pass ----

TEST_CASE("final pass gets recognition only for a job", "[test_personalize_foundation_runtime]") {
    // from
    // tests/test_personalize_foundation_runtime.py::test_final_pass_gets_recognition_only_for_a_job
    // The context shows as the language the engine is asked for: the job's, or the engine's own
    // ("") for a file without a job.
    RigOptions options;
    options.settings["dictation_languages"] = Json::array({"en", "fr"});
    options.settings["dictation_active_language"] = "fr";
    CleanupRig c(options);
    auto& rig = c.rig;

    c.transcribe(dictation(), "hi");
    REQUIRE(rig.runtime->claim_job());
    rig.runtime->transcribe_audio_file(rig.wav("upload.wav"));
    rig.run_workers();

    std::vector<std::string> languages;
    for (const auto& call : rig.engine->calls()) {
        if (call.op == "decode") {
            languages.push_back(call.language);
        }
    }
    CHECK(languages == std::vector<std::string>{"fr", ""});
}

// ---- cleanup stage ----

TEST_CASE("dictionary applies with cleanup off", "[test_personalize_foundation_runtime]") {
    // from tests/test_personalize_foundation_runtime.py::test_dictionary_applies_with_cleanup_off
    FakeCleaner cleaner;
    const Json settings{
        {"dictation_dictionary",
         Json::array({Json{{"id", "a"}, {"term", "Acme"}, {"heard", Json::array({"ackme"})}}})}};

    const CleanupOutcome outcome = cleanup("ackme rocks", settings, cleaner);

    CHECK(outcome.text == "Acme rocks");
    CHECK_FALSE(outcome.raw_text);
    CHECK_FALSE(outcome.info);
    CHECK(cleaner.calls().empty());
}

TEST_CASE("cleanup records its level", "[test_personalize_foundation_runtime]") {
    // from tests/test_personalize_foundation_runtime.py::test_cleanup_records_its_level
    FakeCleaner cleaner([](const std::string&) { return std::string("Hello there."); });

    const CleanupOutcome outcome =
        cleanup("hello there", Json{{"transcript_cleanup_enabled", true}}, cleaner);

    CHECK(outcome.text == "Hello there.");
    CHECK(outcome.raw_text == "hello there");
    REQUIRE(outcome.info);
    CHECK(outcome.info->level == "medium");
    CHECK(outcome.info->provider == "openai");
    CHECK(outcome.info->model == "gpt");
    const auto calls = cleaner.calls();
    REQUIRE(calls.size() == 1);
    CHECK(calls[0].text == "hello there");
}

TEST_CASE("a whole snippet skips cleanup", "[test_personalize_foundation_runtime]") {
    // from tests/test_personalize_foundation_runtime.py::test_a_whole_snippet_skips_cleanup
    FakeCleaner cleaner([](const std::string& text) { return fv::py::upper(text); });
    const Json settings{
        {"transcript_cleanup_enabled", true},
        {"dictation_snippets", Json::array({snippet_json("s", "my address", "1 Main St")})}};
    const auto job = dictation();

    const CleanupOutcome outcome = cleanup("my address", settings, cleaner, job.get());

    CHECK(outcome.text == "1 Main St");
    CHECK_FALSE(outcome.raw_text);
    CHECK_FALSE(outcome.info);
    CHECK(cleaner.calls().empty());
}

TEST_CASE("a lost placeholder reports a failed cleanup", "[test_personalize_foundation_runtime]") {
    // from
    // tests/test_personalize_foundation_runtime.py::test_a_lost_placeholder_reports_a_failed_cleanup
    // The snippet functions the Python test patches, as a preparer.
    class StandIn final : public TextPreparer {
    public:
        SnippetPlan plan_expansion(std::string_view, const Json&) const override {
            return SnippetPlan{"thanks [[S1]]",
                               std::nullopt,
                               {{"[[S1]]", Snippet{"s", "sig", "Best, Dana", true}}}};
        }
        Expansion expand(std::string_view text, const SnippetPlan&) const override {
            std::string out(text);
            const auto at = out.find("[[S1]]");
            if (at == std::string::npos) {
                return Expansion{out, "", false};
            }
            out.replace(at, 6, "Best, Dana");
            return Expansion{out, "<b>Dana</b>", true};
        }
    } preparer;
    FakeCleaner cleaner([](const std::string&) { return std::string("Thanks."); });
    const auto job = dictation();

    const CleanupOutcome outcome = cleanup("thanks sig", Json{{"transcript_cleanup_enabled", true}},
                                           cleaner, job.get(), std::nullopt, &preparer);

    CHECK(outcome.text == "thanks Best, Dana");
    CHECK_FALSE(outcome.raw_text);
    CHECK_FALSE(outcome.info);
    CHECK(outcome.failure == "snippet placeholder lost");
    CHECK(outcome.delivery_html == "<b>Dana</b>");
}

// ---- delivery ----

TEST_CASE("dictation paste call is unchanged without html",
          "[test_personalize_foundation_runtime]") {
    // from
    // tests/test_personalize_foundation_runtime.py::test_dictation_paste_call_is_unchanged_without_html
    CleanupRig c;

    c.complete(dictation(), "Hello.");

    const auto& stages = c.rig.clipboard.inner.stages;
    REQUIRE(stages.size() == 1);
    CHECK(stages[0].text == "Hello.");
    CHECK_FALSE(stages[0].html);
    CHECK(c.pastes() == 1);
}

TEST_CASE("html reaches the clipboard stage", "[test_personalize_foundation_runtime]") {
    // from tests/test_personalize_foundation_runtime.py::test_html_reaches_the_clipboard_stage
    // The worker sets the rich text from a formatted snippet, where Python sets it by hand.
    CleanupRig c(with(Json{{"dictation_snippets",
                            Json::array({snippet_json("hi", "say hello", "**Hello.**", true)})}}));

    c.transcribe(dictation(), "Say hello.");

    const auto& stages = c.rig.clipboard.inner.stages;
    REQUIRE(stages.size() == 1);
    CHECK(stages[0].text == "Hello.");
    CHECK(stages[0].html == "<b>Hello.</b>");
}

TEST_CASE("the focused scratchpad takes the dictation", "[test_personalize_foundation_runtime]") {
    // from
    // tests/test_personalize_foundation_runtime.py::test_the_focused_scratchpad_takes_the_dictation
    CleanupRig c;
    auto& rig = c.rig;
    FakeScratchpad pad;
    pad.focused = true;
    rig.runtime->set_scratchpad(&pad);
    PendingAudio pending;
    pending.audio_path = rig.wav("recording.wav");
    rig.runtime->set_pending(pending);

    c.complete(dictation(), "Note to self.");

    CHECK(pad.inserted == std::vector<std::string>{"Note to self."});
    CHECK(rig.clipboard.inner.stages.empty());
    CHECK(c.pastes() == 0);
    CHECK(rig.statuses().back() == "Ready (Added to Scratchpad)");
    const auto entries = rig.entries();
    REQUIRE(entries.size() == 1);
    CHECK(entries[0].text == "Note to self.");
    rig.runtime->set_scratchpad(nullptr);
}

TEST_CASE("after paste follows a successful paste", "[test_personalize_foundation_runtime]") {
    // from
    // tests/test_personalize_foundation_runtime.py::test_after_paste_follows_a_successful_paste
    CleanupRig c;
    auto& rig = c.rig;
    RecordingPreparer preparer;
    rig.runtime->set_text_preparer(&preparer);

    c.complete(dictation(), "Hello.");
    c.complete(dictation(), "Hello, Sam.", "hello sam");
    rig.settings.update(Json{{"auto_paste", false}});
    c.complete(dictation(), "Not pasted.");

    CHECK(preparer.learned() == std::vector<RecordingPreparer::Learning>{
                                    {JobMode::Dictation, "Hello.", std::nullopt},
                                    {JobMode::Dictation, "Hello, Sam.", "hello sam"}});
    rig.runtime->set_text_preparer(nullptr);
}

// ---- history ----

TEST_CASE("history entries carry the job context", "[test_personalize_foundation_runtime]") {
    // from tests/test_personalize_foundation_runtime.py::test_history_entries_carry_the_job_context
    CleanupRig c;
    auto& rig = c.rig;
    fv::focus::FakeCaptureService service(FocusSnapshot{outlook(), std::nullopt});
    const auto job = std::make_shared<const DictationJob>(
        begin_job(JobMode::Dictation, Json::object(), RecognitionContext{}, {}, &service));
    PendingAudio pending;
    pending.audio_path = rig.wav("recording.wav");
    rig.runtime->set_pending(pending);

    c.complete(job, "Hello.", "hello", CleanupInfo{"openai", "gpt", 0.3, "high"});
    say(*rig.engine, "From a file.");
    REQUIRE(rig.runtime->upload_audio_file(rig.wav("upload.wav")));
    rig.run_workers();
    c.complete(nullptr, "Re-transcribed.");

    const auto live = c.entry_with("Hello.");
    const auto upload = c.entry_with("From a file.");
    const auto retranscribed = c.entry_with("Re-transcribed.");
    REQUIRE(live);
    REQUIRE(upload);
    REQUIRE(retranscribed);
    CHECK(live->entry_kind == "dictation");
    CHECK(live->app_id == "outlook.exe");
    CHECK(live->app_name == "Outlook");
    CHECK(live->cleanup_level == "high");
    CHECK(upload->entry_kind == "file");
    CHECK_FALSE(upload->app_id);
    CHECK(retranscribed->entry_kind == "file");
}

TEST_CASE("batch entries are file entries", "[test_personalize_foundation_runtime]") {
    // from tests/test_personalize_foundation_runtime.py::test_batch_entries_are_file_entries
    CleanupRig c;
    auto& rig = c.rig;
    const auto audio = fv::base_utf8::path_to_utf8(rig.wav("upload.wav"));
    BatchResult result;
    result.request.items = {BatchItem{audio, std::nullopt}, BatchItem{audio, std::nullopt}};
    for (int n = 0; n < 2; ++n) {
        BatchItemResult item;
        item.item = result.request.items[static_cast<std::size_t>(n)];
        item.text = "Part " + std::to_string(n) + ".";
        result.items.push_back(item);
    }
    REQUIRE(rig.runtime->claim_job());

    rig.runtime->on_batch_complete(result);
    rig.run_workers();

    const auto entries = rig.entries();
    REQUIRE(entries.size() == 2);
    for (const auto& entry : entries) {
        CHECK(entry.entry_kind == "file");
    }
}

TEST_CASE("stats failures never touch the saved entry", "[test_personalize_foundation_runtime]") {
    // from
    // tests/test_personalize_foundation_runtime.py::test_stats_failures_never_touch_the_saved_entry
    // The stats table is gone, so recording the stats throws, where Python patches record_stats.
    CleanupRig c;
    auto& rig = c.rig;
    rig.database->write(
        [](fv::sql::Connection& connection) { connection.exec("DROP TABLE dictation_stats"); });
    std::vector<HistoryOutcome> outcomes;
    const auto link = rig.runtime->history_persisted.connect(
        rig.ui, [&outcomes](HistoryOutcome outcome) { outcomes.push_back(outcome); });

    c.complete(dictation(), "Hello.");

    const auto entries = rig.entries();
    REQUIRE(entries.size() == 1);
    CHECK(entries[0].text == "Hello.");
    CHECK(outcomes == std::vector<HistoryOutcome>{HistoryOutcome{"", true, ""}});
    CHECK(rig.last_status() == "Ready (Pasted)");
    CHECK_FALSE(rig.runtime->has_active_job());
}

TEST_CASE("paste text now pastes without history and refuses while busy",
          "[test_personalize_foundation_runtime]") {
    // from
    // tests/test_personalize_foundation_runtime.py::test_paste_text_now_pastes_without_history_and_refuses_while_busy
    CleanupRig c(with(Json{{"auto_paste", false}}));
    auto& rig = c.rig;

    CHECK(rig.runtime->paste_text_now("original words"));
    REQUIRE(rig.clipboard.inner.stages.size() == 1);
    CHECK(rig.clipboard.inner.stages[0].text == "original words");
    rig.run_workers();
    CHECK(rig.entries().empty());

    REQUIRE(rig.runtime->claim_job());
    CHECK_FALSE(rig.runtime->paste_text_now("again"));
    CHECK(c.pastes() == 1);
}

// ---- tests/test_personalize_s5_pipeline.py ----

namespace {

const Json kCal = snippet_json("cal", "my calendar link", "https://cal.example.com/dana?ref=email");
const Json kSignOff = snippet_json("sig", "my sign off", "**Dana Lee**\n- Product designer", true);
const std::string kCalText = "https://cal.example.com/dana?ref=email";

Json s5_settings(Json extra = Json::object()) {
    Json settings{{"dictation_snippets", Json::array({kCal, kSignOff})},
                  {"app_context_enabled", false}};
    settings.update(extra);
    return settings;
}

} // namespace

TEST_CASE("a whole snippet skips cleanup and pastes its exact text",
          "[test_personalize_s5_pipeline]") {
    // from
    // tests/test_personalize_s5_pipeline.py::test_a_whole_snippet_skips_cleanup_and_pastes_its_exact_text
    const DictationJob job;
    const PreparedText prepared =
        prepare_text("My calendar link.", &job, s5_settings(), dictionary_preparer());

    CHECK(prepared.skip_cleanup);
    CHECK(finish_text(prepared.cleanup_input, prepared, dictionary_preparer()) ==
          FinishedText{kCalText, "", true, kCalText});
}

TEST_CASE("an embedded snippet survives cleanup", "[test_personalize_s5_pipeline]") {
    // from tests/test_personalize_s5_pipeline.py::test_an_embedded_snippet_survives_cleanup
    const DictationJob job;
    const PreparedText prepared = prepare_text("here's my calendar link for friday", &job,
                                               s5_settings(), dictionary_preparer());

    CHECK_FALSE(prepared.skip_cleanup);
    CHECK(prepared.cleanup_input == "here's [[S1]] for friday");
    const std::string cleaned = capitalize_and_punctuate(prepared.cleanup_input);
    CHECK(finish_text(cleaned, prepared, dictionary_preparer()) ==
          FinishedText{"Here's " + kCalText + " for friday.", "", true,
                       "here's " + kCalText + " for friday"});
}

TEST_CASE("a lost placeholder falls back to the uncleaned text with expansions",
          "[test_personalize_s5_pipeline]") {
    // from
    // tests/test_personalize_s5_pipeline.py::test_a_lost_placeholder_falls_back_to_the_uncleaned_text_with_expansions
    const DictationJob job;
    const PreparedText prepared =
        prepare_text("thanks again my sign off", &job, s5_settings(), dictionary_preparer());

    CHECK(finish_text("Thanks again.", prepared, dictionary_preparer()) ==
          FinishedText{"thanks again Dana Lee\n- Product designer",
                       "thanks again <b>Dana Lee</b><ul><li>Product designer</li></ul>", false,
                       "thanks again Dana Lee\n- Product designer"});
}

TEST_CASE("snippets only run for live dictation with snippets on",
          "[test_personalize_s5_pipeline]") {
    // from
    // tests/test_personalize_s5_pipeline.py::test_snippets_only_run_for_live_dictation_with_snippets_on
    const int which = GENERATE(0, 1, 2, 3);
    DictationJob job;
    Json settings = s5_settings();
    const DictationJob* given = &job;
    if (which == 0) {
        settings["snippets_enabled"] = false;
    } else if (which == 1) {
        job.mode = JobMode::Command;
    } else if (which == 2) {
        job.mode = JobMode::Transform;
    } else {
        given = nullptr;
    }

    const PreparedText prepared =
        prepare_text("my calendar link", given, settings, dictionary_preparer());

    CHECK_FALSE(prepared.skip_cleanup);
    CHECK(prepared.cleanup_input == "my calendar link");
    CHECK(finish_text("My calendar link.", prepared, dictionary_preparer()).text ==
          "My calendar link.");
}

TEST_CASE("the cleanup prompt carries the placeholder guard", "[test_personalize_s5_pipeline]") {
    // from
    // tests/test_personalize_s5_pipeline.py::test_the_cleanup_prompt_carries_the_placeholder_guard
    const Json settings = s5_settings();
    const DictationJob job;
    const PreparedText prepared =
        prepare_text("send my calendar link", &job, settings, dictionary_preparer());

    const std::string prompt = compose_cleanup_prompt(settings, &job, nullptr, {}, prepared);

    CHECK(prompt.ends_with("The transcript contains the placeholder [[S1]], which stands for text "
                           "inserted later; keep it exactly once, unchanged and in its place."));
}

TEST_CASE("the runtime never sends a whole snippet to cleanup", "[test_personalize_s5_pipeline]") {
    // from
    // tests/test_personalize_s5_pipeline.py::test_the_runtime_never_sends_a_whole_snippet_to_cleanup
    FakeCleaner cleaner(capitalize_and_punctuate);
    const DictationJob job;

    const CleanupOutcome outcome = cleanup(
        "My sign off.", s5_settings(Json{{"transcript_cleanup_enabled", true}}), cleaner, &job);

    CHECK(outcome.text == "Dana Lee\n- Product designer");
    CHECK_FALSE(outcome.raw_text);
    CHECK_FALSE(outcome.info);
    CHECK(cleaner.calls().empty());
    CHECK(outcome.delivery_html == "<b>Dana Lee</b><ul><li>Product designer</li></ul>");
}

TEST_CASE("the runtime cleans around a protected snippet", "[test_personalize_s5_pipeline]") {
    // from
    // tests/test_personalize_s5_pipeline.py::test_the_runtime_cleans_around_a_protected_snippet
    FakeCleaner cleaner(capitalize_and_punctuate);
    const DictationJob job;

    const CleanupOutcome outcome =
        cleanup("book with my calendar link",
                s5_settings(Json{{"transcript_cleanup_enabled", true}}), cleaner, &job);

    const auto calls = cleaner.calls();
    REQUIRE(calls.size() == 1);
    CHECK(calls[0].text == "book with [[S1]]");
    CHECK(calls[0].system_prompt.find("[[S1]]") != std::string::npos);
    CHECK((calls[0].text + calls[0].system_prompt).find(kCalText) == std::string::npos);
    CHECK(outcome.text == "Book with " + kCalText + ".");
    CHECK(outcome.raw_text == "book with " + kCalText);
    CHECK(outcome.info);
    CHECK(outcome.delivery_html.empty());
}

TEST_CASE("the runtime reports a cleanup that dropped a snippet",
          "[test_personalize_s5_pipeline]") {
    // from
    // tests/test_personalize_s5_pipeline.py::test_the_runtime_reports_a_cleanup_that_dropped_a_snippet
    FakeCleaner cleaner([](const std::string&) { return std::string("Book with it."); });
    const DictationJob job;

    const CleanupOutcome outcome =
        cleanup("book with my calendar link",
                s5_settings(Json{{"transcript_cleanup_enabled", true}}), cleaner, &job);

    CHECK(outcome.text == "book with " + kCalText);
    CHECK_FALSE(outcome.raw_text);
    CHECK_FALSE(outcome.info);
    CHECK(outcome.failure == "snippet placeholder lost");
}

// ---- tests/test_application_controller.py ----

TEST_CASE("transcription complete passes cleanup time to ui", "[test_application_controller]") {
    // from
    // tests/test_application_controller.py::TestApplicationController::test_transcription_complete_passes_cleanup_time_to_ui
    CleanupRig c;
    auto& rig = c.rig;
    PendingAudio pending;
    pending.audio_path = rig.wav("source.wav");
    pending.audio_duration = 10.0;
    pending.file_size = 2048;
    pending.transcription_time = 2.5;
    rig.runtime->set_pending(pending);

    c.complete(nullptr, "Cleaned text", "raw text", CleanupInfo{"openai", "gpt-4o", 1.2, ""});

    REQUIRE(rig.stats_shown);
    CHECK(rig.stats_shown->cleanup_time == 1.2);
}

TEST_CASE("transcribe audio file applies cleanup when enabled", "[test_application_controller]") {
    // from
    // tests/test_application_controller.py::TestApplicationController::test_transcribe_audio_file_applies_cleanup_when_enabled
    // The real TranscriptCleanup on its default endpoint (OpenRouter), against a fake provider.
    RigOptions options;
    options.settings["transcript_cleanup_enabled"] = true;
    options.script.texts = {"um cleaned text"};
    RuntimeRig rig(options);
    CleanupKeys keys;
    auto http = std::make_shared<fv::http::FakeHttpClient>();
    http->set_handler(chat_answer("Cleaned text."));
    rig.runtime->set_cleanup(make_cleanup(http, keys, rig.settings));

    rig.runtime->transcribe_audio_file(rig.wav("clip.wav"));
    rig.run_workers();
    rig.runtime->set_cleanup(nullptr);

    const auto entries = rig.entries();
    REQUIRE(entries.size() == 1);
    CHECK(entries[0].text == "Cleaned text.");
    CHECK(entries[0].raw_text == "um cleaned text");
    CHECK(entries[0].cleanup_provider == "openrouter");
    REQUIRE(entries[0].cleanup_model);
    CHECK_FALSE(entries[0].cleanup_model->empty());
}

TEST_CASE("profile recording formats with start snapshot and copies result",
          "[test_application_controller]") {
    // from
    // tests/test_application_controller.py::TestApplicationController::test_profile_recording_formats_with_start_snapshot_and_copies_result
    CleanupRig c(with(Json{{"transcript_cleanup_rules", Json::array({"Spell Acme exactly."})}}),
                 [](const std::string&) {
                     return std::string("Subject: Help\n\nPlease fix the login issue.");
                 });
    auto& rig = c.rig;
    REQUIRE(rig.runtime->start_recording(StartOptions{.profile_id = "email"}));
    // A library edit after Start must not change this job's format or rules.
    rig.settings.update(Json{{"transcript_cleanup_profiles", Json::array()},
                             {"transcript_cleanup_rules", Json::array({"Replace every name."})}});

    rig.runtime->stop_recording();
    rig.run_workers();

    const auto calls = c.cleaner.calls();
    REQUIRE(calls.size() == 1);
    CHECK(calls[0].system_prompt.find(starter("email").instructions) != std::string::npos);
    CHECK(calls[0].system_prompt.find("Spell Acme exactly.") != std::string::npos);
    CHECK(calls[0].system_prompt.find("Replace every name.") == std::string::npos);
    const auto entries = rig.entries();
    REQUIRE(entries.size() == 1);
    CHECK(entries[0].source_name == "Quick Record · Email");
    CHECK(entries[0].raw_text);
    CHECK(entries[0].text.starts_with("Subject: Help"));
    CHECK(entries[0].cleanup_level == "profile");
    REQUIRE_FALSE(rig.clipboard.inner.stages.empty());
    CHECK(rig.clipboard.inner.stages.back().text == entries[0].text);
    CHECK(rig.runtime->recording_profile().empty());
    CHECK_FALSE(rig.runtime->has_active_job());
    CHECK_FALSE(fv::keys::transcript_cleanup_enabled.read(*rig.settings.snapshot()));
}

TEST_CASE("profile shortcut stops without replacing recording format",
          "[test_application_controller]") {
    // from
    // tests/test_application_controller.py::TestApplicationController::test_profile_shortcut_stops_without_replacing_recording_format
    // The profile shortcut (M4's hotkey runtime) starts a profile recording or stops the one
    // running: a start for another profile is refused, and the toggle stops.
    CleanupRig c;
    auto& rig = c.rig;
    REQUIRE(rig.runtime->start_recording(StartOptions{.profile_id = "support-ticket"}));

    CHECK_FALSE(rig.runtime->start_recording(StartOptions{.profile_id = "email"}));
    rig.runtime->toggle_recording();

    CHECK_FALSE(rig.recorder.is_recording());
    CHECK(rig.runtime->recording_profile() == "support-ticket");
    rig.run_workers();
}

TEST_CASE("cancel profile then standard recording and upload use standard cleanup",
          "[test_application_controller]") {
    // from
    // tests/test_application_controller.py::TestApplicationController::test_cancel_profile_then_standard_recording_and_upload_use_standard_cleanup
    CleanupRig c;
    auto& rig = c.rig;
    REQUIRE(rig.runtime->start_recording(StartOptions{.profile_id = "email"}));
    rig.runtime->cancel();
    CHECK(rig.runtime->recording_profile().empty());
    REQUIRE(rig.runtime->start_recording());
    CHECK(rig.runtime->recording_profile().empty());
    rig.runtime->cancel();

    // Uploads and retranscriptions share this cleanup path, but no profile.
    c.transcribe(nullptr, "unformatted");

    CHECK(rig.transcript_text == "unformatted");
    CHECK_FALSE(rig.transcript_raw);
    CHECK(c.cleaner.calls().empty());
}

TEST_CASE("missing busy and failed profile starts do not leak profile",
          "[test_application_controller]") {
    // from
    // tests/test_application_controller.py::TestApplicationController::test_missing_busy_and_failed_profile_starts_do_not_leak_profile
    CleanupRig c;
    auto& rig = c.rig;

    CHECK_FALSE(rig.runtime->start_recording(StartOptions{.profile_id = "deleted"}));
    CHECK(rig.last_status() == "Cleanup profile no longer exists");
    CHECK_FALSE(rig.recorder.is_recording());
    REQUIRE(rig.runtime->claim_job());
    CHECK_FALSE(rig.runtime->start_recording(StartOptions{.profile_id = "email"}));
    rig.runtime->finish_job();
    rig.recorder.start_ok = false;
    CHECK_FALSE(rig.runtime->start_recording(StartOptions{.profile_id = "email"}));

    CHECK(rig.runtime->recording_profile().empty());
}

TEST_CASE("unavailable profile cleanup reports raw fallback", "[test_application_controller]") {
    // from
    // tests/test_application_controller.py::TestApplicationController::test_unavailable_profile_cleanup_reports_raw_fallback
    CleanupRig c;
    auto& rig = c.rig;
    c.cleaner.available = false;
    REQUIRE(rig.runtime->start_recording(StartOptions{.profile_id = "email"}));

    rig.runtime->stop_recording();
    rig.run_workers();

    const std::string status = rig.last_status();
    CHECK(status.find("Email formatting failed") != std::string::npos);
    CHECK(status.find("using raw transcript") != std::string::npos);
    CHECK(status == "Ready (Pasted) — Email formatting failed (cleanup unavailable); using raw "
                    "transcript");
    CHECK(rig.runtime->recording_profile().empty());
}

TEST_CASE("combined cleanup failure shows raw text and says so in status",
          "[test_application_controller]") {
    // from
    // tests/test_application_controller.py::TestApplicationController::test_combined_cleanup_failure_shows_raw_text_and_says_so_in_status
    CleanupRig c(with(Json{{"transcript_cleanup_enabled", true}}));
    auto& rig = c.rig;
    c.cleaner.error = "timed out";
    BatchUploadRequest request;
    request.items = {BatchItem{fv::base_utf8::path_to_utf8(rig.wav("a.wav")), std::nullopt},
                     BatchItem{fv::base_utf8::path_to_utf8(rig.wav("b.wav")), std::nullopt}};
    request.relation = BatchRelation::Sequential;

    REQUIRE(rig.runtime->upload_audio_files(request));
    rig.run_workers();

    const auto entries = rig.entries();
    REQUIRE(entries.size() == 1);
    CHECK(entries[0].text.find("\n\n") != std::string::npos);
    CHECK_FALSE(entries[0].raw_text);
    const std::string status = rig.last_status();
    CHECK(status.starts_with("Ready"));
    CHECK(status.find("AI cleanup failed (timed out)") != std::string::npos);
}

// ---- one test each of four more files ----

TEST_CASE("cancel during a stalled cleanup frees the dictation", "[test_cleanup_deadline]") {
    // from tests/test_cleanup_deadline.py::test_cancel_during_a_stalled_cleanup_frees_the_dictation
    RigOptions options;
    options.settings["transcript_cleanup_enabled"] = true;
    options.script.texts = {"um private text"};
    RuntimeRig rig(options);
    CleanupKeys keys;
    auto stalling = std::make_shared<StallingClient>();
    rig.runtime->set_cleanup(make_cleanup(stalling, keys, rig.settings));
    REQUIRE(rig.runtime->claim_job());
    const auto clip = rig.wav("recorded.wav");

    auto worker = std::async(std::launch::async, [&] { rig.runtime->transcribe_audio_file(clip); });
    REQUIRE(stalling->wait_started());
    rig.runtime->cancel();
    const bool ended = worker.wait_for(1s) == std::future_status::ready;
    stalling->release();
    worker.wait();
    rig.run_workers();
    rig.runtime->set_cleanup(nullptr);

    CHECK(ended);
    CHECK(rig.trace.with_prefix("transcript:").empty());
    CHECK(rig.trace.with_prefix("error_dialog:") ==
          std::vector<std::string>{"Error: Transcription canceled"});
    CHECK_FALSE(rig.runtime->has_active_job());
}

TEST_CASE("cancel during cleanup fails the job instead of pasting", "[test_cancel_after_stop]") {
    // from
    // tests/test_cancel_after_stop.py::test_cancel_during_cleanup_fails_the_job_instead_of_pasting
    CleanupRig c(with(Json{{"transcript_cleanup_enabled", true}}));
    auto& rig = c.rig;
    c.cleaner.during = [&rig] {
        rig.runtime->cancel();
    };

    c.transcribe(dictation(), "Hello there.");

    CHECK(rig.trace.with_prefix("transcript:").empty());
    CHECK(rig.trace.with_prefix("error_dialog:") ==
          std::vector<std::string>{"Error: Transcription canceled"});
    CHECK(rig.clipboard.inner.stages.empty());
}

TEST_CASE("cleanup reads its switch strictly", "[test_strict_switch_reads]") {
    // from tests/test_strict_switch_reads.py::test_cleanup_reads_its_switch_strictly
    const auto [stored, expected] = GENERATE(table<Json, bool>({{Json(true), true},
                                                                {Json(false), false},
                                                                {Json("false"), false},
                                                                {Json("no"), false},
                                                                {Json(1), false},
                                                                {Json(0), false},
                                                                {Json(nullptr), false}}));
    const Json settings{{"transcript_cleanup_enabled", stored}};

    CHECK(fv::text::cleanup_may_run(settings) == expected);
    // The stage reads the switch the same way.
    FakeCleaner cleaner([](const std::string&) { return std::string("Cleaned."); });
    cleanup("raw words", settings, cleaner);
    CHECK(cleaner.calls().size() == (expected ? 1u : 0u));
}

TEST_CASE("copy only delivery keeps snippet formatting", "[test_personalize_coordinator]") {
    // from tests/test_personalize_coordinator.py::test_copy_only_delivery_keeps_snippet_formatting
    fv::focus::FakeClipboard clipboard;
    fv::focus::FakeSyntheticKeys keys;
    DeliveryPort port;
    port.clipboard = &clipboard;
    port.keys = &keys;
    const Json settings{{"copy_clipboard", true}, {"auto_paste", false}};
    const StatusReport report = [](const std::string&) {
    };

    apply_clipboard_and_paste(port, settings, "Book here: cal.example", report, {}, "<a>Book</a>");
    apply_clipboard_and_paste(port, settings, "plain", report);

    CHECK(clipboard.writes == std::vector<std::pair<std::string, std::string>>{
                                  {"Book here: cal.example", "<a>Book</a>"}, {"plain", ""}});
}

// ---- the package's acceptance, through the real TranscriptCleanup ----

TEST_CASE("a dictation cleaned at medium shows cleaning and pastes the model's text",
          "[dictation][cleanup]") {
    RigOptions options;
    options.settings["transcript_cleanup_enabled"] = true;
    options.settings["transcript_cleanup_level"] = "medium";
    options.script.texts = {"um so the meeting is uh tomorrow at ten"};
    RuntimeRig rig(options);
    CleanupKeys keys;
    auto http = std::make_shared<fv::http::FakeHttpClient>();
    http->set_handler(chat_answer("The meeting is tomorrow at 10."));
    rig.runtime->set_cleanup(make_cleanup(http, keys, rig.settings));

    REQUIRE(rig.runtime->start_recording());
    rig.runtime->stop_recording();
    rig.run_workers();
    rig.runtime->set_cleanup(nullptr);

    CHECK(rig.trace.has("overlay:cleaning"));
    CHECK(rig.trace.has("status:Cleaning up..."));
    REQUIRE(rig.clipboard.inner.stages.size() == 1);
    CHECK(rig.clipboard.inner.stages[0].text == "The meeting is tomorrow at 10.");
    const auto entries = rig.entries();
    REQUIRE(entries.size() == 1);
    CHECK(entries[0].raw_text == "um so the meeting is uh tomorrow at ten");
    CHECK(entries[0].cleanup_level == "medium");
    CHECK(entries[0].cleanup_provider == "openrouter");
    // The system prompt is the Medium preset; the transcript goes as the user's message.
    const auto requests = http->requests();
    REQUIRE_FALSE(requests.empty());
    const Json messages = sent_messages(requests.back());
    REQUIRE(messages.size() == 2);
    CHECK(messages[0].at("content").get<std::string>().starts_with(fv::text::preset("medium")));
    CHECK(messages[1].at("content") == "um so the meeting is uh tomorrow at ten");
}

TEST_CASE("a model that never answers pastes the raw text after seventeen seconds",
          "[dictation][cleanup]") {
    RigOptions options;
    options.settings["transcript_cleanup_enabled"] = true;
    options.script.texts = {"um raw words"};
    RuntimeRig rig(options);
    CleanupKeys keys;
    fv::FakeClock clock;
    auto stalling = std::make_shared<StallingClient>();
    rig.runtime->set_cleanup(make_cleanup(stalling, keys, rig.settings, &clock));
    REQUIRE(rig.runtime->start_recording());
    rig.runtime->stop_recording();

    auto worker = std::async(std::launch::async, [&rig] { rig.run_workers(); });
    REQUIRE(stalling->wait_started());
    // Two 8 s attempts and a second for the retry's backoff.
    clock.advance(16900ms);
    CHECK(worker.wait_for(300ms) == std::future_status::timeout);
    clock.advance(100ms);
    REQUIRE(worker.wait_for(5s) == std::future_status::ready);
    worker.get();
    stalling->release();
    rig.runtime->set_cleanup(nullptr);

    REQUIRE(rig.clipboard.inner.stages.size() == 1);
    CHECK(rig.clipboard.inner.stages[0].text == "um raw words");
    const auto entries = rig.entries();
    REQUIRE(entries.size() == 1);
    CHECK_FALSE(entries[0].raw_text);
    CHECK_FALSE(entries[0].cleanup_provider);
    // A dictation falls back without a word in the status, as OpenWhisper's does; only a
    // profile or a batch says so.
    CHECK(rig.last_status() == "Ready (Pasted)");
}

TEST_CASE("a combined batch whose model never answers says why in the status",
          "[dictation][cleanup]") {
    RigOptions options;
    options.settings["transcript_cleanup_enabled"] = true;
    RuntimeRig rig(options);
    CleanupKeys keys;
    fv::FakeClock clock;
    auto stalling = std::make_shared<StallingClient>();
    rig.runtime->set_cleanup(make_cleanup(stalling, keys, rig.settings, &clock));
    BatchUploadRequest request;
    request.items = {BatchItem{fv::base_utf8::path_to_utf8(rig.wav("a.wav")), std::nullopt},
                     BatchItem{fv::base_utf8::path_to_utf8(rig.wav("b.wav")), std::nullopt}};
    request.relation = BatchRelation::Sequential;
    REQUIRE(rig.runtime->upload_audio_files(request));

    auto worker = std::async(std::launch::async, [&rig] { rig.run_workers(); });
    REQUIRE(stalling->wait_started());
    // The combined pass: 120 s attempts with the batch's two retries, and the backoff's second.
    clock.advance(360s);
    CHECK(worker.wait_for(300ms) == std::future_status::timeout);
    clock.advance(1s);
    REQUIRE(worker.wait_for(5s) == std::future_status::ready);
    worker.get();
    stalling->release();
    rig.runtime->set_cleanup(nullptr);

    CHECK(rig.last_status() ==
          "Ready — AI cleanup failed (timed out after 361 s); showing raw text");
}

TEST_CASE("the llm cleaner applies the resolved endpoint on every check", "[dictation][cleanup]") {
    fv::MemorySettingsStore settings(
        Json{{"transcript_cleanup_provider", "openai"}, {"transcript_cleanup_model", "gpt-test"}});
    CleanupKeys keys;
    auto http = std::make_shared<fv::http::FakeHttpClient>();
    http->set_handler(chat_answer("Tidy."));
    LlmTranscriptCleaner cleaner(make_cleanup(http, keys, settings));

    REQUIRE(cleaner.is_available(*settings.snapshot()));
    CHECK(cleaner.client().provider() == "openai");
    CHECK(cleaner.client().model() == "gpt-test");

    CleanupRequest request;
    request.text = "raw";
    request.settings = *settings.snapshot();
    request.system_prompt = "Be brief.";
    request.profile_name = "Email";
    const CleanupResult result = cleaner.cleanup(request);

    CHECK(result.text == "Tidy.");
    CHECK_FALSE(result.error);
    CHECK(result.provider == "openai");
    CHECK(result.model == "gpt-test");
    CHECK(result.level == "profile");
    const auto requests = http->requests();
    REQUIRE_FALSE(requests.empty());
    CHECK(sent_messages(requests.back()).at(0).at("content") == "Be brief.");

    // Without a key the endpoint is unavailable.
    fv::MemorySettingsStore keyless(
        Json{{"transcript_cleanup_provider", "groq"}, {"transcript_cleanup_model", "llama-test"}});
    LlmTranscriptCleaner other(make_cleanup(http, keys, keyless));
    CHECK_FALSE(other.is_available(*keyless.snapshot()));
}
