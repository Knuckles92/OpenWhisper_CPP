#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <algorithm>
#include <chrono>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "automation_support.h"
#include "dictation/batch.h"
#include "dictation/cleaner_port.h"
#include "dictation/messages.h"

// What a Cancel does to the job in progress, and says (tests/test_automation_r3_cancel_window.py,
// tests/test_automation_r4_cancel_runtime.py, tests/test_automation_r4_cancel_uploads.py). The
// controller's half of a Cancel (running actions, "Nothing to cancel") and the live API's
// refusal are later packages'; here they are the runtime calls they make: cancel(), and the
// API's `cancelable() && cancel()` (app_control._cancel).

using namespace fv::dictation;
using namespace fv::dictation::testing;
using fv::Json;

namespace {

const std::vector<std::string> kStopped = {"recording.started", "recording.stopped"};
const std::vector<std::string> kCanceled = {"recording.canceled", "transcription.failed(canceled)"};

std::vector<std::string> concat(std::vector<std::string> a, const std::vector<std::string>& b) {
    a.insert(a.end(), b.begin(), b.end());
    return a;
}

// What the runtime has shown so far: statuses and overlay states.
std::pair<std::vector<std::string>, std::vector<std::string>> shown(const AutomationRig& rig) {
    return {rig.statuses(), rig.overlays()};
}

// app_control._cancel's question: false is the API's "Nothing is recording or transcribing."
bool api_cancel(AutomationRig& rig) {
    return rig.runtime->cancelable() && rig.runtime->cancel();
}

std::size_t count(const std::vector<std::string>& kinds, const std::string& kind) {
    return static_cast<std::size_t>(std::ranges::count(kinds, kind));
}

// QUICK: an agent asked for a quick answer, shown as a notice.
Json quick_action() {
    return action_json("q1", "Quick answer", "agent",
                       Json{{"agent", "claude_code"}, {"mode", "ask"}, {"output", "notice"}});
}

// A Quick answer dictation, delivered to its action, its History save not run yet.
void quick_answer_delivered(AutomationRig& rig) {
    rig.with_actions({quick_action()});
    rig.live_recording();
    REQUIRE(rig.runtime->claim_job(make_job(JobMode::Action, "q1", "hotkey")));
    rig.runtime->transcribe_audio_file(rig.recorder.output_file());
    REQUIRE(rig.port.deliveries().size() == 1);
    REQUIRE(rig.runtime->has_active_job());
    REQUIRE(rig.persistence.queued() > 0);
}

} // namespace

// ---- tests/test_automation_r3_cancel_window.py ----

TEST_CASE("test_the_runtime_has_nothing_to_cancel_once_the_result_is_delivered",
          "[test_automation_r3_cancel_window]") {
    AutomationRig rig;
    quick_answer_delivered(rig);

    CHECK_FALSE(rig.runtime->cancelable());
    CHECK_FALSE(rig.runtime->cancel());
    CHECK_FALSE(rig.runtime->job_slot().cancel_requested());
    // A new recording still waits for the save.
    CHECK(rig.runtime->has_active_job());
}

TEST_CASE("test_an_api_cancel_while_a_delivered_job_is_saved_is_refused",
          "[test_automation_r3_cancel_window]") {
    AutomationRig rig;
    quick_answer_delivered(rig);
    const auto before = shown(rig);
    const auto notices = rig.notices();

    CHECK_FALSE(api_cancel(rig));

    CHECK(count(rig.kinds(), "recording.canceled") == 0);
    // The user is told nothing: the API's answer is the report.
    CHECK(shown(rig) == before);
    CHECK(rig.notices() == notices);
    // The user's Quick answer goes on.
    CHECK_FALSE(rig.runtime->job_slot().cancel_requested());
}

TEST_CASE("test_a_cancel_before_the_result_is_delivered_still_cancels_the_job",
          "[test_automation_r3_cancel_window]") {
    AutomationRig rig;
    rig.with_actions({quick_action()});
    rig.live_recording();
    REQUIRE(rig.runtime->claim_job(make_job(JobMode::Action, "q1", "hotkey")));
    // The worker finished, but its result hasn't reached the UI thread yet.
    rig.ui.hold();
    rig.runtime->transcribe_audio_file(rig.recorder.output_file());
    auto on_its_way = rig.ui.take();
    REQUIRE(on_its_way.size() == 1);

    CHECK(rig.runtime->cancelable());
    CHECK(rig.runtime->cancel());
    on_its_way.front()();
    rig.run_workers();

    CHECK(rig.port.deliveries().empty());
    const auto failed = rig.events.of("transcription.failed");
    REQUIRE_FALSE(failed.empty());
    CHECK(failed.back().data["error"] == "canceled");
    CHECK(count(rig.kinds(), "recording.canceled") == 1);
}

TEST_CASE("test_a_failed_jobs_audio_being_kept_is_not_canceled",
          "[test_automation_r3_cancel_window]") {
    AutomationRig rig;
    rig.with_actions({quick_action()});
    rig.live_recording();
    REQUIRE(rig.runtime->claim_job(make_job(JobMode::Action, "q1", "hotkey")));
    rig.runtime->on_transcription_error("engine exploded");
    REQUIRE(rig.runtime->has_active_job()); // keeping the audio
    REQUIRE(rig.persistence.queued() > 0);

    CHECK_FALSE(rig.runtime->cancel());
    CHECK(count(rig.kinds(), "recording.canceled") == 0);
    const auto failed = rig.events.of("transcription.failed");
    REQUIRE(failed.size() == 1);
    CHECK(failed[0].data["error"] == "failed");
}

// ---- tests/test_automation_r4_cancel_runtime.py ----

TEST_CASE("test_a_second_cancel_while_recording_is_quiet", "[test_automation_r4_cancel_runtime]") {
    AutomationRig rig;
    REQUIRE(rig.runtime->start_recording());
    CHECK(rig.runtime->cancel());
    const auto before = shown(rig);

    // The capture's stream is still closing.
    CHECK_FALSE(rig.runtime->cancelable());
    CHECK(rig.runtime->cancel());
    CHECK(shown(rig) == before);
    rig.recorder.closed();

    // A recording canceled before it stopped ends with recording.canceled.
    CHECK(rig.kinds() == std::vector<std::string>{"recording.started", "recording.canceled"});
    CHECK_FALSE(rig.runtime->cancel());
}

TEST_CASE("test_a_second_cancel_after_the_stop_is_quiet", "[test_automation_r4_cancel_runtime]") {
    const bool post_roll_over = GENERATE(false, true);
    CAPTURE(post_roll_over);
    AutomationRig rig;
    REQUIRE(rig.runtime->start_recording());
    rig.runtime->stop_recording(); // the slot is claimed; post-roll goes on
    if (post_roll_over) {
        rig.recorder.recording = false; // the WAV is next
    }
    CHECK(rig.runtime->cancel());
    const auto before = shown(rig);

    CHECK_FALSE(rig.runtime->cancelable());
    CHECK(rig.runtime->cancel());
    CHECK(shown(rig) == before);
    rig.run_workers();

    CHECK(rig.kinds() == concat(kStopped, kCanceled));
    CHECK_FALSE(rig.runtime->has_active_job());
}

TEST_CASE("test_a_second_cancel_while_transcribing_is_quiet",
          "[test_automation_r4_cancel_runtime]") {
    AutomationRig rig;
    REQUIRE(rig.runtime->start_recording());
    rig.runtime->stop_recording();
    rig.engine->hold();
    std::thread worker([&rig] { rig.jobs.run_all(); });
    bool entered = false;
    try {
        entered = rig.engine->wait_entered(std::chrono::seconds(30));
        REQUIRE(entered);
        REQUIRE(rig.runtime->is_transcribing());
        CHECK(rig.runtime->cancel());
        const auto before = shown(rig);

        CHECK_FALSE(rig.runtime->cancelable());
        CHECK(rig.runtime->cancel());
        CHECK(shown(rig) == before);
        // The engine was told to stop: the decode's cancel is set.
        CHECK(rig.runtime->job_slot().cancel_token().cancelled());
    } catch (...) {
        rig.engine->resume();
        worker.join();
        throw;
    }
    rig.engine->resume();
    worker.join();
    rig.run_workers();

    CHECK(rig.kinds() == concat(kStopped, kCanceled));
    CHECK_FALSE(rig.runtime->has_active_job());
}

TEST_CASE("test_the_api_refuses_a_second_cancel", "[test_automation_r4_cancel_runtime]") {
    AutomationRig rig;
    REQUIRE(rig.runtime->start_recording());
    rig.runtime->stop_recording();

    CHECK(api_cancel(rig));
    CHECK_FALSE(api_cancel(rig)); // refused

    rig.run_workers();
    CHECK(rig.kinds() == concat(kStopped, kCanceled));
}

TEST_CASE("test_a_rewrite_canceled_while_it_reads_the_selection_ends_canceled",
          "[test_automation_r4_cancel_runtime]") {
    AutomationRig rig;
    const auto job = make_job(JobMode::Transform, "", "hotkey");
    REQUIRE(rig.runtime->begin_rewrite_job(job, "Transform · Shout"));

    CHECK(rig.runtime->cancel());
    rig.runtime->abandon_rewrite_job("Select text to transform");

    CHECK(rig.kinds() == kCanceled);
    const auto failed = rig.events.of("transcription.failed");
    REQUIRE(failed.size() == 1);
    CHECK(failed[0].data["job_id"] == job->id);
    CHECK_FALSE(rig.runtime->has_active_job());
}

TEST_CASE("test_a_cancel_after_an_upload_was_delivered_says_nothing",
          "[test_automation_r4_cancel_runtime]") {
    AutomationRig rig;
    REQUIRE(rig.runtime->upload_audio_file(rig.wav()));
    rig.jobs.run_all(); // delivered; its history is being saved
    REQUIRE(rig.persistence.queued() > 0);
    const auto before = shown(rig);

    CHECK_FALSE(rig.runtime->cancel());

    CHECK(shown(rig) == before);
    CHECK(count(rig.kinds(), "recording.canceled") == 0);
}

// The controller then says "Nothing to cancel" (M5.22); the runtime's half is that its Cancel
// canceled nothing and showed nothing.
TEST_CASE("test_cancel_while_a_pasted_dictation_is_saved_says_there_was_nothing_to_cancel",
          "[test_automation_r4_cancel_runtime]") {
    AutomationRig rig;
    rig.transcribe(make_job(JobMode::Dictation, "", "hotkey"));
    REQUIRE_FALSE(rig.stages().empty());
    REQUIRE(rig.persistence.queued() > 0); // pasted; its history is being saved
    const auto before = shown(rig);

    CHECK_FALSE(rig.runtime->cancel());

    CHECK(shown(rig) == before); // no "Canceled", no cancel animation
    rig.run_workers();
    CHECK(rig.entries().size() == 1);
}

// ---- tests/test_automation_r4_cancel_uploads.py ----

namespace {

BatchResult batch(const std::vector<std::string>& texts, bool combine = false,
                  const std::string& combined = {},
                  std::optional<std::string> cleanup_error = std::nullopt) {
    BatchResult result;
    result.request.relation = combine ? BatchRelation::Sequential : BatchRelation::Separate;
    for (std::size_t n = 0; n < texts.size(); ++n) {
        BatchItem item;
        item.audio_path = "speech.wav" + std::to_string(n);
        item.duration_seconds = 1.0;
        result.request.items.push_back(item);
        BatchItemResult done;
        done.item = item;
        done.text = texts[n];
        result.items.push_back(done);
    }
    if (!combined.empty()) {
        result.combined_text = combined;
        result.combined_raw_text = combined;
    }
    result.cleanup_error = std::move(cleanup_error);
    return result;
}

// Between files, or done: the slot is held by the batch.
void batch_under_way(AutomationRig& rig) {
    REQUIRE(rig.runtime->claim_job());
}

} // namespace

TEST_CASE("test_an_upload_canceled_after_its_worker_finished_is_canceled",
          "[test_automation_r4_cancel_uploads]") {
    AutomationRig rig;
    REQUIRE(rig.runtime->upload_audio_file(rig.wav()));
    rig.ui.hold();
    rig.jobs.run_all(); // the worker's result is on its way to the UI thread
    auto on_its_way = rig.ui.take();
    REQUIRE(on_its_way.size() == 1);

    CHECK(rig.runtime->cancel());
    on_its_way.front()();
    rig.run_workers();

    CHECK(rig.kinds() == kCanceled);
    CHECK(rig.entries().empty());
    CHECK(rig.last_status() == "Error: Transcription canceled");
    CHECK_FALSE(rig.runtime->has_active_job());
}

TEST_CASE("test_a_batch_canceled_after_its_files_finished_says_canceled",
          "[test_automation_r4_cancel_uploads]") {
    AutomationRig rig;
    batch_under_way(rig);

    CHECK(rig.runtime->cancel());
    rig.runtime->on_batch_complete(batch({"One.", "Two."}));
    rig.run_workers();

    // Files already done are kept, as when the Cancel lands between files.
    CHECK(rig.last_status() == "Canceled — kept 2 of 2 files");
    std::vector<std::string> texts;
    for (const auto& entry : rig.entries()) {
        texts.push_back(entry.text);
    }
    std::ranges::sort(texts);
    CHECK(texts == std::vector<std::string>{"One.", "Two."});
    CHECK(count(rig.kinds(), "history.saved") == 2);
}

TEST_CASE("test_a_combined_batch_canceled_after_it_finished_says_canceled",
          "[test_automation_r4_cancel_uploads]") {
    AutomationRig rig;
    batch_under_way(rig);

    CHECK(rig.runtime->cancel());
    rig.runtime->on_batch_complete(batch({"One.", "Two."}, true, "One. Two."));

    CHECK(rig.last_status() == "Canceled — kept the transcript");
}

TEST_CASE("test_a_combined_batch_canceled_during_its_cleanup_keeps_the_raw_transcript",
          "[test_automation_r4_cancel_uploads]") {
    AutomationRig rig;
    batch_under_way(rig);

    CHECK(rig.runtime->cancel());
    rig.runtime->on_batch_complete(
        batch({"one", "two"}, true, "one two", std::string(CLEANUP_CANCELED_REASON)));

    CHECK(rig.last_status() == "Canceled — kept the transcript without AI cleanup");
}

TEST_CASE("test_a_batch_canceled_with_nothing_to_keep_ends_canceled",
          "[test_automation_r4_cancel_uploads]") {
    AutomationRig rig;
    batch_under_way(rig);

    CHECK(rig.runtime->cancel());
    rig.runtime->on_batch_complete(batch({"", ""}));

    CHECK(rig.kinds() == kCanceled);
    CHECK(rig.last_status() == "Error: Transcription canceled");
    const auto statuses = rig.statuses();
    CHECK(std::ranges::find(statuses, std::string(EMPTY_ASR_MESSAGE)) == statuses.end());
}
