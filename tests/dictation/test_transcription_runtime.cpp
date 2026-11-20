#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <filesystem>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "dictation/messages.h"
#include "dictation/transcription_runtime.h"
#include "history/entry.h"
#include "runtime_support.h"

// tests/test_application_controller.py (the transcription runtime's part) and
// tests/test_local_transcription_contracts.py::test_saved_recording_dispatch_guards_and_releases_claim.

using namespace fv::dictation;
using namespace fv::dictation::testing;
using Catch::Matchers::ContainsSubstring;
using Catch::Matchers::StartsWith;

namespace {

std::int64_t fixture_size() {
    return static_cast<std::int64_t>(
        std::filesystem::file_size(audio_fixture("tone-16k-mono.wav")));
}

std::size_t decodes(const RuntimeRig& rig) {
    std::size_t count = 0;
    for (const auto& op : rig.engine->ops()) {
        count += op == "decode" ? 1 : 0;
    }
    return count;
}

// A failed recording kept in History, as _save_failed_recording leaves one: its saved audio's
// path and its id.
std::pair<std::filesystem::path, std::string> failed_entry(RuntimeRig& rig) {
    fv::AddEntry fields;
    fields.text = "";
    fields.model = "parakeet (Fake | cpu)";
    fields.source_audio_path = rig.wav("failed.wav");
    fields.audio_duration = 2.0;
    fields.source_name = "Quick Record";
    fields.columns["entry_kind"] = std::string(fv::FAILED_RECORDING_KIND);
    const auto entry = rig.history->add_entry(fields);
    REQUIRE(entry);
    REQUIRE(entry->audio_file);
    return {rig.history->recordings_folder() / *entry->audio_file, entry->id};
}

} // namespace

TEST_CASE("transcription complete saves history and resets pending state",
          "[test_application_controller]") {
    // from
    // tests/test_application_controller.py::test_transcription_complete_saves_history_and_resets_pending_state
    RuntimeRig rig;
    const auto source = rig.wav("source.wav");
    rig.runtime->set_pending({source, 9.5, 2048, std::nullopt, 1.0});

    rig.runtime->on_transcription_complete("hello world");
    rig.run_workers();

    const auto entries = rig.entries();
    REQUIRE(entries.size() == 1);
    CHECK(entries[0].text == "hello world");
    CHECK_FALSE(entries[0].raw_text);
    CHECK(entries[0].audio_file); // the source was copied into Recordings
    CHECK(entries[0].audio_duration == 9.5);
    CHECK(entries[0].file_size == 2048);
    CHECK(rig.trace.has("refresh_history"));
    CHECK(rig.clipboard.inner.text() == "hello world");
    CHECK(rig.runtime->pending() == PendingAudio{});
}

TEST_CASE("start recording failure stays idle", "[test_application_controller]") {
    // from tests/test_application_controller.py::test_start_recording_failure_stays_idle
    RuntimeRig rig;
    rig.recorder.start_ok = false;
    rig.recorder.start_error = "No audio device available";

    CHECK_FALSE(rig.runtime->start_recording());

    CHECK_FALSE(rig.recorder.is_recording());
    CHECK(rig.trace.has("recording:off"));
    CHECK_FALSE(rig.activity.snapshot().recording);
    CHECK(rig.last_status() == "Failed to start recording: No audio device available");
}

TEST_CASE("empty asr skips history clipboard and paste", "[test_application_controller]") {
    // from tests/test_application_controller.py::test_empty_asr_skips_history_clipboard_and_paste
    RuntimeRig rig;
    rig.runtime->set_pending({rig.wav("source.wav"), 3.7, 2048, "sample.wav", 1.0});

    rig.runtime->on_transcription_complete("   ");
    rig.run_workers();

    CHECK(rig.entries().empty());
    CHECK(rig.clipboard.inner.stages.empty());
    CHECK(rig.clipboard.inner.writes.empty());
    CHECK(rig.keys.sent().empty());
    CHECK(rig.transcript_text == "No speech detected (empty after VAD)");
    CHECK(rig.last_status() == "No speech detected (empty after VAD)");
    REQUIRE(rig.stats_shown);
    CHECK(rig.stats_shown->audio_duration == 3.7);
    CHECK(rig.runtime->pending() == PendingAudio{});
}

TEST_CASE("stop recording without streaming keeps no preview", "[test_application_controller]") {
    // from
    // tests/test_application_controller.py::test_stop_recording_without_streaming_keeps_no_preview
    RuntimeRig rig;
    REQUIRE(rig.runtime->start_recording());

    rig.runtime->stop_recording();

    CHECK(rig.runtime->pending_streaming_text().empty());
}

TEST_CASE("transcription failure keeps recording in history", "[test_application_controller]") {
    // from
    // tests/test_application_controller.py::test_transcription_failure_keeps_recording_in_history
    RuntimeRig rig;
    const auto source = rig.wav("failed-quick-record.wav");
    PendingAudio pending;
    pending.audio_path = source;
    pending.source_name = "Quick Record";
    rig.runtime->set_pending(pending);
    REQUIRE(rig.runtime->claim_job());

    rig.runtime->on_transcription_error("backend unavailable");
    rig.run_workers();

    const auto entries = rig.entries();
    REQUIRE(entries.size() == 1);
    CHECK(entries[0].entry_kind == std::string(fv::FAILED_RECORDING_KIND));
    CHECK(entries[0].text.empty());
    CHECK(entries[0].audio_file);
    CHECK(rig.trace.has("refresh_history"));
    CHECK_THAT(rig.last_status(), ContainsSubstring("Retry transcription"));
    CHECK(rig.clipboard.inner.writes.empty());
    CHECK(rig.clipboard.inner.stages.empty());
    CHECK(rig.runtime->pending() == PendingAudio{});
    CHECK_FALSE(rig.runtime->has_active_job());
}

TEST_CASE("retry from history reuses the failed entry", "[test_application_controller]") {
    // from tests/test_application_controller.py::test_retry_from_history_reuses_the_failed_entry
    const std::optional<std::string> result =
        GENERATE(std::optional<std::string>("recovered words"), std::optional<std::string>(),
                 std::optional<std::string>(""));
    RigOptions options;
    if (result) {
        options.script.texts = {*result};
    } else {
        options.script.decode_error = "still offline";
    }
    RuntimeRig rig(options);
    const auto [audio, id] = failed_entry(rig);

    rig.runtime->retranscribe_audio(audio);
    rig.run_workers();

    const auto entries = rig.entries();
    REQUIRE(entries.size() == 1); // no new entry
    CHECK(rig.clipboard.inner.stages.empty());
    CHECK(rig.clipboard.inner.writes.empty());
    CHECK_FALSE(rig.runtime->has_active_job());
    const auto entry = rig.history->get_entry(id);
    REQUIRE(entry);
    if (result && !result->empty()) {
        CHECK(entry->text == *result);
        CHECK(entry->entry_kind == std::string(FILE_ENTRY_KIND));
        CHECK(entry->audio_file); // the retry keeps the recording
    } else {
        CHECK(entry->text.empty());
        CHECK(entry->entry_kind == std::string(fv::FAILED_RECORDING_KIND));
        CHECK_THAT(rig.last_status(), ContainsSubstring("recording kept in History"));
    }
}

TEST_CASE("copy failure does not claim pasted", "[test_application_controller]") {
    // from tests/test_application_controller.py::test_copy_failure_does_not_claim_pasted
    RuntimeRig rig;
    rig.clipboard.inner.fail_writes = true;

    rig.runtime->on_transcription_complete("hello world");
    rig.run_workers();

    CHECK(rig.keys.sent().empty());
    CHECK(rig.last_status() == "Transcription complete (copy failed)");
    CHECK(rig.entries().size() == 1);
}

TEST_CASE("auto paste schedules clipboard restore even when copy is enabled",
          "[test_application_controller]") {
    // from
    // tests/test_application_controller.py::test_auto_paste_schedules_clipboard_restore_even_when_copy_is_enabled
    RuntimeRig rig;

    rig.runtime->on_transcription_complete("hello world");

    CHECK(rig.keys.sent() == std::vector<std::string>{"paste"});
    REQUIRE(rig.clipboard.inner.stages.size() == 1);
    CHECK(rig.clipboard.inner.stages[0].text == "hello world");
    CHECK(rig.clipboard.inner.restore_delays ==
          std::vector<std::chrono::milliseconds>{fv::focus::CLIPBOARD_RESTORE_DELAY});
    CHECK_FALSE(rig.trace.has("commit"));
    CHECK(rig.last_status() == "Ready (Pasted)");
}

TEST_CASE("auto paste with restore off leaves transcript on clipboard",
          "[test_application_controller]") {
    // from
    // tests/test_application_controller.py::test_auto_paste_with_restore_off_leaves_transcript_on_clipboard
    RigOptions options;
    options.settings["restore_clipboard"] = false;
    RuntimeRig rig(options);

    rig.runtime->on_transcription_complete("hello world");

    CHECK(rig.keys.sent() == std::vector<std::string>{"paste"});
    CHECK(rig.clipboard.inner.text() == "hello world");
    CHECK(rig.clipboard.inner.restore_delays.empty());
    CHECK(rig.last_status() == "Ready (Pasted)");
}

TEST_CASE("auto paste leaves transcript when clipboard started blank",
          "[test_application_controller]") {
    // from
    // tests/test_application_controller.py::test_auto_paste_leaves_transcript_when_clipboard_started_blank
    RuntimeRig rig;
    rig.clipboard.inner.external_copy("");

    rig.runtime->on_transcription_complete("hello world");

    CHECK(rig.keys.sent() == std::vector<std::string>{"paste"});
    CHECK(rig.clipboard.inner.text() == "hello world");
    CHECK(rig.clipboard.inner.restore_delays.empty());
    CHECK(rig.last_status() == "Ready (Pasted)");
}

TEST_CASE("auto paste reports when clipboard snapshot is unavailable",
          "[test_application_controller]") {
    // from
    // tests/test_application_controller.py::test_auto_paste_reports_when_clipboard_snapshot_is_unavailable
    RuntimeRig rig;
    rig.clipboard.restore_unavailable = true;

    rig.runtime->on_transcription_complete("hello world");

    CHECK(rig.keys.sent() == std::vector<std::string>{"paste"});
    CHECK(rig.clipboard.inner.restore_delays.empty());
    CHECK(rig.last_status() == "Ready (Pasted; clipboard restore unavailable)");
}

TEST_CASE("auto paste failure keeps transcript and cancels restore",
          "[test_application_controller]") {
    // from
    // tests/test_application_controller.py::test_auto_paste_failure_keeps_transcript_and_cancels_restore
    RuntimeRig rig;
    rig.keys.fail = true;

    rig.runtime->on_transcription_complete("hello world");

    CHECK(rig.trace.count("commit") == 1);
    CHECK(rig.clipboard.inner.text() == "hello world");
    CHECK_FALSE(rig.clipboard.inner.restore_pending());
    CHECK(rig.clipboard.inner.restore_delays.empty());
    CHECK(rig.last_status() == "Transcription complete (paste failed)");
}

TEST_CASE("recording start is refused while a job is queued", "[test_application_controller]") {
    // from
    // tests/test_application_controller.py::test_recording_start_is_refused_while_a_job_is_queued
    RuntimeRig rig;
    REQUIRE(rig.runtime->claim_job()); // an upload holds the slot
    REQUIRE(rig.runtime->has_active_job());

    CHECK_FALSE(rig.runtime->start_recording());

    CHECK_FALSE(rig.recorder.is_recording());
    CHECK_THAT(rig.last_status(), ContainsSubstring("already in progress"));
    REQUIRE_FALSE(rig.notices().empty());
    CHECK_THAT(rig.notices().back(), StartsWith("A transcription is already in progress"));
}

TEST_CASE("stop recording does not block the qt thread", "[test_application_controller]") {
    // from tests/test_application_controller.py::test_stop_recording_does_not_block_the_qt_thread
    RuntimeRig rig;
    REQUIRE(rig.runtime->start_recording());

    rig.runtime->stop_recording();

    CHECK_FALSE(rig.trace.has("waited")); // the post-roll wait ran on the UI thread
    CHECK_FALSE(rig.trace.has("saved"));  // the WAV write ran on the UI thread
    CHECK(rig.jobs.queued() == 1);

    rig.run_workers();

    REQUIRE(rig.trace.has("saved"));
    CHECK(rig.trace.index("waited") < rig.trace.index("saved"));
}

TEST_CASE("stopped recording claims job before post roll finishes",
          "[test_application_controller]") {
    // from
    // tests/test_application_controller.py::test_stopped_recording_claims_job_before_post_roll_finishes
    RuntimeRig rig;
    const auto upload = rig.wav("racing-upload.wav");
    REQUIRE(rig.runtime->start_recording());
    bool upload_claimed = true;
    // An upload arrives while the post-roll runs: the slot is already the recording's.
    rig.recorder.on_wait = [&] {
        upload_claimed = rig.runtime->claim_job();
    };
    rig.engine->hold();

    rig.runtime->stop_recording();
    REQUIRE(rig.jobs.queued() == 1);
    std::thread worker([&] { rig.jobs.run_all(); });
    REQUIRE(rig.engine->wait_entered(std::chrono::seconds(5)));

    // At hand-off to the decode, the recording's own metadata is pending.
    const PendingAudio pending = rig.runtime->pending();
    rig.engine->resume();
    worker.join();
    rig.run_workers();

    CHECK_FALSE(upload_claimed);
    CHECK(pending.source_name == "Quick Record");
    CHECK(pending.audio_duration == 4.0);
    CHECK(pending.file_size == fixture_size());
    CHECK(pending.audio_path == rig.recorder.output_file());
}

TEST_CASE("transcribe audio file skips cleanup when disabled", "[test_application_controller]") {
    // from
    // tests/test_application_controller.py::test_transcribe_audio_file_skips_cleanup_when_disabled
    RigOptions options;
    options.settings["transcript_cleanup_enabled"] = false;
    options.script.texts = {"raw only"};
    RuntimeRig rig(options);
    const auto clip = rig.wav("clip.wav");

    rig.runtime->transcribe_audio_file(clip);
    rig.run_workers();

    const auto entries = rig.entries();
    REQUIRE(entries.size() == 1);
    CHECK(entries[0].text == "raw only");
    CHECK_FALSE(entries[0].raw_text);
    CHECK_FALSE(entries[0].cleanup_provider);
    CHECK_FALSE(entries[0].cleanup_model);
}

TEST_CASE("history persistence is queued and keeps recording slot",
          "[test_application_controller]") {
    // from
    // tests/test_application_controller.py::test_history_persistence_is_queued_and_keeps_recording_slot
    RuntimeRig rig;
    REQUIRE(rig.runtime->claim_job());
    PendingAudio pending;
    pending.audio_path = rig.wav("source.wav");
    pending.source_name = "original.wav";
    rig.runtime->set_pending(pending);

    rig.runtime->on_transcription_complete("keep these words");

    CHECK(rig.entries().empty());
    CHECK(rig.transcript_text == "keep these words");
    CHECK(rig.clipboard.inner.text() == "keep these words");
    CHECK(rig.runtime->has_active_job());
    CHECK_FALSE(rig.runtime->start_recording());

    rig.persistence.run_all();

    const auto entries = rig.entries();
    REQUIRE(entries.size() == 1);
    CHECK(entries[0].source_name == "original.wav");
    CHECK_FALSE(rig.runtime->has_active_job());
}

TEST_CASE("history failure is visible and keeps transcript", "[test_application_controller]") {
    // from
    // tests/test_application_controller.py::test_history_failure_is_visible_and_keeps_transcript
    RuntimeRig rig;
    REQUIRE(rig.runtime->claim_job());
    rig.break_history();

    rig.runtime->on_transcription_complete("recover this text");
    rig.run_workers();

    CHECK_THAT(rig.last_status(), StartsWith("History could not be saved"));
    CHECK(rig.transcript_text == "recover this text");
    CHECK(rig.clipboard.inner.text() == "recover this text");
    CHECK_FALSE(rig.runtime->has_active_job());
}

TEST_CASE("capture failure preserves audio without transcribing", "[test_application_controller]") {
    // from
    // tests/test_application_controller.py::test_capture_failure_preserves_audio_without_transcribing
    RuntimeRig rig;
    rig.recorder.set_capture_error("microphone disconnected");

    rig.runtime->on_capture_error("microphone disconnected");

    CHECK(rig.runtime->has_active_job());
    rig.run_workers();
    CHECK(decodes(rig) == 0);
    const auto entries = rig.entries();
    REQUIRE(entries.size() == 1);
    CHECK(entries[0].entry_kind == std::string(fv::FAILED_RECORDING_KIND));
    CHECK_THAT(rig.last_status(), ContainsSubstring("microphone disconnected"));
    CHECK_FALSE(rig.runtime->has_active_job());
}

TEST_CASE("stop timeout preserves journal and does not transcribe",
          "[test_application_controller]") {
    // from
    // tests/test_application_controller.py::test_stop_timeout_preserves_journal_and_does_not_transcribe
    RuntimeRig rig;
    rig.recorder.wait_ok = false;
    REQUIRE(rig.runtime->claim_job());

    rig.runtime->finish_recording_job();

    CHECK(decodes(rig) == 0);
    CHECK_FALSE(rig.trace.has("saved"));
    CHECK(rig.recorder.acknowledged == 0); // the journal stays for recovery
    CHECK(rig.trace.has("fail_capture:Recording did not finish stopping; captured audio was kept "
                        "for recovery."));
    CHECK_THAT(rig.last_status(), ContainsSubstring("did not finish stopping"));
    CHECK_FALSE(rig.runtime->has_active_job());
}

TEST_CASE("queued capture error after cancel does not stop new recording",
          "[test_application_controller]") {
    // from
    // tests/test_application_controller.py::test_queued_capture_error_after_cancel_does_not_stop_new_recording
    RuntimeRig rig;
    REQUIRE(rig.runtime->start_recording());

    rig.runtime->on_capture_error("old microphone error");

    CHECK(rig.recorder.is_recording());
    CHECK(rig.jobs.queued() == 0);
    CHECK_FALSE(rig.runtime->has_active_job());
}

TEST_CASE("history refresh failure releases recording slot", "[test_application_controller]") {
    // from
    // tests/test_application_controller.py::test_history_refresh_failure_releases_recording_slot
    RuntimeRig rig;
    REQUIRE(rig.runtime->claim_job());
    rig.refresh_error = "view closed";

    rig.runtime->on_transcription_complete("saved despite view failure");
    rig.run_workers();

    const auto entries = rig.entries();
    REQUIRE(entries.size() == 1);
    CHECK(entries[0].text == "saved despite view failure");
    CHECK_FALSE(rig.runtime->has_active_job());
    CHECK(rig.runtime->pending() == PendingAudio{});
}

TEST_CASE("shutdown refuses new jobs but releases finished job", "[test_application_controller]") {
    // from
    // tests/test_application_controller.py::test_shutdown_refuses_new_jobs_but_releases_finished_job
    RuntimeRig rig;
    REQUIRE(rig.runtime->claim_job());
    rig.activity.set_shutting_down(true);

    rig.runtime->finish_job();

    CHECK_FALSE(rig.runtime->has_active_job());
    CHECK_FALSE(rig.runtime->claim_job());
}

TEST_CASE("saved recording dispatch guards and releases claim",
          "[test_local_transcription_contracts]") {
    // from
    // tests/test_local_transcription_contracts.py::test_saved_recording_dispatch_guards_and_releases_claim
    const std::string condition =
        GENERATE("meeting", "missing", "recording", "busy", "ready", "submit_error");
    RuntimeRig rig;
    const auto path = rig.root.path() / "saved.wav";
    if (condition != "missing") {
        rig.wav("saved.wav");
    }
    if (condition == "meeting") {
        rig.activity.set_meeting_active(true);
    } else if (condition == "recording") {
        rig.recorder.recording = true;
    } else if (condition == "busy") {
        REQUIRE(rig.runtime->claim_job());
    } else if (condition == "ready") {
        rig.engine->hold();
    }

    rig.runtime->retranscribe_audio(path);
    if (condition == "submit_error") {
        // Gone before the worker reads it: the dispatch fails where Python's submit raised.
        std::filesystem::remove(path);
    }

    if (condition != "ready" && condition != "submit_error") {
        CHECK(rig.jobs.queued() == 0);
        CHECK(rig.runtime->has_active_job() == (condition == "busy"));
        return;
    }
    REQUIRE(rig.jobs.queued() == 1);
    if (condition == "ready") {
        std::thread worker([&] { rig.jobs.run_all(); });
        REQUIRE(rig.engine->wait_entered(std::chrono::seconds(5)));
        CHECK(rig.runtime->has_active_job());
        const PendingAudio pending = rig.runtime->pending();
        CHECK(pending.source_name == "saved.wav");
        CHECK(pending.file_size == fixture_size());
        rig.engine->resume();
        worker.join();
        rig.run_workers();
        CHECK(decodes(rig) == 1);
    } else {
        rig.run_workers();
        CHECK_THAT(rig.last_status(), StartsWith("Error: Failed to process audio: "));
        CHECK_FALSE(rig.runtime->pending().source_name);
        CHECK_FALSE(rig.runtime->pending().file_size);
        CHECK_FALSE(rig.runtime->has_active_job());
        CHECK(decodes(rig) == 0);
    }
}

TEST_CASE("a recording publishes its events and a History entry with its context",
          "[dictation][events]") {
    RuntimeRig rig;
    REQUIRE(rig.runtime->start_recording());
    rig.runtime->stop_recording();
    rig.run_workers();

    const auto started = rig.events.of("recording.started");
    REQUIRE(started.size() == 1);
    const std::string job_id = started[0].data["job_id"].get<std::string>();
    CHECK(job_id.size() == 16);
    CHECK(started[0].data == fv::Json{{"job_id", job_id},
                                      {"mode", "dictation"},
                                      {"action_id", nullptr},
                                      {"source", "hotkey"},
                                      {"route", ""}});
    const auto stopped = rig.events.of("recording.stopped");
    REQUIRE(stopped.size() == 1);
    CHECK(stopped[0].data == fv::Json{{"job_id", job_id}, {"duration_s", 4.0}});
    const auto completed = rig.events.of("transcription.completed");
    REQUIRE(completed.size() == 1);
    CHECK(completed[0].data["text"] == "Hello there.");
    CHECK(completed[0].data["mode"] == "dictation");
    CHECK(completed[0].data["raw_text"].is_null());
    CHECK(completed[0].data["audio_s"] == 4.0);
    const auto saved = rig.events.of("history.saved");
    REQUIRE(saved.size() == 1);
    CHECK(saved[0].data["job_id"] == job_id);
    CHECK(saved[0].data["kind"] == "dictation");

    const auto entries = rig.entries();
    REQUIRE(entries.size() == 1);
    CHECK(entries[0].source_name == "Quick Record");
    CHECK(entries[0].entry_kind == "dictation");
    CHECK(entries[0].model == "parakeet (Parakeet TDT 0.6B v3 | cpu)");
    CHECK(rig.recorder.acknowledged == 1);
    CHECK(rig.stats->rows().size() == 1);
}
