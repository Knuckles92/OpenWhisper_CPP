// The controller's overlay routing (ui_controller.py set_overlay_state 875-914,
// _show_listening_overlay 923-932, _start_cancel_animation and _on_cancel_animation_finished
// 1450-1471; A03-31, A03-32, A03-33, A03-34): which overlay state each logical state shows.
#include <catch2/catch_test_macros.hpp>

#include <QTest>

#include "controller_support.h"
#include "ui_controller/overlay_router.h"
#include "ui_overlay/waveform_overlay.h"

using fv::WaveformOverlay;
using fv::appcore::OverlayState;
using fvtest::ControllerRig;

namespace {
// CANCELLATION_ANIMATION_DURATION_MS + CANCELLATION_GRACE_MS, and a little more.
constexpr int kAnimationOverMs = fv::kCancellationAnimationMs + fv::kCancellationGraceMs + 300;
} // namespace

TEST_CASE("a cancel plays its animation then hides the overlay", "[widget]") {
    ControllerRig rig;
    fv::OverlayRouter& router = rig.ui().overlayRouter();
    router.setOverlayState(OverlayState::Recording);
    REQUIRE(rig.overlay().isVisible());

    router.setOverlayState(OverlayState::Canceling);
    CHECK(rig.overlay().currentState() == WaveformOverlay::kStateCanceling);
    CHECK(rig.overlay().isVisible());
    QTest::qWait(fv::kCancellationAnimationMs);
    CHECK(rig.overlay().isVisible()); // not before 1400 ms
    QTest::qWait(kAnimationOverMs - fv::kCancellationAnimationMs);
    CHECK_FALSE(rig.overlay().isVisible());
}

TEST_CASE("a recording started during the cancel animation keeps its overlay", "[widget]") {
    ControllerRig rig;
    fv::OverlayRouter& router = rig.ui().overlayRouter();
    router.setOverlayState(OverlayState::Recording);
    router.setOverlayState(OverlayState::Canceling);
    router.setOverlayState(OverlayState::Recording);
    QTest::qWait(kAnimationOverMs);
    CHECK(rig.overlay().isVisible());
    CHECK(rig.overlay().currentState() == WaveformOverlay::kStateRecording);
}

TEST_CASE("listening shows the live preview while it runs and a work state replaces it",
          "[widget]") {
    ControllerRig rig;
    fv::OverlayRouter& router = rig.ui().overlayRouter();

    router.setOverlayState(OverlayState::CommandListening);
    CHECK(rig.overlay().currentState() == WaveformOverlay::kStateCommandListening);
    router.setOverlayState(OverlayState::None);
    CHECK_FALSE(rig.overlay().isVisible());

    router.showStreamingOverlay();
    CHECK(rig.overlay().currentState() == WaveformOverlay::kStateStreaming);
    router.setOverlayState(OverlayState::Recording);
    CHECK(rig.overlay().currentState() == WaveformOverlay::kStateStreaming);

    // The work state ends the preview's flow: the next recording listens without it.
    router.setOverlayState(OverlayState::Processing);
    CHECK(rig.overlay().currentState() == WaveformOverlay::kStateProcessing);
    router.setOverlayState(OverlayState::Recording);
    CHECK(rig.overlay().currentState() == WaveformOverlay::kStateRecording);

    // Hiding the preview leaves the overlay up.
    router.showStreamingOverlay();
    router.hideStreamingOverlay();
    CHECK(rig.overlay().isVisible());
    router.setOverlayState(OverlayState::Recording);
    CHECK(rig.overlay().currentState() == WaveformOverlay::kStateRecording);

    router.setOverlayState(OverlayState::SttDisabled);
    CHECK(rig.overlay().currentState() == WaveformOverlay::kStateSttDisable);
}
