#include <catch2/catch_test_macros.hpp>

#include <string>
#include <vector>

#include "automation_support.h"
#include "dictation/batch.h"
#include "dictation/events.h"

// Every job's events end, failures carry a category, and files publish completions
// (tests/test_automation_fix_pipeline_lifecycle.py). The preview throttle's two tests belong to
// the controller's composition.

using namespace fv::dictation;
using namespace fv::dictation::testing;
using fv::Json;

namespace {

// _batch: one item per text, the cleanup's provider and the lower-cased raw text on the ones
// with words.
BatchResult batch(const std::vector<std::string>& texts, bool combine = false,
                  const std::string& combined = {}) {
    BatchResult result;
    result.request.relation = combine ? BatchRelation::Sequential : BatchRelation::Separate;
    for (std::size_t n = 0; n < texts.size(); ++n) {
        BatchItem item;
        item.audio_path = "speech.wav" + std::to_string(n);
        item.duration_seconds = 1.0 + static_cast<double>(n);
        result.request.items.push_back(item);
        BatchItemResult done;
        done.item = item;
        done.text = texts[n];
        if (!texts[n].empty()) {
            std::string lowered = texts[n];
            for (char& c : lowered) {
                if (c >= 'A' && c <= 'Z') {
                    c = static_cast<char>(c - 'A' + 'a');
                }
            }
            done.raw_text = lowered;
            done.cleanup_provider = "openrouter";
            done.cleanup_model = "m";
        }
        result.items.push_back(done);
    }
    if (!combined.empty()) {
        result.combined_text = combined;
    }
    return result;
}

} // namespace

// ---- multi-file uploads ----

TEST_CASE("test_a_multi_file_upload_publishes_a_completion_per_saved_file",
          "[test_automation_fix_pipeline_lifecycle]") {
    AutomationRig rig;
    REQUIRE(rig.runtime->claim_job());
    rig.runtime->on_batch_complete(batch({"One.", "", "Three."}));
    rig.run_workers();

    const auto completed = rig.events.of("transcription.completed");
    REQUIRE(completed.size() == 2);
    CHECK(completed[0].data["mode"] == "file");
    CHECK(completed[0].data["text"] == "One.");
    CHECK(completed[0].data["raw_text"] == "one.");
    CHECK(completed[1].data["mode"] == "file");
    CHECK(completed[1].data["text"] == "Three.");
    CHECK(completed[1].data["raw_text"] == "three.");
    CHECK(completed[0].data["cleanup"]["provider"] == "openrouter");
    CHECK(completed[1].data["audio_s"] == 3.0);
    CHECK(rig.events.of("history.saved").size() == 2);
}

TEST_CASE("test_a_combined_upload_publishes_one_completion",
          "[test_automation_fix_pipeline_lifecycle]") {
    AutomationRig rig;
    REQUIRE(rig.runtime->claim_job());
    rig.runtime->on_batch_complete(batch({"One.", "Two."}, true, "One. Two."));
    rig.run_workers();

    const auto completed = rig.events.of("transcription.completed");
    REQUIRE(completed.size() == 1);
    CHECK(completed[0].data["text"] == "One. Two.");
    CHECK(completed[0].data["mode"] == "file");
}

TEST_CASE("test_an_empty_upload_says_there_was_no_speech",
          "[test_automation_fix_pipeline_lifecycle]") {
    AutomationRig rig;
    REQUIRE(rig.runtime->claim_job());
    rig.runtime->on_batch_complete(batch({"", ""}));

    const auto failed = rig.events.of("transcription.failed");
    REQUIRE(failed.size() == 1);
    CHECK(failed[0].data["error"] == "no_speech");
    CHECK(rig.events.of("transcription.completed").empty());
}

// ---- lifecycles that ended without a completion ----

TEST_CASE("test_an_empty_dictation_publishes_no_speech",
          "[test_automation_fix_pipeline_lifecycle]") {
    AutomationRig rig;
    rig.hears("   ");
    const auto job = make_job(JobMode::Dictation);
    rig.transcribe(job);

    const auto failed = rig.events.of("transcription.failed");
    REQUIRE(failed.size() == 1);
    CHECK(failed[0].data ==
          Json{{"job_id", job->id}, {"error", "no_speech"}, {"message", nullptr}});
    CHECK_FALSE(rig.runtime->has_active_job());
}

TEST_CASE("test_a_stop_refused_as_busy_publishes_recording_canceled",
          "[test_automation_fix_pipeline_lifecycle]") {
    AutomationRig rig;
    rig.recorder.stop_ends_capture = true;
    StartOptions options;
    REQUIRE(rig.runtime->start_recording(options));
    const auto job = rig.runtime->current_job();
    rig.activity.set_backup_in_progress(true); // the slot can't be claimed
    rig.runtime->stop_recording();

    const auto canceled = rig.events.of("recording.canceled");
    REQUIRE(canceled.size() == 1);
    CHECK(canceled[0].data == Json{{"job_id", job->id}});
    CHECK(rig.jobs.queued() == 0);
}

// ---- failures carry a category, never raw exception text ----

TEST_CASE("test_a_failure_publishes_its_category_not_the_exception",
          "[test_automation_fix_pipeline_lifecycle]") {
    AutomationRig rig;
    const auto job = make_job(JobMode::Dictation);
    REQUIRE(rig.runtime->claim_job(job));
    rig.runtime->on_transcription_error(
        R"([Errno 2] No such file: 'C:\Users\sam\AppData\Local\FioriVoce\audio.wav')");

    const auto failed = rig.events.of("transcription.failed");
    REQUIRE(failed.size() == 1);
    CHECK(failed[0].data == Json{{"job_id", job->id}, {"error", "failed"}, {"message", nullptr}});
}

TEST_CASE("test_a_cancel_publishes_canceled", "[test_automation_fix_pipeline_lifecycle]") {
    AutomationRig rig;
    REQUIRE(rig.runtime->claim_job(make_job(JobMode::Dictation)));
    REQUIRE(rig.runtime->cancel());
    rig.runtime->on_transcription_error("Transcription canceled");

    const auto failed = rig.events.of("transcription.failed");
    REQUIRE(failed.size() == 1);
    CHECK(failed[0].data["error"] == "canceled");
    CHECK(failed[0].data["message"].is_null());
}

// Python calls _refuse directly; here the Command Mode recording's completion refuses it.
TEST_CASE("test_a_refusal_publishes_its_copy", "[test_automation_fix_pipeline_lifecycle]") {
    AutomationRig rig;
    rig.rewriter.refuse_recording = "Select text first";
    const auto job = make_job(JobMode::Command);
    rig.transcribe(job);
    rig.run_workers();

    const auto failed = rig.events.of("transcription.failed");
    REQUIRE(failed.size() == 1);
    CHECK(failed[0].data ==
          Json{{"job_id", job->id}, {"error", "refused"}, {"message", "Select text first"}});
}

TEST_CASE("test_the_builder_never_passes_free_text_as_the_error",
          "[test_automation_fix_pipeline_lifecycle]") {
    CHECK(events::transcription_failed(nullptr, "Traceback: C:\\secret\\path") ==
          Json{{"job_id", ""}, {"error", "failed"}, {"message", nullptr}});
    CHECK(events::transcription_failed(nullptr, "refused", "Select text first")["message"] ==
          "Select text first");
    CHECK(events::transcription_failed(nullptr, "failed", "C:\\x")["message"].is_null());
}
