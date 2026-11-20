#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <memory>
#include <thread>
#include <vector>

#include "dictation/job_slot.h"

// The slot fields and transitions of transcription.py _claim_job, _settle, _cancel and
// _finish_job. Their Python tests drive the whole TranscriptionRuntime (M1.14); these pin the
// slot on its own.

using namespace fv::dictation;

namespace {

std::shared_ptr<const DictationJob> make_job(JobMode mode = JobMode::Dictation) {
    DictationJob job;
    job.mode = mode;
    return std::make_shared<const DictationJob>(std::move(job));
}

} // namespace

TEST_CASE("one job at a time, and every gate refuses a claim", "[dictation][job_slot]") {
    JobSlot slot;
    CHECK_FALSE(slot.has_active_job());
    CHECK_FALSE(slot.claim(nullptr, {.shutting_down = true}));
    CHECK_FALSE(slot.claim(nullptr, {.backup_in_progress = true}));
    CHECK_FALSE(slot.claim(nullptr, {.host_update_active = true}));
    CHECK_FALSE(slot.claim(nullptr, {.recorder_busy = true}));
    CHECK_FALSE(slot.has_active_job());

    const auto job = make_job();
    CHECK(slot.claim(job));
    CHECK(slot.has_active_job());
    CHECK(slot.active_job() == job);
    CHECK_FALSE(slot.claim(make_job()));
    CHECK(slot.active_job() == job);

    slot.finish();
    CHECK_FALSE(slot.has_active_job());
    CHECK(slot.active_job() == nullptr);
    CHECK(slot.claim(nullptr)); // an upload has no job
}

TEST_CASE("current_job is the recording's until a claim, then the slot's",
          "[dictation][job_slot]") {
    JobSlot slot;
    const auto recording = make_job();
    slot.set_recording_job(recording);
    CHECK(slot.current_job() == recording);
    CHECK(slot.recording_job() == recording);

    const auto routed = make_job(JobMode::Action);
    REQUIRE(slot.claim(routed));
    CHECK(slot.current_job() == routed);
    REQUIRE(slot.claim(nullptr) == false);

    slot.finish();
    CHECK(slot.current_job() == nullptr);
    CHECK(slot.recording_job() == nullptr);

    REQUIRE(slot.claim(nullptr));
    CHECK(slot.current_job() == nullptr); // an upload: the slot's job is none
}

TEST_CASE("a cancel before settle is honored, one after reaches nothing", "[dictation][job_slot]") {
    JobSlot slot;
    REQUIRE(slot.claim(make_job()));
    CHECK(slot.working());

    const CancelRequest first = slot.request_cancel();
    CHECK(first.working);
    CHECK_FALSE(first.repeat);
    CHECK(slot.cancel_requested());
    CHECK(slot.cancel_token().cancelled());
    CHECK_FALSE(slot.working());
    const CancelRequest second = slot.request_cancel();
    CHECK(second.repeat);
    CHECK(slot.settle());

    slot.finish();
    REQUIRE(slot.claim(make_job()));
    // A new job gets a fresh cancel: the last one's stays set for whoever still looks at it.
    CHECK_FALSE(slot.cancel_requested());
    CHECK_FALSE(slot.settle());
    CHECK(slot.saving());
    const CancelRequest late = slot.request_cancel();
    CHECK_FALSE(late.working);
    CHECK_FALSE(slot.cancel_requested());
}

TEST_CASE("a claim gives the job a new cancel source", "[dictation][job_slot]") {
    JobSlot slot;
    REQUIRE(slot.claim(make_job()));
    const fv::CancelToken first = slot.cancel_token();
    slot.cancel();
    CHECK(first.cancelled());
    slot.finish();
    REQUIRE(slot.claim(make_job()));
    CHECK(first.cancelled());
    CHECK_FALSE(slot.cancel_token().cancelled());
}

TEST_CASE("no cancel without a working job", "[dictation][job_slot]") {
    JobSlot slot;
    const auto recording = make_job();
    slot.set_recording_job(recording);
    const CancelRequest request = slot.request_cancel();
    CHECK_FALSE(request.working);
    CHECK_FALSE(request.repeat);
    CHECK(request.job == recording);
    CHECK_FALSE(slot.cancel_requested());
}

TEST_CASE("the preview is published only for its job and stops at end_preview",
          "[dictation][job_slot]") {
    JobSlot slot;
    const auto job = make_job();
    const auto other = make_job();
    int published = 0;
    CHECK_FALSE(slot.with_preview(job.get(), [&] { ++published; }));
    slot.start_preview(job);
    CHECK(slot.previewing() == job);
    CHECK(slot.with_preview(job.get(), [&] { ++published; }));
    CHECK_FALSE(slot.with_preview(other.get(), [&] { ++published; }));
    CHECK_FALSE(slot.with_preview(nullptr, [&] { ++published; }));
    slot.end_preview();
    CHECK_FALSE(slot.with_preview(job.get(), [&] { ++published; }));
    CHECK(published == 1);

    slot.set_recording_job(job);
    slot.start_preview(job);
    slot.forget_recording();
    CHECK(slot.previewing() == nullptr);
    CHECK(slot.recording_job() == nullptr);
}

TEST_CASE("claims race to exactly one winner", "[dictation][job_slot]") {
    JobSlot slot;
    std::atomic<int> won{0};
    std::vector<std::thread> threads;
    for (int i = 0; i < 8; ++i) {
        threads.emplace_back([&] {
            if (slot.claim(make_job())) {
                ++won;
            }
        });
    }
    for (auto& thread : threads) {
        thread.join();
    }
    CHECK(won == 1);
}
