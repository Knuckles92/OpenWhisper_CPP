// Dragging the recording overlay aside (tests/test_drag_to_move.py, the overlay's tests).
#include <catch2/catch_test_macros.hpp>

#include <QApplication>
#include <QMouseEvent>
#include <QPoint>
#include <QScreen>
#include <QSignalSpy>

#include "overlay_test_support.h"

using fv::WaveformOverlay;
using fvtest::Overlay;

namespace {

// Press at start, carry the pointer by, release; positions follow the window.
void drag(QWidget& widget, const QPoint& start, const QPoint& by) {
    fvtest::sendMouse(widget, QEvent::MouseButtonPress, start);
    // The grab point stays under the pointer, so the event's local position is where it was
    // pressed, at a global spot `by` further on.
    const QPoint globalEnd = widget.mapToGlobal(start) + by;
    QMouseEvent move(QEvent::MouseMove, QPointF(start), QPointF(globalEnd), Qt::NoButton,
                     Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(&widget, &move);
    fvtest::sendMouse(widget, QEvent::MouseButtonRelease, start);
}

// A drag vector that stays on screen from the widget's spot: toward the middle.
QPoint room(const QWidget& widget) {
    const QRect area = widget.screen()->availableGeometry();
    return QPoint(widget.x() > area.center().x() ? -40 : 40,
                  widget.y() > area.center().y() ? -30 : 30);
}

QPoint grabPoint(const QWidget& widget) {
    return QPoint(20, widget.height() - 6);
}

} // namespace

// from tests/test_drag_to_move.py::test_the_overlay_opens_by_the_pointer_and_can_be_dragged_aside
TEST_CASE("the overlay opens by the pointer and can be dragged aside",
          "[widget][test_drag_to_move]") {
    Overlay overlay;
    overlay->showAtCursor(WaveformOverlay::kStateRecording);
    const QPoint opened = overlay->pos();
    const QPoint by = room(*overlay);

    drag(*overlay, grabPoint(*overlay), by);

    CHECK(overlay->pos() == opened + by);
}

// from tests/test_drag_to_move.py::test_a_dragged_overlay_stays_put_through_the_recording
TEST_CASE("a dragged overlay stays put through the recording", "[widget][test_drag_to_move]") {
    Overlay overlay;
    overlay->showAtCursor(WaveformOverlay::kStateStreaming);
    drag(*overlay, grabPoint(*overlay), room(*overlay));
    const QPoint placed = overlay->pos();

    // Re-showing while visible (a streaming restart, Copied) keeps the user's spot.
    overlay->showAtCursor(WaveformOverlay::kStateStreaming);
    overlay->setState(WaveformOverlay::kStateProcessing);

    CHECK(overlay->pos() == placed);
}

// from tests/test_drag_to_move.py::test_the_next_recording_opens_by_the_pointer_again
TEST_CASE("the next recording opens by the pointer again", "[widget][test_drag_to_move]") {
    Overlay overlay;
    overlay->showAtCursor(WaveformOverlay::kStateRecording);
    const QPoint opened = overlay->pos();
    drag(*overlay, grabPoint(*overlay), room(*overlay));
    overlay.access.finishHide();

    overlay->showAtCursor(WaveformOverlay::kStateRecording);

    CHECK(overlay->pos() == opened);
}

// from tests/test_drag_to_move.py::test_a_drag_cannot_carry_the_overlay_off_screen
TEST_CASE("a drag cannot carry the overlay off screen", "[widget][test_drag_to_move]") {
    Overlay overlay;
    overlay->showAtCursor(WaveformOverlay::kStateRecording);
    const QRect area = overlay->screen()->availableGeometry();

    drag(*overlay, grabPoint(*overlay), QPoint(-10'000, -10'000));

    CHECK(overlay->pos() == area.topLeft() + QPoint(8, 8));
}

// from tests/test_drag_to_move.py::test_pressing_snap_still_snaps_and_does_not_drag
TEST_CASE("pressing snap still snaps and does not drag", "[widget][test_drag_to_move]") {
    Overlay overlay;
    overlay.access.setTakesClicks(true);
    overlay->setRecordingDestination(QStringLiteral("Walkthrough · 1 shot"));
    overlay->setWalkthroughControls(true);
    overlay->showAtCursor(WaveformOverlay::kStateRecording);
    // The grab lays out the controls.
    overlay->grab();
    const QPoint snap = overlay.access.controlCenter(QStringLiteral("snap"));
    QSignalSpy spy(overlay.widget.get(), &WaveformOverlay::walkthroughSnapRequested);
    const QPoint opened = overlay->pos();

    drag(*overlay, snap, room(*overlay));

    CHECK(spy.count() == 1);
    CHECK(overlay->pos() == opened);
}

// from tests/test_drag_to_move.py::test_the_settings_preview_and_embedded_overlay_do_not_drag
// The embedded overlay is Wayland's (other-platform); the preview half.
TEST_CASE("the settings preview and embedded overlay do not drag", "[widget][test_drag_to_move]") {
    QWidget host;
    WaveformOverlay preview(&host, WaveformOverlay::Mode::Preview);
    CHECK_FALSE(fv::WaveformOverlayTestAccess(preview).hasDrag());
    Overlay standalone;
    CHECK(standalone.access.hasDrag());
}

namespace {

QPoint corner(const QWidget& widget) {
    const QRect area = widget.screen()->availableGeometry();
    return area.topLeft() + QPoint(WaveformOverlay::kParkMargin, WaveformOverlay::kParkMargin);
}

} // namespace

// from tests/test_drag_to_move.py::test_a_walkthrough_sends_the_overlay_to_the_corner_until_hidden
TEST_CASE("a walkthrough sends the overlay to the corner until hidden",
          "[widget][test_drag_to_move]") {
    Overlay overlay;
    overlay.access.setTakesClicks(true);
    overlay->showAtCursor(WaveformOverlay::kStateStreaming);
    overlay->setWalkthroughControls(true);
    CHECK(overlay->pos() == corner(*overlay));

    overlay->updateStreamingText(QStringLiteral("a few words of preview text"));
    overlay->showAtCursor(WaveformOverlay::kStateStreaming);
    // Done: the controls go, the finish plays where the walkthrough was.
    overlay->setWalkthroughControls(false);
    overlay->setState(WaveformOverlay::kStateProcessing);
    CHECK(overlay->pos() == corner(*overlay));

    overlay.access.finishHide();
    overlay->showAtCursor(WaveformOverlay::kStateRecording);
    CHECK_FALSE(overlay.access.cornered());
}

// from tests/test_drag_to_move.py::test_a_walkthrough_overlay_dragged_aside_stays_there
TEST_CASE("a walkthrough overlay dragged aside stays there", "[widget][test_drag_to_move]") {
    Overlay overlay;
    overlay.access.setTakesClicks(true);
    overlay->showAtCursor(WaveformOverlay::kStateStreaming);
    overlay->setWalkthroughControls(true);
    drag(*overlay, grabPoint(*overlay), QPoint(60, 50));
    const QPoint placed = overlay->pos();
    CHECK(placed != corner(*overlay));

    overlay->updateStreamingText(QStringLiteral("a few words of preview text"));
    overlay->showAtCursor(WaveformOverlay::kStateStreaming);
    overlay->park(overlay->screen()->geometry());
    overlay->unpark();

    CHECK(overlay->pos() == placed);
}

// from tests/test_drag_to_move.py::test_a_snap_parks_the_overlay_in_its_corner_and_puts_it_back
TEST_CASE("a snap parks the overlay in its corner and puts it back",
          "[widget][test_drag_to_move]") {
    Overlay overlay;
    overlay.access.setTakesClicks(true);
    overlay->showAtCursor(WaveformOverlay::kStateStreaming);
    drag(*overlay, grabPoint(*overlay), room(*overlay));
    const QPoint placed = overlay->pos();
    const QRect area = overlay->screen()->geometry();

    overlay->park(area);
    const QPoint parked =
        area.topLeft() + QPoint(WaveformOverlay::kParkMargin, WaveformOverlay::kParkMargin);
    CHECK(overlay->pos() == parked);
    // The preview growing, or a re-show, leaves it in the corner.
    overlay->updateStreamingText(QStringLiteral("a few words of preview text"));
    overlay->showAtCursor(WaveformOverlay::kStateStreaming);
    CHECK(overlay->pos() == parked);

    overlay->unpark();
    CHECK(overlay->pos() == placed);
}

// from tests/test_drag_to_move.py::test_a_hidden_overlay_is_not_parked_and_hiding_forgets_the_park
TEST_CASE("a hidden overlay is not parked and hiding forgets the park",
          "[widget][test_drag_to_move]") {
    Overlay overlay;
    overlay.access.setTakesClicks(true);
    const QRect area = overlay->screen()->geometry();
    overlay->park(area);
    CHECK_FALSE(overlay->isVisible());
    CHECK_FALSE(overlay.access.placed());

    overlay->showAtCursor(WaveformOverlay::kStateRecording);
    overlay->park(area);
    overlay.access.finishHide();
    overlay->unpark();

    CHECK_FALSE(overlay.access.placed());
}

// from tests/test_drag_to_move.py::test_an_overlay_that_was_not_dragged_returns_by_the_pointer
TEST_CASE("an overlay that was not dragged returns by the pointer", "[widget][test_drag_to_move]") {
    Overlay overlay;
    overlay->showAtCursor(WaveformOverlay::kStateRecording);
    const QPoint opened = overlay->pos();

    overlay->park(overlay->screen()->geometry());
    overlay->unpark();

    CHECK(overlay->pos() == opened);
    CHECK_FALSE(overlay.access.placed());
}
