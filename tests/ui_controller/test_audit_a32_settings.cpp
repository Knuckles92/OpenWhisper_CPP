// a32 checklist SR-01 (docs/port/map/a32-user-facing-features.md, Settings > Recording): when the
// microphone is lost mid-recording the overlay says "Switched to <mic>"
// (ui_controller.py on_recording_device_switched). test_wiring.cpp pins the status line only, with
// the overlay hidden, where a caption is ignored; this pins the caption on a showing overlay.
#include <catch2/catch_test_macros.hpp>

#include <QString>

#include "../ui_overlay/overlay_test_support.h"
#include "controller_support.h"
#include "ui_overlay/waveform_overlay.h"

using fvtest::ControllerRig;

TEST_CASE("a32 SR-01: a microphone switch names the new microphone on the showing overlay",
          "[widget]") {
    ControllerRig rig;
    fv::WaveformOverlay& overlay = rig.overlay();
    fv::WaveformOverlayTestAccess access(overlay);
    overlay.showAtCursor(fv::WaveformOverlay::kStateRecording);
    REQUIRE(overlay.isVisible());

    rig.ui().onRecordingDeviceSwitched(QStringLiteral("USB headset"), QStringLiteral("Laptop mic"));
    CHECK(access.caption() == QStringLiteral("Switched to Laptop mic"));
    CHECK(rig.window().status() ==
          QStringLiteral("Microphone disconnected — continuing on Laptop mic"));

    // The last device in the order is the system default, which has no name of its own.
    rig.ui().onRecordingDeviceSwitched(QStringLiteral("Laptop mic"), QString());
    CHECK(access.caption() == QStringLiteral("Switched to the system default microphone"));
    overlay.hide();
}
