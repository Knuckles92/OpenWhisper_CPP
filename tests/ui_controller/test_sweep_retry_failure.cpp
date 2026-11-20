// Audit A03-35: after a remote engine's reconnects ran out, the failed recording's notice is
// the overlay's even while the main window is in front (ui_controller.py
// show_transcription_retry_failure, called from transcription.py on_history_persisted), where
// an ordinary notice is skipped because the status line already says it.
#include <catch2/catch_test_macros.hpp>

#include <string>

#include <QApplication>

#include "../ui_overlay/overlay_test_support.h"
#include "controller_support.h"
#include "ui_controller/overlay_router.h"

using fvtest::ControllerRig;

namespace {

// The main window in front, as when the user dictated into OpenWhisper_CPP itself.
void bringToFront(ControllerRig& rig) {
    rig.window().show();
    rig.window().activateWindow();
    REQUIRE(fvtest::waitUntil([&rig] { return rig.window().isActiveWindow(); }));
}

} // namespace

TEST_CASE("sweep A03-35: an exhausted reconnect shows its notice over the active main window",
          "[widget]") {
    ControllerRig rig;
    bringToFront(rig);

    rig.runtime().transcription_retry_failed.emit(
        std::string("Reconnect failed. Audio saved in History."));

    REQUIRE(fvtest::waitUntil([&rig] {
        return rig.overlay().isVisible() &&
               rig.overlay().currentState() == fv::WaveformOverlay::kStateNotice;
    }));
    fv::WaveformOverlayTestAccess access(rig.overlay());
    CHECK(access.notice() == QStringLiteral("Reconnect failed. Audio saved in History."));
    rig.overlay().hide();
}

TEST_CASE("sweep A03-35: an ordinary notice stays off the overlay while the window is in front",
          "[widget]") {
    ControllerRig rig;
    bringToFront(rig);

    rig.runtime().notice_requested.emit(
        std::string("Transcription failed. Retry the recording from History."), false);
    QApplication::processEvents();
    QApplication::processEvents();

    CHECK_FALSE(rig.overlay().isVisible());
}
