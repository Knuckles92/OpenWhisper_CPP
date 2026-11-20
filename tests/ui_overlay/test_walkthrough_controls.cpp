// A walkthrough's Snap and Done on the recording overlay, and the column standing in for it
// (tests/test_walkthrough_buttons.py, the overlay's tests; ui_overlay/walkthrough_controls.h).
#include <catch2/catch_test_macros.hpp>

#include <string>
#include <utility>

#include <QApplication>
#include <QFontMetrics>
#include <QLabel>
#include <QMouseEvent>
#include <QSignalSpy>

#include "golden/golden.h"
#include "overlay_test_support.h"
#include "ui_overlay/walkthrough_controls.h"
#include "uimath/overlay_layout.h"

using fv::WaveformOverlay;
using fvtest::Overlay;
namespace golden = fv::golden;

namespace {

const QString kSnap = QStringLiteral("snap");
const QString kFinish = QStringLiteral("finish");

// The fixture: an overlay on a platform that takes overlay clicks.
struct ClickableOverlay : Overlay {
    ClickableOverlay() { access.setTakesClicks(true); }
};

// _recording_walkthrough.
void recordingWalkthrough(Overlay& overlay) {
    overlay->setRecordingDestination(QStringLiteral("Walkthrough · 1 shot"));
    overlay->setWalkthroughControls(true);
    overlay->showAtCursor(WaveformOverlay::kStateRecording);
    fvtest::paint(overlay);
}

// QTest.mouseClick: a left press and release at local.
void click(QWidget& widget, const QPoint& local) {
    fvtest::sendMouse(widget, QEvent::MouseButtonPress, local);
    fvtest::sendMouse(widget, QEvent::MouseButtonRelease, local);
}

// A pointer move over local with no button held, as the hover handler receives it.
void hover(QWidget& widget, const QPoint& local) {
    QMouseEvent event(QEvent::MouseMove, QPointF(local), QPointF(widget.mapToGlobal(local)),
                      Qt::NoButton, Qt::NoButton, Qt::NoModifier);
    QApplication::sendEvent(&widget, &event);
}

} // namespace

// from tests/test_walkthrough_buttons.py::test_snap_and_done_show_only_while_a_walkthrough_records
TEST_CASE("snap and done show only while a walkthrough records",
          "[widget][test_walkthrough_buttons]") {
    ClickableOverlay overlay;
    overlay->showAtCursor(WaveformOverlay::kStateRecording);
    fvtest::paint(overlay);
    CHECK(overlay.access.controlRects().empty());

    recordingWalkthrough(overlay);
    const auto rects = overlay.access.controlRects();
    REQUIRE(rects.size() == 2);
    REQUIRE(rects.contains(kSnap));
    REQUIRE(rects.contains(kFinish));
    const QRectF snap = rects.at(kSnap);
    const QRectF done = rects.at(kFinish);
    CHECK(snap.right() < done.left());
    CHECK(done.right() <= overlay->width());
    CHECK(snap.left() > overlay.access.badgeRect().right());

    overlay->setState(WaveformOverlay::kStateProcessing);
    fvtest::paint(overlay);
    CHECK(overlay.access.controlRects().empty());
}

// from tests/test_walkthrough_buttons.py::test_clicking_snap_and_done
TEST_CASE("clicking snap and done", "[widget][test_walkthrough_buttons]") {
    ClickableOverlay overlay;
    QSignalSpy snap(overlay.widget.get(), &WaveformOverlay::walkthroughSnapRequested);
    QSignalSpy done(overlay.widget.get(), &WaveformOverlay::walkthroughFinishRequested);
    recordingWalkthrough(overlay);

    const auto counts = [&snap, &done] {
        return std::pair<int, int>(static_cast<int>(snap.count()), static_cast<int>(done.count()));
    };
    click(*overlay, QPoint(20, 60));
    CHECK(counts() == std::pair(0, 0));
    click(*overlay, overlay.access.controlCenter(kSnap));
    CHECK(counts() == std::pair(1, 0));
    click(*overlay, overlay.access.controlCenter(kFinish));
    CHECK(counts() == std::pair(1, 1));
}

// from tests/test_walkthrough_buttons.py::test_hovering_a_control_shows_a_pointing_hand
// QTest.mouseMove warps the pointer, which the offscreen platform need not deliver; the move is
// sent to the overlay as the next test sends it.
TEST_CASE("hovering a control shows a pointing hand", "[widget][test_walkthrough_buttons]") {
    ClickableOverlay overlay;
    recordingWalkthrough(overlay);

    hover(*overlay, overlay.access.controlCenter(kSnap));
    CHECK(overlay.access.hover() == kSnap);
    CHECK(overlay->cursor().shape() == Qt::PointingHandCursor);

    overlay->setWalkthroughControls(false);
    CHECK(overlay.access.hover().isEmpty());
    CHECK(overlay.access.controlRects().empty());
}

// from tests/test_walkthrough_buttons.py::test_hovering_a_control_shows_its_shortcut
TEST_CASE("hovering a control shows its shortcut", "[widget][test_walkthrough_buttons]") {
    ClickableOverlay overlay;
    overlay->setWalkthroughShortcuts(QStringLiteral("Ctrl+Alt+S"), QString());
    recordingWalkthrough(overlay);

    hover(*overlay, overlay.access.controlCenter(kSnap));
    fv::HotkeyHoverHint* hint = overlay.access.hotkeyHint();
    REQUIRE(hint != nullptr);
    CHECK(hint->isVisible());
    const auto* label = hint->findChild<QLabel*>();
    REQUIRE(label != nullptr);
    CHECK(label->text() == QStringLiteral("Ctrl+Alt+S"));
    CHECK(hint->geometry().bottom() <
          overlay->mapToGlobal(QPoint(0, 0)).y() + overlay.access.controlRect(kSnap).top());

    // Done's Walkthrough shortcut is unset: no pill.
    hover(*overlay, overlay.access.controlCenter(kFinish));
    CHECK_FALSE(hint->isVisible());

    hover(*overlay, overlay.access.controlCenter(kSnap));
    CHECK(hint->isVisible());
    overlay->hide();
    CHECK_FALSE(hint->isVisible());
}

// from tests/test_walkthrough_buttons.py::test_beside_a_language_chip_the_controls_drop_their_words
TEST_CASE("beside a language chip the controls drop their words",
          "[widget][test_walkthrough_buttons]") {
    ClickableOverlay overlay;
    const QStringList choices{QStringLiteral("en"), QStringLiteral("es")};
    overlay->setLanguage(QStringLiteral("en"), choices);
    recordingWalkthrough(overlay);
    // A new recording reads the languages from settings; set them again, as Python does.
    overlay->setLanguage(QStringLiteral("en"), choices);
    fvtest::paint(overlay);

    const QRectF snap = overlay.access.controlRect(kSnap);
    const QRectF done = overlay.access.controlRect(kFinish);
    CHECK(done.right() < overlay.access.chipRect().left());
    CHECK(snap.left() > overlay.access.badgeRect().right());
    CHECK(snap.width() == done.width());
    CHECK(done.width() < 30);
}

// from tests/test_walkthrough_buttons.py::test_a_capture_leaves_the_recording_overlay_as_it_was
// capture.hide_windows and restore_windows (ui_snap) hide and show with Qt's own QWidget::hide
// and show, not the overlay's hide(), which would end the recording it shows.
TEST_CASE("a capture leaves the recording overlay as it was",
          "[widget][test_walkthrough_buttons]") {
    ClickableOverlay overlay;
    recordingWalkthrough(overlay);

    QWidget& window = *overlay;
    window.QWidget::hide();
    CHECK_FALSE(overlay->isVisible());
    const bool quiet = window.testAttribute(Qt::WA_ShowWithoutActivating);
    window.setAttribute(Qt::WA_ShowWithoutActivating, true);
    window.QWidget::show();
    window.setAttribute(Qt::WA_ShowWithoutActivating, quiet);

    CHECK(overlay->isVisible());
    CHECK(overlay->currentState() == WaveformOverlay::kStateRecording);
    CHECK(overlay->recordingDestination() == QStringLiteral("Walkthrough · 1 shot"));
    fvtest::paint(overlay);
    const auto rects = overlay.access.controlRects();
    CHECK(rects.size() == 2);
    CHECK(rects.contains(kSnap));
    CHECK(rects.contains(kFinish));
}

// from
// tests/test_walkthrough_buttons.py::test_snaps_keep_the_overlay_out_only_when_it_is_its_own_window
// The controller's start_snap keeps the overlay out of a snap only when it is a window of its
// own; that call is the walkthrough presenter's. This pins the overlay's half: the floating
// overlay is its own window, a preview inside a page is not.
TEST_CASE("snaps keep the overlay out only when it is its own window",
          "[widget][test_walkthrough_buttons]") {
    Overlay overlay;
    CHECK(overlay->isWindow());
    QWidget host;
    WaveformOverlay preview(&host, WaveformOverlay::Mode::Preview);
    CHECK_FALSE(preview.isWindow());
}

TEST_CASE("snap and done are offered only where the platform takes overlay clicks", "[widget]") {
    Overlay overlay;
    overlay.access.setTakesClicks(false);
    QSignalSpy snap(overlay.widget.get(), &WaveformOverlay::walkthroughSnapRequested);
    recordingWalkthrough(overlay);
    click(*overlay, QPoint(200, 18));
    CHECK(overlay.access.controlRects().empty());
    CHECK(snap.count() == 0);
    // Without them the compact look sits under the badge alone.
    CHECK(overlay->height() == overlay.access.baseHeight() + fv::kTopRow);
}

TEST_CASE("snap is the accent chip with its word, done the ghost with its word", "[widget]") {
    ClickableOverlay overlay;
    recordingWalkthrough(overlay);
    const QFontMetrics metrics(fv::walkthroughControlFont());
    const QRectF snap = overlay.access.controlRect(kSnap);
    const QRectF done = overlay.access.controlRect(kFinish);
    CHECK(snap.width() == 12 + 16 + 4 + metrics.horizontalAdvance(QStringLiteral("Snap")));
    CHECK(done.width() == 12 + 16 + 4 + metrics.horizontalAdvance(QStringLiteral("Done")));
    CHECK(done.right() == overlay->width() - fv::kBadgeMargin);
    CHECK(done.left() - snap.right() == fv::kControlGap);
    CHECK(snap.top() == fv::kBadgeMargin);
    CHECK(snap.height() == fv::kBadgeHeight);
    // The pointer over a control (2 px either side, 4 above and below) presses it.
    hover(*overlay, (snap.topLeft() + QPointF(-1.5, -3.5)).toPoint());
    CHECK(overlay.access.hover() == kSnap);
    hover(*overlay, QPoint(20, 60));
    CHECK(overlay.access.hover().isEmpty());
    CHECK(overlay->cursor().shape() == Qt::OpenHandCursor);
}

// --- the column stands in for the overlay ---------------------------------------------------

TEST_CASE("while a stand-in shows the walkthrough the overlay keeps to notices", "[widget]") {
    Overlay overlay;
    bool showing = true;
    overlay->setStandIn([&showing] { return showing; });

    overlay->showAtCursor(WaveformOverlay::kStateRecording);
    CHECK_FALSE(overlay->isVisible());
    overlay->showNotice(QStringLiteral("Couldn't save the screenshot"));
    CHECK(overlay->isVisible());
    CHECK(overlay->currentState() == WaveformOverlay::kStateNotice);
    // The notice keeps its time; the processing it would turn into stays away.
    overlay->setState(WaveformOverlay::kStateProcessing);
    CHECK(overlay->currentState() == WaveformOverlay::kStateNotice);

    overlay->hide();
    showing = false;
    overlay->showAtCursor(WaveformOverlay::kStateRecording);
    CHECK(overlay->isVisible());
    CHECK(overlay->currentState() == WaveformOverlay::kStateRecording);
}

TEST_CASE("a stand-in that opens over the recording overlay hides it", "[widget]") {
    ClickableOverlay overlay;
    recordingWalkthrough(overlay);
    CHECK(overlay->isVisible());

    overlay->setStandIn([] { return true; });

    CHECK_FALSE(overlay->isVisible());
    CHECK(overlay->currentState() == WaveformOverlay::kStateIdle);
}

TEST_CASE("a stood-in state on screen hides, and moments still show", "[widget]") {
    Overlay overlay;
    overlay->showAtCursor(WaveformOverlay::kStateRecording);
    bool showing = true;
    overlay->setStandIn([&showing] { return showing; });
    CHECK_FALSE(overlay->isVisible());

    overlay->showAtCursor(WaveformOverlay::kStateCopied);
    CHECK(overlay->isVisible());
    CHECK(overlay->currentState() == WaveformOverlay::kStateCopied);
    // Every listening and work state is the stand-in's.
    for (const QString& state :
         {WaveformOverlay::kStateStreaming, WaveformOverlay::kStateCommandListening,
          WaveformOverlay::kStateTranscribing, WaveformOverlay::kStateCleaning,
          WaveformOverlay::kStateRewriting}) {
        overlay->setState(state);
        CHECK(overlay->currentState() == WaveformOverlay::kStateCopied);
    }
    // An empty stand-in ends it.
    overlay->setStandIn({});
    overlay->setState(WaveformOverlay::kStateRecording);
    CHECK(overlay->currentState() == WaveformOverlay::kStateRecording);
}

TEST_CASE("the processing a stand-in shows hides from the overlay too", "[widget]") {
    // _STOOD_IN_STATES is the listening and the fading states: a result being worked on is the
    // column's as much as the recording.
    Overlay overlay;
    overlay->showAtCursor(WaveformOverlay::kStateRecording);
    overlay->setState(WaveformOverlay::kStateProcessing);
    REQUIRE(overlay->isVisible());
    overlay->setStandIn([] { return true; });
    CHECK_FALSE(overlay->isVisible());
    CHECK(overlay->currentState() == WaveformOverlay::kStateIdle);

    // A stand-in that starts showing later: the next work state on screen hides at once.
    bool showing = false;
    overlay->setStandIn([&showing] { return showing; });
    overlay->showAtCursor(WaveformOverlay::kStateRecording);
    overlay->setState(WaveformOverlay::kStateTranscribing);
    REQUIRE(overlay->isVisible());
    showing = true;
    overlay->setState(WaveformOverlay::kStateCleaning);
    CHECK_FALSE(overlay->isVisible());
    CHECK(overlay->currentState() == WaveformOverlay::kStateIdle);
}

// --- golden vectors ------------------------------------------------------------------------

TEST_CASE("ui_qt.overlays.waveform_overlay controls", "[golden]") {
    const QFontMetrics metrics(fv::walkthroughControlFont());
    golden::for_each("ui_qt.overlays.waveform_overlay", "controls", [&](const golden::Case& c) {
        const fv::Json params = c.arg<fv::Json>(0);
        const fv::Json expected = c.result<fv::Json>();
        const int snapWidth = metrics.horizontalAdvance(QStringLiteral("Snap"));
        const int doneWidth = metrics.horizontalAdvance(QStringLiteral("Done"));
        CHECK(expected.at("labels").at(0).get<int>() == snapWidth);
        CHECK(expected.at("labels").at(1).get<int>() == doneWidth);

        fv::WalkthroughControlsInput in;
        in.width = params.value("width", fv::kOverlayWidth);
        in.chipVisible = params.value("chip", false);
        in.chipWidth = params.value("chip_width", 30.0);
        in.badgeShown = params.value("badge", false);
        in.badgeWidth = params.value("badge_width", 60.0);
        // The words as Python measured them, so the layout is checked even where fonts differ.
        in.snapLabelWidth = expected.at("labels").at(0).get<int>();
        in.doneLabelWidth = expected.at("labels").at(1).get<int>();
        const auto placed = fv::layoutWalkthroughControls(in);

        fv::Json actual = fv::Json::array();
        for (const fv::PlacedControl& control : placed) {
            const QRectF& r = control.rect;
            actual.push_back(
                fv::Json::array({fv::walkthroughControlKey(control.control).toStdString(),
                                 fv::Json::array({r.x(), r.y(), r.width(), r.height()}),
                                 control.label.toStdString()}));
        }
        CHECK(golden::equal(expected.at("controls"), actual));
    });
}
