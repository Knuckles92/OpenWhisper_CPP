// What the walkthrough runtime says about screenshots and Auto Mode
// (services/runtime/walkthrough.py's _tell, show_overlay_caption and report_status texts).
#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <memory>
#include <thread>

#include "runtime_rig.h"

using namespace fv::walkthrough;
using namespace fv::walkthrough::test;

// walkthrough.py:693: a screenshot taken outside a walkthrough that the clipboard refused.
TEST_CASE("a screenshot the clipboard refuses says it couldn't be copied", "[runtime]") {
    RuntimeRig rig;
    rig.copy_works = false;

    rig.runtime->snap();
    rig.keep();

    CHECK(rig.notice == "Couldn't copy the screenshot");
}

// walkthrough.py:654: the overlay's caption for each kept screenshot.
TEST_CASE("each kept screenshot is captioned with its number", "[runtime]") {
    RuntimeRig rig;
    rig.runtime->toggle();

    rig.runtime->snap();
    rig.keep();
    CHECK(rig.overlay.caption == "Screenshot 1 added");
    rig.runtime->snap();
    rig.keep();
    CHECK(rig.overlay.caption == "Screenshot 2 added");
}

// walkthrough.py:158: the session's writer thread could not write the PNG; the status line says
// so on the UI thread and the walkthrough goes on.
TEST_CASE("a screenshot the writer could not save says the walkthrough keeps recording",
          "[runtime]") {
    RuntimeRig rig;
    rig.runtime->toggle();
    rig.runtime->snap();

    rig.snap.kept_(
        std::make_shared<FakeShotImage>(2, 2, 0x123456, FakeShotImage::Save::FailsHalfWritten));
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (rig.recording.status.empty() && std::chrono::steady_clock::now() < deadline) {
        rig.ui.run_all();
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }

    CHECK(rig.recording.status ==
          "Screenshot 1 couldn't be saved — the walkthrough keeps recording");
    CHECK(rig.recording.recording_walkthrough());
    CHECK(rig.recording.stops == 0);
}

// walkthrough.py:666: a capture that failed without a reason of its own.
TEST_CASE("a screenshot that failed without a reason says it couldn't be taken", "[runtime]") {
    RuntimeRig rig;
    rig.runtime->snap();
    REQUIRE(rig.snap.failed_);

    rig.snap.failed_("");
    rig.ui.run_all();

    CHECK(rig.notice == "Couldn't take a screenshot");
}

// walkthrough.py:267: the click hook did not start; Snap still works.
TEST_CASE("auto mode without a click hook says snap still works", "[runtime]") {
    RuntimeRig rig(true);
    rig.clicks_work = false;

    rig.runtime->toggle();

    CHECK(rig.notice == "Auto Mode couldn't watch for clicks. You can still use Snap.");
    CHECK(rig.recording.recording_walkthrough());
    rig.runtime->snap();
    CHECK(rig.snap.requests.size() == 1);
}
