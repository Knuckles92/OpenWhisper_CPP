// Checklist lines of docs/port/map/a32-user-facing-features.md that the walkthrough runtime's
// own tests left unpinned: the Snap menu's pick is what Snap and the Screenshot shortcut repeat in
// later walkthroughs, while outside a walkthrough the shortcut always draws a region (WT-22); in
// Auto Mode the column's Draw button arms the live drawing instead of taking a screenshot
// (WT-42).
#include <catch2/catch_test_macros.hpp>

#include <chrono>

#include "runtime_rig.h"
#include "settings/keys.h"

namespace fv::walkthrough::test {

TEST_CASE("a snap mode picked from the menu is repeated by later walkthroughs", "[runtime]") {
    RuntimeRig rig;
    rig.runtime->toggle();
    rig.runtime->choose_snap_mode("screen");
    CHECK(rig.settings.get(keys::walkthrough_snap_mode) == "screen");
    REQUIRE(rig.snap.requests.size() == 1);
    CHECK(rig.snap.requests.back().take == SnapTake::Screen);
    rig.keep();

    // The walkthrough ends; outside one the Screenshot shortcut draws a region whatever the mode.
    rig.recording.end_session();
    rig.runtime->on_recording_state_changed(false);
    rig.ui.run_all();
    rig.runtime->snap();
    REQUIRE(rig.snap.requests.size() == 2);
    CHECK(rig.snap.requests.back().take == SnapTake::Region);
    rig.keep();

    // The next walkthrough's column shows the pick, and Snap repeats it.
    rig.panel.snap_mode.clear();
    rig.runtime->toggle();
    REQUIRE(rig.recording.recording_walkthrough());
    CHECK(rig.panel.snap_mode == "screen");
    rig.runtime->snap();
    REQUIRE(rig.snap.requests.size() == 3);
    CHECK(rig.snap.requests.back().take == SnapTake::Screen);
}

TEST_CASE("in auto mode the column's Draw arms the live drawing", "[runtime]") {
    RuntimeRig rig(true);
    rig.runtime->toggle();
    REQUIRE(rig.click);
    CHECK_FALSE(rig.drawing->armed());

    rig.panel.snap_requested.emit();
    rig.ui.run_all();

    CHECK(rig.drawing->armed());
    CHECK(rig.snap.requests.empty());
}

} // namespace fv::walkthrough::test
