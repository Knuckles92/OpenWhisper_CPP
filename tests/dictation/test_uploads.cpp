#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <tuple>
#include <vector>

#include "dictation/events.h"
#include "dictation/messages.h"
#include "dictation/transcription_runtime.h"
#include "dictation/uploads.h"
#include "uploads_support.h"

// tests/test_application_controller.py (uploads and batches),
// tests/test_local_transcription_contracts.py (the split a backend reports) and
// tests/test_paste_latency.py::test_upload_displays_ready_then_saves_history_and_never_pastes,
// plus the runtime's own upload behaviour: refusals, the cleaner and Scratchpad ports, the
// backend hooks.

using namespace fv::dictation;
using namespace fv::dictation::testing;
using Catch::Matchers::ContainsSubstring;
using Catch::Matchers::StartsWith;
using fv::Json;

namespace {

std::filesystem::path write_bytes(const std::filesystem::path& path, std::size_t count, char byte) {
    std::ofstream out(path, std::ios::binary);
    out << std::string(count, byte);
    return path;
}

bool contains(const std::vector<std::string>& items, const std::string& item) {
    return std::find(items.begin(), items.end(), item) != items.end();
}

// The trace entries with `prefix` that came before the first `until` (the worker's own steps,
// before the result reaches the UI thread).
std::vector<std::string> before(const Trace& trace, const std::string& prefix,
                                const std::string& until) {
    std::vector<std::string> found;
    for (const auto& entry : trace.entries()) {
        if (entry.starts_with(until)) {
            break;
        }
        if (entry.starts_with(prefix)) {
            found.push_back(entry.substr(prefix.size()));
        }
    }
    return found;
}

// _canceling_backend / the failing backends: the cloud engine with a transcript per file.
void script_api(UploadRig& u, std::function<std::string(const std::string&)> transcribe) {
    u.use_api();
    std::lock_guard lock(u.engines.api->mutex);
    u.engines.api->transcribe = std::move(transcribe);
}

void set_local_texts(UploadRig& u, std::vector<std::string> texts) {
    auto script = Engines::local_script();
    script.texts = std::move(texts);
    u.engines.local->set_script(script);
}

} // namespace

// ---- tests/test_application_controller.py ----

TEST_CASE("test_stop_recording_announces_a_split_only_for_a_backend_that_splits",
          "[test_application_controller]") {
    // The whole teardown is one worker; either way the runtime makes one decode, and splitting
    // is the backend's own business.
    UploadRig u;
    auto& rig = u.rig;
    REQUIRE(rig.runtime->start_recording());
    rig.runtime->stop_recording();
    CHECK(rig.jobs.queued() == 1);
    rig.run_workers();

    // A local backend never splits, so this is one straight decode.
    CHECK(u.transcripts() == std::vector<std::string>{"Hello there."});
    CHECK(u.large_file_states.empty());
    CHECK_FALSE(rig.runtime->has_active_job());

    // A later job, after the first result completed.
    u.use_api(30.0);
    REQUIRE(rig.runtime->start_recording());
    rig.runtime->stop_recording();
    CHECK(rig.jobs.queued() == 1);
    rig.run_workers();

    // The API backend splits, the notice goes out over large_file_detected, and the backend's
    // own steps reach the status.
    CHECK(u.transcripts().back() == "api chunks");
    CHECK(u.large_file_states == std::vector<double>{30.0});
    const auto statuses = rig.statuses();
    CHECK(contains(statuses, "Splitting large file (30.0 MB)..."));
    CHECK(contains(statuses, "Transcribing 2 chunks..."));
}

TEST_CASE("test_upload_reports_large_file_through_the_ui_controller",
          "[test_application_controller]") {
    UploadRig u;
    auto& rig = u.rig;
    const auto audio = write_bytes(rig.root.path() / "upload.wav", 256, '0');
    u.use_api(30.0);

    CHECK(rig.runtime->upload_audio_file(audio, 12.0));
    rig.run_workers();

    CHECK(u.large_file_states == std::vector<double>{30.0});
    REQUIRE(rig.entries().size() == 1);
    CHECK(rig.entries()[0].text == "api chunks");
}

TEST_CASE("test_local_engine_takes_a_large_upload_in_one_pass_with_no_notice",
          "[test_application_controller]") {
    // OpenAI's upload limit is no reason to split, or warn, for a local engine.
    UploadRig u;
    auto& rig = u.rig;
    const auto audio = rig.wav("long.wav");
    // Past config.MAX_FILE_SIZE_MB (23); the WAV header still says how long the audio is.
    std::filesystem::resize_file(audio, (23 + 1) * 1024 * 1024);

    CHECK(rig.runtime->upload_audio_file(audio, 1800.0));
    rig.run_workers();

    CHECK(u.large_file_states.empty());
    for (const auto& status : rig.statuses()) {
        CHECK_THAT(status, !ContainsSubstring("large file"));
    }
    REQUIRE(rig.entries().size() == 1);
    CHECK(rig.entries()[0].text == "Hello there.");
}

TEST_CASE("test_transcription_complete_stores_raw_and_fixed_text",
          "[test_application_controller]") {
    RuntimeRig rig;
    rig.runtime->set_pending(
        {rig.wav("source.wav"), std::nullopt, std::nullopt, std::nullopt, std::nullopt});

    rig.runtime->on_transcription_complete("Fixed sentence.", "um fixed sentence");
    rig.run_workers();

    const auto entries = rig.entries();
    REQUIRE(entries.size() == 1);
    CHECK(entries[0].text == "Fixed sentence.");
    CHECK(entries[0].raw_text == "um fixed sentence");
    CHECK(rig.transcript_text == "Fixed sentence.");
    CHECK(rig.transcript_raw == "um fixed sentence");
    CHECK(rig.clipboard.inner.text() == "Fixed sentence.");
}

TEST_CASE("test_upload_audio_file_uses_preview_duration", "[test_application_controller]") {
    RuntimeRig rig;
    const auto clip = write_bytes(rig.root.path() / "upload.wav", 256, 'x');

    CHECK(rig.runtime->upload_audio_file(clip, 3.72));

    CHECK(rig.runtime->pending().audio_duration == 3.72);
    CHECK(rig.runtime->pending().source_name == "upload.wav");
    CHECK(rig.jobs.queued() == 1);
}

TEST_CASE("test_second_upload_is_refused_without_overwriting_active_job_metadata",
          "[test_application_controller]") {
    RuntimeRig rig;
    const auto first = write_bytes(rig.root.path() / "first.wav", 256, 'x');
    const auto second = write_bytes(rig.root.path() / "second.wav", 512, 'y');

    CHECK(rig.runtime->upload_audio_file(first, 3.0));
    CHECK_FALSE(rig.runtime->upload_audio_file(second, 9.0));

    CHECK(rig.jobs.queued() == 1);
    const PendingAudio pending = rig.runtime->pending();
    CHECK(pending.source_name == "first.wav");
    CHECK(pending.audio_duration == 3.0);
    CHECK(pending.file_size == 256);
    CHECK_THAT(rig.last_status(), ContainsSubstring("already in progress"));
}

TEST_CASE("test_batch_upload_claims_one_job_slot_and_refuses_a_second_upload",
          "[test_application_controller]") {
    UploadRig u;
    auto& rig = u.rig;
    const auto request = u.batch_request({"a.wav", "b.wav"});

    CHECK(rig.runtime->upload_audio_files(request));

    CHECK(rig.runtime->has_active_job());
    CHECK(rig.runtime->upload_job_active());
    CHECK(rig.jobs.queued() == 1);
    CHECK_FALSE(rig.runtime->pending().source_name);

    CHECK_FALSE(rig.runtime->upload_audio_file(
        fv::base_utf8::path_from_utf8(request.items[0].audio_path), 3.0));

    CHECK(rig.jobs.queued() == 1);
    CHECK_THAT(rig.last_status(), ContainsSubstring("already in progress"));
    // The one queued job is the batch.
    rig.run_workers();
    CHECK(u.batch_progress_events.size() == 2);
}

TEST_CASE("test_single_item_batch_request_uses_the_single_file_path",
          "[test_application_controller]") {
    UploadRig u;
    auto& rig = u.rig;
    const auto request = u.batch_request({"a.wav"});

    CHECK(rig.runtime->upload_audio_files(request));

    CHECK(rig.runtime->pending().source_name == "a.wav");
    CHECK(rig.last_status() == "Processing uploaded file...");
    rig.run_workers();
    CHECK(u.batch_progress_events.empty());
    CHECK(rig.entries().size() == 1);
}

TEST_CASE("test_separate_batch_writes_one_history_row_per_file_and_stays_in_the_window",
          "[test_application_controller]") {
    UploadRig u;
    auto& rig = u.rig;
    const auto request = u.batch_request({"a.wav", "b.wav", "c.wav"});

    CHECK(rig.runtime->upload_audio_files(request));
    u.run_batch();

    const auto entries = rig.entries();
    CHECK(u.source_names() == std::vector<std::string>{"a.wav", "b.wav", "c.wav"});
    for (const auto& entry : entries) {
        CHECK_FALSE(entry.audio_file.value_or("").size());
        CHECK(entry.audio_duration == 10.0);
    }
    // Uploads are started from inside the window, so nothing is pasted or copied; the tab's own
    // Copy buttons do that on request.
    CHECK(rig.keys.sent().empty());
    CHECK(rig.clipboard.inner.writes.empty());
    CHECK(rig.clipboard.inner.stages.empty());
    CHECK_THAT(rig.transcript_text, StartsWith("## a.wav\n\nHello there."));
    CHECK_THAT(rig.transcript_text, ContainsSubstring("## b.wav"));
    CHECK(rig.last_status() == "Ready");
    CHECK(u.batch_progress_events == std::vector<std::tuple<int, int, std::string>>{
                                         {1, 3, "a.wav"}, {2, 3, "b.wav"}, {3, 3, "c.wav"}});
    CHECK(u.batch_item_events ==
          std::vector<std::pair<int, bool>>{{1, true}, {2, true}, {3, true}});
    CHECK(u.batch_item_transcripts ==
          std::vector<std::string>{"Hello there.", "Hello there.", "Hello there."});
    REQUIRE(rig.stats_shown);
    CHECK(rig.stats_shown->audio_duration == 30.0);
    CHECK_FALSE(rig.runtime->has_active_job());
}

TEST_CASE("test_single_upload_result_is_neither_copied_nor_pasted",
          "[test_application_controller]") {
    RuntimeRig rig;
    const auto clip = rig.wav("upload.wav");

    CHECK(rig.runtime->upload_audio_file(clip, 3.0));
    rig.run_workers();

    CHECK(rig.keys.sent().empty());
    CHECK(rig.clipboard.inner.writes.empty());
    CHECK(rig.clipboard.inner.stages.empty());
    CHECK(rig.last_status() == "Ready");
    CHECK(rig.entries().size() == 1);
    CHECK_FALSE(rig.runtime->has_active_job());

    // The next dictation pastes again: the upload's choice does not stick.
    rig.runtime->on_transcription_complete("hello world");
    rig.run_workers();
    CHECK(rig.keys.sent() == std::vector<std::string>{"paste"});
}

TEST_CASE("test_sequential_batch_writes_one_row_with_joined_text_and_batch_context",
          "[test_application_controller]") {
    UploadRig u(Json{{"transcript_cleanup_enabled", true}});
    auto& rig = u.rig;
    set_local_texts(u, {"first part.", "second part."});
    RecordingCleaner cleaner("Stitched.");
    rig.runtime->set_cleaner(&cleaner);
    const auto request = u.batch_request({"a.wav", "b.wav"}, BatchRelation::Sequential);

    CHECK(rig.runtime->upload_audio_files(request));
    u.run_batch();

    const auto entries = rig.entries();
    REQUIRE(entries.size() == 1);
    CHECK(entries[0].text == "Stitched.");
    CHECK(entries[0].raw_text == "first part.\n\nsecond part.");
    CHECK_THAT(entries[0].source_name.value_or(""), StartsWith("2 files: "));
    CHECK(entries[0].audio_duration == 20.0);
    const auto calls = cleaner.calls();
    REQUIRE(calls.size() == 1);
    CHECK_THAT(calls[0].system_prompt,
               ContainsSubstring(std::string(TRANSCRIPT_BATCH_CONTEXT_HEADER)));
    CHECK_THAT(calls[0].system_prompt, ContainsSubstring("consecutive parts"));
    CHECK_THAT(calls[0].system_prompt,
               ContainsSubstring(std::string(TRANSCRIPT_BATCH_CONTEXT_GUARD)));
    CHECK(calls[0].timeout_s == TRANSCRIPT_BATCH_CLEANUP_TIMEOUT_S);
    CHECK(rig.transcript_text == "Stitched.");
    CHECK(rig.last_status() == "Ready");
    // One stitched transcript: the rows have no finished text of their own.
    CHECK(u.batch_item_transcripts == std::vector<std::string>{"", ""});
    rig.runtime->set_cleaner(nullptr);
}

TEST_CASE("test_separate_batch_cleanup_does_not_pass_a_batch_context",
          "[test_application_controller]") {
    // Separate must clean each file exactly like a single upload.
    UploadRig u(Json{{"transcript_cleanup_enabled", true}});
    auto& rig = u.rig;
    RecordingCleaner cleaner;
    rig.runtime->set_cleaner(&cleaner);
    const auto request = u.batch_request({"a.wav", "b.wav"});

    CHECK(rig.runtime->upload_audio_files(request));
    u.run_batch();

    const auto calls = cleaner.calls();
    REQUIRE(calls.size() == 2);
    for (const auto& call : calls) {
        CHECK_FALSE(call.batch_context);
        CHECK_THAT(call.system_prompt,
                   !ContainsSubstring(std::string(TRANSCRIPT_BATCH_CONTEXT_HEADER)));
        CHECK_FALSE(call.timeout_s);
    }
    CHECK(u.texts() == std::vector<std::string>{"Cleaned.", "Cleaned."});
    rig.runtime->set_cleaner(nullptr);
}

TEST_CASE("test_custom_batch_per_file_passes_instructions_to_each_cleanup",
          "[test_application_controller]") {
    UploadRig u(Json{{"transcript_cleanup_enabled", true}});
    auto& rig = u.rig;
    RecordingCleaner cleaner;
    rig.runtime->set_cleaner(&cleaner);
    const auto request = u.batch_request({"a.wav", "b.wav"}, BatchRelation::Custom,
                                         "Voice memos about the Q3 budget.", false);

    CHECK(rig.runtime->upload_audio_files(request));
    u.run_batch();

    const auto calls = cleaner.calls();
    REQUIRE(calls.size() == 2);
    for (const auto& call : calls) {
        CHECK_THAT(call.system_prompt, ContainsSubstring("Q3 budget"));
    }
    CHECK_THAT(calls[0].system_prompt, ContainsSubstring("This transcript is a.wav"));
    CHECK_THAT(calls[1].system_prompt, ContainsSubstring("This transcript is b.wav"));
    CHECK(rig.entries().size() == 2);
    rig.runtime->set_cleaner(nullptr);
}

TEST_CASE("test_cancel_mid_separate_batch_keeps_finished_files", "[test_application_controller]") {
    UploadRig u;
    auto& rig = u.rig;
    script_api(u, [&rig](const std::string& path) -> std::string {
        if (path.find("b.wav") != std::string::npos) {
            rig.runtime->cancel();
            throw fv::EngineError("Transcription canceled");
        }
        return "text:" + path;
    });
    const auto request = u.batch_request({"a.wav", "b.wav", "c.wav"});

    CHECK(rig.runtime->upload_audio_files(request));
    u.run_batch();

    CHECK(u.source_names() == std::vector<std::string>{"a.wav"});
    CHECK(rig.last_status() == "Canceled — kept 1 of 3 files");
    CHECK(rig.keys.sent().empty());
    CHECK_FALSE(rig.runtime->has_active_job());
}

TEST_CASE("test_cancel_mid_sequential_batch_discards_everything", "[test_application_controller]") {
    UploadRig u;
    auto& rig = u.rig;
    script_api(u, [&rig](const std::string& path) -> std::string {
        if (path.find("b.wav") != std::string::npos) {
            rig.runtime->cancel();
            throw fv::EngineError("Transcription canceled");
        }
        return "text:" + path;
    });
    const auto request = u.batch_request({"a.wav", "b.wav"}, BatchRelation::Sequential);

    CHECK(rig.runtime->upload_audio_files(request));
    u.run_batch();

    CHECK(rig.entries().empty());
    CHECK(u.error_dialogs() == std::vector<std::string>{"Error: Transcription canceled"});
    CHECK(u.transcripts().empty());
    CHECK_FALSE(rig.runtime->has_active_job());
}

TEST_CASE("test_batch_announces_each_large_file_the_backend_splits",
          "[test_application_controller]") {
    UploadRig u;
    auto& rig = u.rig;
    u.use_api(30.0);
    const auto request = u.batch_request({"a.wav", "b.wav"});

    CHECK(rig.runtime->upload_audio_files(request));
    u.run_batch();

    CHECK(u.large_file_states == std::vector<double>{30.0, 30.0});
    CHECK(u.texts() == std::vector<std::string>{"api chunks", "api chunks"});
}

TEST_CASE("test_batch_failure_in_one_file_keeps_the_others_in_separate_mode",
          "[test_application_controller]") {
    UploadRig u;
    auto& rig = u.rig;
    script_api(u, [](const std::string& path) -> std::string {
        if (path.find("b.wav") != std::string::npos) {
            throw fv::EngineError("boom");
        }
        return "text:" + path;
    });
    const auto request = u.batch_request({"a.wav", "b.wav", "c.wav"});

    CHECK(rig.runtime->upload_audio_files(request));
    u.run_batch();

    CHECK(u.source_names() == std::vector<std::string>{"a.wav", "c.wav"});
    CHECK_THAT(rig.transcript_text, ContainsSubstring("## b.wav\n\nError: boom"));
    CHECK(u.batch_item_events ==
          std::vector<std::pair<int, bool>>{{1, true}, {2, false}, {3, true}});
    CHECK_FALSE(rig.runtime->has_active_job());
}

TEST_CASE("test_batch_with_every_file_failing_reports_an_error", "[test_application_controller]") {
    UploadRig u;
    auto& rig = u.rig;
    script_api(u, [](const std::string&) -> std::string { throw fv::EngineError("boom"); });
    const auto request = u.batch_request({"a.wav", "b.wav"});

    CHECK(rig.runtime->upload_audio_files(request));
    u.run_batch();

    CHECK(rig.entries().empty());
    REQUIRE(u.error_dialogs().size() == 1);
    CHECK_THAT(u.error_dialogs()[0], StartsWith("Error: All 2 files failed"));
    CHECK(u.transcripts().empty());
    CHECK_FALSE(rig.runtime->has_active_job());
}

// ---- tests/test_local_transcription_contracts.py ----

TEST_CASE("test_local_engine_takes_a_file_over_the_api_limit_whole",
          "[test_local_transcription_contracts]") {
    // OpenAI's upload limit must not split, or announce, a local transcription.
    UploadRig u;
    auto& rig = u.rig;
    set_local_texts(u, {"all of it"});
    const auto audio = rig.wav("long-meeting.wav");
    std::filesystem::resize_file(audio, (23 + 1) * 1024 * 1024);
    REQUIRE(rig.runtime->claim_job());

    rig.runtime->transcribe_audio_file(audio);

    CHECK(u.large_file_states.empty());
    CHECK(contains(before(rig.trace, "overlay:", "transcript:"), "transcribing"));
    CHECK(before(rig.trace, "status:", "transcript:") ==
          std::vector<std::string>{"Transcribing..."});
    CHECK(u.transcripts() == std::vector<std::string>{"all of it"});
    CHECK_FALSE(rig.transcript_raw);
    rig.run_workers();
}

TEST_CASE("test_worker_job_shows_the_split_a_backend_reports",
          "[test_local_transcription_contracts]") {
    // The runtime only shows a split; the backend does it, inside its decode.
    UploadRig u;
    auto& rig = u.rig;
    u.engines.api->report_steps = false;
    u.use_api(30.0);
    const auto path = rig.root.path() / "saved.wav";
    {
        std::ofstream out(path, std::ios::binary);
        out << "RIFF" << std::string(64, '\0');
    }
    REQUIRE(rig.runtime->claim_job());

    rig.runtime->transcribe_audio_file(path);

    CHECK(u.engines.api->paths() == std::vector<std::string>{fv::base_utf8::path_to_utf8(path)});
    CHECK(u.large_file_states == std::vector<double>{30.0});
    CHECK(before(rig.trace, "status:", "transcript:") ==
          std::vector<std::string>{"Splitting large file (30.0 MB)..."});
    // The backend's own progress moves the overlay on once the split is done.
    CHECK_FALSE(contains(before(rig.trace, "overlay:", "transcript:"), "transcribing"));
    rig.runtime->report_backend_progress("Transcribing 3 chunks...", true);
    CHECK(u.overlays().back() == "transcribing");
    CHECK(rig.last_status() == "Transcribing 3 chunks...");
    rig.run_workers();
}

// ---- tests/test_paste_latency.py ----

TEST_CASE("test_upload_displays_ready_then_saves_history_and_never_pastes",
          "[test_paste_latency]") {
    RuntimeRig rig;
    // An upload holds the slot, so its result stays in the window; the pending recording is
    // _harness's.
    REQUIRE(rig.runtime->upload_audio_file(rig.wav("upload.wav"), 3.0));
    rig.runtime->set_pending({rig.wav("recording.wav"), 10.0, 4096, "Quick Record", 0.1});

    rig.runtime->on_transcription_complete("uploaded words");
    rig.persistence.run_all();

    const auto status = rig.trace.index("status:Transcript ready to copy");
    const auto history = rig.trace.index("history");
    const auto refresh = rig.trace.index("refresh_history");
    CHECK(status >= 0);
    CHECK(status < history);
    CHECK(history < refresh);
    CHECK(rig.last_status() == "Transcript ready to copy");
    CHECK_FALSE(rig.trace.has("stage"));
    CHECK_FALSE(rig.trace.has("copy"));
    CHECK(rig.keys.sent().empty());
    CHECK_FALSE(rig.runtime->has_active_job());
}

// ---- The runtime's own upload behaviour ----

TEST_CASE("a scripted three-file separate batch reports, saves and frees the slot",
          "[dictation][uploads]") {
    UploadRig u;
    auto& rig = u.rig;
    set_local_texts(u, {"one.", "two.", "three."});
    const auto request = u.batch_request({"a.wav", "b.wav", "c.wav"});

    CHECK(rig.runtime->upload_audio_files(request));
    CHECK(rig.last_status() == "Processing 3 uploaded files...");
    CHECK(u.overlays().back() == "processing");
    CHECK(rig.activity.snapshot().transcribing);
    u.run_batch();

    CHECK(u.batch_progress_events == std::vector<std::tuple<int, int, std::string>>{
                                         {1, 3, "a.wav"}, {2, 3, "b.wav"}, {3, 3, "c.wav"}});
    CHECK(u.batch_item_transcripts == std::vector<std::string>{"one.", "two.", "three."});
    CHECK(u.batches_completed == 1);
    CHECK(u.texts() == std::vector<std::string>{"one.", "two.", "three."});
    for (const auto& entry : rig.entries()) {
        // Only a combined batch's row is named "3 files: ...".
        CHECK_THAT(entry.source_name.value_or(""), !StartsWith("3 files"));
        CHECK(entry.model == "parakeet (Parakeet TDT 0.6B v3 | cpu)");
    }
    CHECK(rig.transcript_text == "## a.wav\n\none.\n\n## b.wav\n\ntwo.\n\n## c.wav\n\nthree.");
    CHECK_FALSE(rig.transcript_raw);
    CHECK(rig.keys.sent().empty());
    CHECK(rig.clipboard.inner.writes.empty());
    CHECK(rig.clipboard.inner.stages.empty());
    CHECK(rig.last_status() == "Ready");
    CHECK_FALSE(rig.runtime->has_active_job());
    CHECK_FALSE(rig.runtime->upload_job_active());
    CHECK_FALSE(rig.activity.snapshot().transcribing);
    // One completion and one save per file, each a file entry.
    CHECK(rig.events.of(events::TRANSCRIPTION_COMPLETED).size() == 3);
    const auto saved = rig.events.of(events::HISTORY_SAVED);
    REQUIRE(saved.size() == 3);
    for (const auto& event : saved) {
        CHECK(event.data.dump().find("\"file\"") != std::string::npos);
    }
}

TEST_CASE("a combined batch saves one row named after its files", "[dictation][uploads]") {
    UploadRig u;
    auto& rig = u.rig;
    set_local_texts(u, {"one.", "two.", "three."});
    const auto request =
        u.batch_request({"a.wav", "b.wav", "c.wav"}, BatchRelation::Custom, "Notes.", true);

    CHECK(rig.runtime->upload_audio_files(request));
    u.run_batch();

    const auto entries = rig.entries();
    REQUIRE(entries.size() == 1);
    CHECK(entries[0].source_name == "3 files: a.wav, b.wav, c.wav");
    CHECK(entries[0].text == "one.\n\ntwo.\n\nthree.");
    CHECK(entries[0].audio_duration == 30.0);
    CHECK(rig.transcript_text == "one.\n\ntwo.\n\nthree.");
    // With cleanup off nothing failed, so the status says nothing about it.
    CHECK(rig.last_status() == "Ready");
}

TEST_CASE("a combined cleanup failure shows the raw text and says so", "[dictation][uploads]") {
    UploadRig u(Json{{"transcript_cleanup_enabled", true}});
    auto& rig = u.rig;
    RecordingCleaner cleaner;
    cleaner.error = "timed out";
    rig.runtime->set_cleaner(&cleaner);
    const auto request = u.batch_request({"a.wav", "b.wav"}, BatchRelation::Sequential);

    CHECK(rig.runtime->upload_audio_files(request));
    u.run_batch();

    const auto entries = rig.entries();
    REQUIRE(entries.size() == 1);
    CHECK(entries[0].text == "Hello there.\n\nHello there.");
    CHECK_FALSE(entries[0].raw_text);
    CHECK_THAT(rig.last_status(), StartsWith("Ready"));
    CHECK_THAT(rig.last_status(), ContainsSubstring("AI cleanup failed (timed out)"));
    rig.runtime->set_cleaner(nullptr);
}

TEST_CASE("cleanup on without a provider falls back to raw text for every file",
          "[dictation][uploads]") {
    UploadRig u(Json{{"transcript_cleanup_enabled", true}});
    auto& rig = u.rig;
    const auto request = u.batch_request({"a.wav", "b.wav"}, BatchRelation::Sequential);

    CHECK(rig.runtime->upload_audio_files(request));
    u.run_batch();

    CHECK(u.texts() == std::vector<std::string>{"Hello there.\n\nHello there."});
    CHECK(rig.last_status() == "Ready — AI cleanup failed (cleanup unavailable); showing raw text");
}

TEST_CASE("a canceled batch never reaches the cleaner", "[dictation][uploads]") {
    UploadRig u(Json{{"transcript_cleanup_enabled", true}});
    auto& rig = u.rig;
    RecordingCleaner cleaner;
    // The Cancel lands after the first file's decode, as its cleanup is about to start.
    cleaner.on_available = [&rig] {
        rig.runtime->cancel();
    };
    rig.runtime->set_cleaner(&cleaner);
    const auto request = u.batch_request({"a.wav", "b.wav"});

    CHECK(rig.runtime->upload_audio_files(request));
    u.run_batch();

    CHECK(cleaner.calls().empty());
    // The first file keeps its words, uncleaned; the second never started.
    CHECK(u.texts() == std::vector<std::string>{"Hello there."});
    CHECK(u.batch_progress_events.size() == 1);
    CHECK(rig.last_status() == "Canceled — kept 1 of 2 files");
    CHECK_FALSE(rig.runtime->has_active_job());
    rig.runtime->set_cleaner(nullptr);
}

TEST_CASE("a combined batch stops at its first failed file", "[dictation][uploads]") {
    // One missing part invalidates a stitched transcript: the files after it never start.
    UploadRig u;
    auto& rig = u.rig;
    script_api(u, [](const std::string& path) -> std::string {
        if (path.find("b.wav") != std::string::npos) {
            throw fv::EngineError("boom");
        }
        return "text:" + path;
    });
    const auto request = u.batch_request({"a.wav", "b.wav", "c.wav"}, BatchRelation::Sequential);

    CHECK(rig.runtime->upload_audio_files(request));
    u.run_batch();

    CHECK(u.engines.api->paths().size() == 2);
    CHECK(u.batch_progress_events.size() == 2);
    CHECK(u.batches_completed == 0);
    CHECK(rig.entries().empty());
    CHECK(u.error_dialogs() == std::vector<std::string>{"Error: boom"});
    CHECK(u.transcripts().empty());
    CHECK_FALSE(rig.runtime->has_active_job());
    CHECK_FALSE(rig.runtime->upload_job_active());
}

TEST_CASE("a combined batch canceled after its last file keeps the transcript uncleaned",
          "[dictation][uploads]") {
    UploadRig u(Json{{"transcript_cleanup_enabled", true}});
    auto& rig = u.rig;
    RecordingCleaner cleaner;
    // Every file is decoded; only the combined cleanup is left when the Cancel lands. (A Cancel
    // during a decode ends it: the speech service drops a canceled job's result.)
    cleaner.on_available = [&rig] {
        rig.runtime->cancel();
    };
    rig.runtime->set_cleaner(&cleaner);
    set_local_texts(u, {"first part.", "second part."});
    const auto request = u.batch_request({"a.wav", "b.wav"}, BatchRelation::Sequential);

    CHECK(rig.runtime->upload_audio_files(request));
    u.run_batch();

    CHECK(cleaner.calls().empty());
    const auto entries = rig.entries();
    REQUIRE(entries.size() == 1);
    CHECK(entries[0].text == "first part.\n\nsecond part.");
    CHECK_FALSE(entries[0].raw_text);
    CHECK_FALSE(entries[0].cleanup_provider);
    CHECK(rig.last_status() == "Canceled — kept the transcript without AI cleanup");
    CHECK(u.error_dialogs().empty());
    CHECK_FALSE(rig.runtime->has_active_job());
    rig.runtime->set_cleaner(nullptr);
}

TEST_CASE("an upload's result never goes to a focused Scratchpad", "[dictation][uploads]") {
    RuntimeRig rig;
    FakeScratchpad pad;
    pad.focused = true;
    rig.runtime->set_scratchpad(&pad);

    REQUIRE(rig.runtime->upload_audio_file(rig.wav("a.wav"), 1.0));
    rig.run_workers();

    CHECK(pad.inserted.empty());
    CHECK(rig.keys.sent().empty());
    CHECK(rig.last_status() == "Ready");
    CHECK(rig.entries().size() == 1);
    rig.runtime->set_scratchpad(nullptr);
}

TEST_CASE("a single upload runs the cleaner and saves both texts", "[dictation][uploads]") {
    UploadRig u(Json{{"transcript_cleanup_enabled", true}});
    auto& rig = u.rig;
    RecordingCleaner cleaner;
    rig.runtime->set_cleaner(&cleaner);
    std::optional<CleanupInfo> shown;
    auto link = rig.runtime->transcription_completed.connect(
        rig.ui, [&shown](std::string, std::optional<std::string>, std::optional<CleanupInfo> info) {
            shown = info;
        });

    REQUIRE(rig.runtime->upload_audio_file(rig.wav("a.wav"), 1.0));
    rig.run_workers();

    CHECK(contains(rig.statuses(), "Cleaning up..."));
    CHECK(contains(u.overlays(), "cleaning"));
    const auto entries = rig.entries();
    REQUIRE(entries.size() == 1);
    CHECK(entries[0].text == "Cleaned.");
    CHECK(entries[0].raw_text == "Hello there.");
    CHECK(entries[0].cleanup_provider == "openai");
    CHECK(entries[0].cleanup_model == "gpt-4o-mini");
    REQUIRE(shown);
    CHECK(shown->provider == "openai");
    CHECK(shown->level == "medium");
    CHECK(rig.last_status() == "Ready");
    link.disconnect();
    rig.runtime->set_cleaner(nullptr);
}

TEST_CASE("uploads are refused during a meeting, for missing files and with nothing to do",
          "[dictation][uploads]") {
    UploadRig u;
    auto& rig = u.rig;

    CHECK_FALSE(rig.runtime->upload_audio_files(BatchUploadRequest{}));
    CHECK(rig.last_status() == "No audio files to transcribe");

    CHECK_FALSE(rig.runtime->upload_audio_file(rig.root.path() / "gone.wav", 1.0));
    CHECK(rig.last_status() == "Error: Audio file not found");
    CHECK(u.overlays().back() == "none");

    auto request = u.batch_request({"a.wav", "b.wav"});
    request.items[1].audio_path = fv::base_utf8::path_to_utf8(rig.root.path() / "gone.wav");
    CHECK_FALSE(rig.runtime->upload_audio_files(request));
    CHECK(rig.last_status() == "Error: Audio file not found: gone.wav");

    rig.activity.set_meeting_active(true);
    CHECK_FALSE(rig.runtime->upload_audio_file(rig.wav("c.wav"), 1.0));
    CHECK(rig.last_status() == "Meeting Mode is active — end it before uploading audio");
    CHECK_FALSE(rig.runtime->upload_audio_files(u.batch_request({"a.wav", "b.wav"})));
    CHECK(rig.last_status() == "Meeting Mode is active — end it before uploading audio");
    rig.activity.set_meeting_active(false);

    rig.recorder.recording = true;
    CHECK_FALSE(rig.runtime->upload_audio_files(u.batch_request({"a.wav", "b.wav"})));
    CHECK_THAT(rig.last_status(), ContainsSubstring("wait before uploading audio"));
    rig.recorder.recording = false;

    CHECK(rig.jobs.queued() == 0);
    CHECK_FALSE(rig.runtime->has_active_job());
}

TEST_CASE("an upload with the speech model missing asks for consent and frees the slot",
          "[dictation][uploads]") {
    UploadRig u;
    auto& rig = u.rig;
    auto script = Engines::local_script();
    script.readiness = {fv::speech::ReadyState::ModelMissing,
                        "Download Parakeet TDT 0.6B v3 in Downloads.", ""};
    u.engines.local->set_script(script);
    rig.load();
    int asked = 0;
    rig.runtime->set_model_consent_hook([&asked] { ++asked; });

    CHECK_FALSE(rig.runtime->upload_audio_files(u.batch_request({"a.wav", "b.wav"})));

    CHECK(asked == 1);
    CHECK_FALSE(rig.trace.has("request_model_consent"));
    CHECK(u.error_dialogs() ==
          std::vector<std::string>{"Error: Download Parakeet TDT 0.6B v3 in Downloads."});
    CHECK(rig.jobs.queued() == 0);
    CHECK_FALSE(rig.runtime->has_active_job());
    CHECK_FALSE(rig.runtime->upload_job_active());
}

TEST_CASE("a focused Scratchpad takes the dictation instead of the paste", "[dictation][uploads]") {
    RuntimeRig rig;
    FakeScratchpad pad;
    rig.runtime->set_scratchpad(&pad);

    // Not the active window: the normal paste path runs.
    rig.runtime->on_transcription_complete("first words");
    rig.run_workers();
    CHECK(pad.inserted.empty());
    CHECK(rig.keys.sent() == std::vector<std::string>{"paste"});
    CHECK(rig.last_status() == std::string(READY_PASTED));

    pad.focused = true;
    rig.runtime->on_transcription_complete("second words");
    rig.run_workers();
    CHECK(pad.inserted == std::vector<std::string>{"second words"});
    CHECK(rig.keys.sent() == std::vector<std::string>{"paste"});
    CHECK(rig.last_status() == std::string(ADDED_TO_SCRATCHPAD));
    CHECK(rig.entries().size() == 2);
    rig.runtime->set_scratchpad(nullptr);
}

TEST_CASE("the engine prepares for transcription as a recording starts", "[dictation][uploads]") {
    int prepared = 0;
    RigOptions options;
    options.hooks = [&prepared](RuntimeHooks& hooks) {
        hooks.prepare_for_transcription = [&prepared] {
            ++prepared;
        };
    };
    RuntimeRig rig(std::move(options));

    REQUIRE(rig.runtime->start_recording());
    CHECK(prepared == 1);
    rig.runtime->cancel();
    CHECK(prepared == 1);
}

TEST_CASE("an upload marks the slot as an upload job until it is saved", "[dictation][uploads]") {
    RuntimeRig rig;
    CHECK_FALSE(rig.runtime->upload_job_active());
    REQUIRE(rig.runtime->upload_audio_file(rig.wav("a.wav"), 1.0));
    CHECK(rig.runtime->upload_job_active());
    rig.jobs.run_all();
    CHECK(rig.runtime->upload_job_active()); // the History save still holds the slot
    rig.run_workers();
    CHECK_FALSE(rig.runtime->upload_job_active());
    CHECK(rig.entries()[0].source_name == "a.wav");
    CHECK(rig.entries()[0].audio_duration == 1.0);
}

// ---- The status builders over hand-made results ----

TEST_CASE("batch_status, batch_display and batch_line", "[dictation][uploads]") {
    BatchResult result;
    result.request.items = {BatchItem{"C:/x/a.wav", 1.0}, BatchItem{"C:/x/b.wav", 2.0}};
    BatchItemResult a;
    a.item = result.request.items[0];
    a.text = " words ";
    a.raw_text = "um words";
    BatchItemResult b;
    b.item = result.request.items[1];
    b.error = "boom";
    result.items = {a, b};

    CHECK(batch_line(a) == "words");
    CHECK(batch_line(a, true) == "um words");
    CHECK(batch_line(b) == "Error: boom");
    const auto [display, raw] = batch_display(result, false);
    CHECK(display == "## a.wav\n\nwords\n\n## b.wav\n\nError: boom");
    CHECK(raw == "## a.wav\n\num words\n\n## b.wav\n\nError: boom");
    CHECK(batch_status(result, 1, true) == "Canceled — kept 1 of 2 files");
    result.cleanup_error = std::string(CLEANUP_CANCELED_REASON);
    CHECK(batch_status(result, 1, false) == "Ready without AI cleanup");
    result.request.relation = BatchRelation::Sequential;
    CHECK(batch_status(result, 0, true) == "Canceled — kept the transcript without AI cleanup");
    CHECK(batch_display(result, true).first == std::string(EMPTY_ASR_MESSAGE));
}
