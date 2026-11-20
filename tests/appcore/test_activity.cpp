// ActivityState: the flags the gates read, and their change signal.
#include <catch2/catch_test_macros.hpp>

#include <thread>
#include <vector>

#include "appcore/activity.h"
#include "base/testing/manual_executor.h"

using fv::ActivitySnapshot;
using fv::ActivityState;

TEST_CASE("a set that changes a flag emits the new snapshot; the same value emits nothing",
          "[activity]") {
    fv::ManualExecutor ui;
    ActivityState state;
    std::vector<ActivitySnapshot> seen;
    auto link = state.changed.connect(ui, [&](const ActivitySnapshot& s) { seen.push_back(s); });

    state.set_recording(true);
    state.set_recording(true);
    state.set_meeting_active(true);
    state.set_recording(false);
    ui.run_all();

    REQUIRE(seen.size() == 3);
    CHECK(seen[0].recording);
    CHECK_FALSE(seen[0].meeting_active);
    CHECK(seen[1].recording);
    CHECK(seen[1].meeting_active);
    CHECK_FALSE(seen[2].recording);
    CHECK(state.snapshot() == seen[2]);
}

TEST_CASE("every flag has its own setter", "[activity]") {
    ActivityState state;
    state.set_startup_ready(true);
    state.set_shutting_down(true);
    state.set_recovery_running(true);
    state.set_backup_in_progress(true);
    state.set_meeting_setup_open(true);
    state.set_meeting_active(true);
    state.set_meeting_finishing(true);
    state.set_recording(true);
    state.set_transcribing(true);
    state.set_walkthrough_job(true);
    state.set_write_up_running(true);
    state.set_library_change(true);
    state.set_rule_dictation_busy(true);
    state.set_host_update_active(true);
    state.set_component_installing(true);
    const ActivitySnapshot all{true, true, true, true, true, true, true, true,
                               true, true, true, true, true, true, true};
    CHECK(state.snapshot() == all);
}

TEST_CASE("flags may be set from any thread, and listeners end on the current state",
          "[activity]") {
    // Two runtimes changing their flags at once: the snapshots must not reach listeners out of
    // order, or the reload machine would keep a recording that has ended.
    for (int round = 0; round < 20; ++round) {
        fv::ManualExecutor ui;
        ActivityState state;
        std::vector<ActivitySnapshot> seen;
        auto link =
            state.changed.connect(ui, [&](const ActivitySnapshot& s) { seen.push_back(s); });
        std::thread a([&] {
            for (int i = 0; i < 201; ++i) {
                state.set_recording(i % 2 == 0);
            }
        });
        std::thread b([&] {
            for (int i = 0; i < 200; ++i) {
                state.set_transcribing(i % 2 == 0);
            }
        });
        a.join();
        b.join();
        ui.run_all();
        REQUIRE_FALSE(seen.empty());
        CHECK(seen.size() <= 401);
        CHECK(seen.back() == state.snapshot());
        CHECK(state.snapshot().recording);
        CHECK_FALSE(state.snapshot().transcribing);
    }
}

TEST_CASE("a set made by a listener running inline is emitted after the one it saw", "[activity]") {
    fv::ManualExecutor ui;
    ActivityState state;
    std::vector<ActivitySnapshot> seen;
    auto link = state.changed.connect(ui, [&](const ActivitySnapshot& s) {
        seen.push_back(s);
        if (s.recording && !s.transcribing) {
            state.set_transcribing(true);
        }
    });
    ui.post([&] { state.set_recording(true); });
    ui.run_all();
    REQUIRE(seen.size() == 2);
    CHECK(seen[0].recording);
    CHECK_FALSE(seen[0].transcribing);
    CHECK(seen[1].transcribing);
    CHECK(seen.back() == state.snapshot());
}
