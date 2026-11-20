// The walkthrough runtime with its column and Auto Mode's live drawing
// (tests/test_walkthrough_shortcuts.py): what an automatic capture crops, marks and tells the
// column, and the column's buttons across walkthroughs.
#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <memory>

#include "runtime_rig.h"

using namespace fv::walkthrough;
using namespace fv::walkthrough::test;

// from
// tests/test_walkthrough_shortcuts.py::test_auto_region_crops_clicks_and_drawings_with_marks_at_device_resolution
// The pixels of the crop at each device pixel ratio are the snap port's render
// (tests/ui_snap/test_live_ports.cpp, "the snap port renders marks onto a crop at device
// resolution"); here the runtime asks for that crop and those marks, on the region's screen,
// and gives the column the region's place on the desktop for both kinds of capture.
TEST_CASE("auto region crops clicks and drawings with marks at device resolution", "[runtime]") {
    for (const double dpr : {1.0, 1.5, 2.0}) {
        CAPTURE(dpr);
        RuntimeRig rig(true);
        rig.runtime->toggle();
        const ScreenRef screen{"left", 1};
        const Rect rect{6, 4, 12, 10};
        // The screen's geometry starts at (-1920, -500).
        const Rect global{-1914, -496, 12, 10};
        rig.drawing->region = CaptureRegion{screen, rect, global};
        auto marks = std::make_shared<int>(1);
        rig.drawing->pinned = marks;

        for (int capture = 0; capture < 2; ++capture) {
            if (capture == 0) {
                rig.click(-120, 25, 0);
                rig.ui.run_all();
                rig.ui.advance(std::chrono::milliseconds(350));
            } else {
                rig.drawing->drawn.emit(screen, marks);
                rig.ui.run_all();
            }
            REQUIRE(rig.snap.requests.size() == static_cast<std::size_t>(capture + 1));
            CHECK(rig.snap.requests.back().screen == screen);
            rig.snap.kept_(std::make_shared<FakeShotImage>(static_cast<int>(24 * dpr),
                                                           static_cast<int>(16 * dpr), 0x194754,
                                                           FakeShotImage::Save::Works, dpr));
            rig.ui.run_all();
            CHECK(rig.snap.render_crop == rect);
            CHECK(rig.snap.render_marks == marks);
            REQUIRE(rig.panel.sources.size() == static_cast<std::size_t>(capture + 1));
            CHECK(rig.panel.sources.back() == global);
        }
        CHECK(rig.recording.recording_walkthrough()->shot_count() == 2);
    }
}

// from tests/test_walkthrough_shortcuts.py::test_the_column_is_answered_once_across_walkthroughs
TEST_CASE("the column is answered once across walkthroughs", "[runtime]") {
    RuntimeRig rig;
    rig.runtime->toggle();
    rig.recording.end_session();
    rig.runtime->on_recording_state_changed(false);
    rig.ui.run_all();
    rig.runtime->toggle();

    rig.panel.snap_requested.emit();
    rig.ui.run_all();

    CHECK(rig.snap.requests.size() == 1);
}
