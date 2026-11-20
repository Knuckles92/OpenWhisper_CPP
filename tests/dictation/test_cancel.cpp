#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <string>
#include <thread>

#include "dictation/transcription_runtime.h"
#include "preview_support.h"
#include "runtime_support.h"

// tests/test_cancel_after_stop.py: a cancel after the stop press must never paste.

using namespace fv::dictation;
using namespace fv::dictation::testing;

namespace {

// _runtime: a recording whose capture is still running, a saved WAV and an engine that hears
// "Hello there.".
struct CancelRig : RuntimeRig {
    CancelRig() { recorder.recording = true; }

    std::size_t decodes() const {
        std::size_t count = 0;
        for (const auto& op : engine->ops()) {
            count += op == "decode" ? 1 : 0;
        }
        return count;
    }
    bool completed() const { return !trace.with_prefix("transcript:").empty(); }
    bool failed() const { return !trace.with_prefix("error_dialog:").empty(); }
};

} // namespace

TEST_CASE("cancel during post roll transcribes nothing", "[test_cancel_after_stop]") {
    // from tests/test_cancel_after_stop.py::test_cancel_during_post_roll_transcribes_nothing
    CancelRig rig;
    REQUIRE(rig.runtime->claim_job()); // what stop_recording does before the post-roll

    rig.runtime->cancel(); // the recorder is still capturing the post-roll
    rig.runtime->finish_recording_job();

    CHECK_FALSE(rig.trace.has("saved"));
    CHECK_FALSE(rig.completed());
    CHECK_FALSE(rig.failed());
    CHECK_FALSE(rig.runtime->has_active_job());
    const auto failures = rig.events.of("transcription.failed");
    REQUIRE(failures.size() == 1);
    CHECK(failures[0].data["error"] == "canceled");
}

TEST_CASE("cancel before any stop leaves the next job alone", "[test_cancel_after_stop]") {
    // from tests/test_cancel_after_stop.py::test_cancel_before_any_stop_leaves_the_next_job_alone
    CancelRig rig;

    rig.runtime->cancel(); // an ordinary cancel mid-recording
    CHECK_FALSE(rig.runtime->job_slot().cancel_requested());

    REQUIRE(rig.runtime->claim_job());
    rig.runtime->finish_recording_job();

    CHECK(rig.transcript_text == "Hello there.");
}

TEST_CASE("uncanceled dictation still completes", "[test_cancel_after_stop]") {
    // from tests/test_cancel_after_stop.py::test_uncanceled_dictation_still_completes
    CancelRig rig;
    REQUIRE(rig.runtime->claim_job());

    rig.runtime->finish_recording_job();

    CHECK(rig.trace.count("saved") == 1);
    CHECK(rig.transcript_text == "Hello there.");
}

TEST_CASE("cancel before transcription starts skips the engine", "[test_cancel_after_stop]") {
    // from
    // tests/test_cancel_after_stop.py::test_cancel_before_transcription_starts_skips_the_engine
    CancelRig rig;
    REQUIRE(rig.runtime->claim_job());
    // Cancel pressed while the WAV saves.
    rig.recorder.on_save = [&] {
        rig.runtime->job_slot().cancel();
    };

    rig.runtime->finish_recording_job();

    CHECK(rig.decodes() == 0);
    CHECK_FALSE(rig.completed());
    CHECK_FALSE(rig.failed());
    CHECK_FALSE(rig.runtime->has_active_job());
}

TEST_CASE("cancel during decode never reaches cleanup", "[test_cancel_after_stop]") {
    // from tests/test_cancel_after_stop.py::test_cancel_during_decode_never_reaches_cleanup
    RigOptions options;
    // With cleanup on, reaching it would log that no provider is available.
    options.settings["transcript_cleanup_enabled"] = true;
    options.script.texts = {"private text"};
    RuntimeRig rig(options);
    RuntimeLog log;
    const auto wav = rig.wav("recorded.wav");
    REQUIRE(rig.runtime->claim_job());
    rig.engine->hold();

    std::thread worker([&] { rig.runtime->transcribe_audio_file(wav); });
    REQUIRE(rig.engine->wait_entered(std::chrono::seconds(5)));
    rig.runtime->job_slot().cancel(); // the engine had not yet finished
    rig.engine->resume();
    worker.join();

    CHECK_FALSE(log.contains("Transcript cleanup enabled but unavailable"));
    CHECK(rig.trace.with_prefix("error_dialog:") ==
          std::vector<std::string>{"Error: Transcription canceled"});
    CHECK(rig.trace.with_prefix("transcript:").empty());
}

TEST_CASE("cancel between emit and slot does not paste", "[test_cancel_after_stop]") {
    // from tests/test_cancel_after_stop.py::test_cancel_between_emit_and_slot_does_not_paste
    CancelRig rig;
    REQUIRE(rig.runtime->claim_job());
    rig.runtime->job_slot().cancel();

    rig.runtime->on_transcription_complete("Hello there.");

    CHECK_FALSE(rig.trace.has("stage"));
    CHECK_FALSE(rig.trace.has("copy"));
    CHECK(rig.keys.sent().empty());
    CHECK_FALSE(rig.runtime->has_active_job());
}

TEST_CASE("stop after cancel claims no job", "[test_cancel_after_stop]") {
    // from tests/test_cancel_after_stop.py::test_stop_after_cancel_claims_no_job
    CancelRig rig;
    rig.recorder.canceled = true; // canceled, stream closing

    rig.runtime->stop_recording();

    CHECK_FALSE(rig.trace.has("recorder_stop"));
    CHECK(rig.jobs.queued() == 0);
    CHECK_FALSE(rig.runtime->has_active_job());
}

TEST_CASE("abandoned job drops its early decode session", "[test_cancel_after_stop]") {
    // from tests/test_cancel_after_stop.py::test_abandoned_job_drops_its_early_decode_session
    PreviewRig p;
    REQUIRE(p.rig.runtime->start_recording());
    const auto session = p.rig.runtime->incremental_dictation().session();
    REQUIRE(session);
    p.rig.runtime->stop_recording();
    REQUIRE(p.rig.runtime->cancel()); // the job's cancel, during the post-roll

    p.rig.run_workers(); // finish_recording_job

    CHECK(p.rig.runtime->incremental_dictation().session() == nullptr);
    CHECK(session->invalid_reason() == "the recording was canceled");
    CHECK(p.rig.trace.has("discard_prefetch"));
}

TEST_CASE("preview that cannot start shows no empty overlay", "[test_cancel_after_stop]") {
    // from tests/test_cancel_after_stop.py::test_preview_that_cannot_start_shows_no_empty_overlay
    PreviewRig p;
    p.engine->hold(fv::speech::Lane::Preview);
    REQUIRE(p.recorder->start_recording());
    p.streaming->start_session("job-1");
    REQUIRE(p.shows() == 1);
    p.recorder->append(tone(130.0 * kBlock / kRate));
    p.preview_ui.advance(std::chrono::milliseconds(100));
    REQUIRE(p.engine->wait_waiting()); // the last recording's window is still decoding
    p.streaming->stop_session();

    p.streaming->start_session("job-2"); // refused to start

    CHECK(p.shows() == 1);
    p.engine->open();
}
