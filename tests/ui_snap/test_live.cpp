// Live annotation input, screenshot boundaries and mark lifetime (tests/test_snap_live.py, 17 of
// its 18 functions; test_mac_live_toolbar_explains_command_to_pin is macOS's). Nothing reaches
// the desktop: ScopedFakeDesktop poses as Windows with fake capture exclusion, foreground and
// window list, the settings are in memory and the marks fade by a FakeClock, so a test moves time
// and then runs the expiry the canvas's timer would (Python's _expire.timeout.emit()).
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <chrono>
#include <cmath>
#include <cstdlib>
#include <optional>
#include <tuple>
#include <utility>
#include <vector>

#include <QApplication>
#include <QColor>
#include <QGuiApplication>
#include <QHelpEvent>
#include <QImage>
#include <QList>
#include <QPointer>
#include <QScreen>
#include <QTest>
#include <QToolTip>
#include <QWidget>

#include "base/testing/fake_clock.h"
#include "settings/keys.h"
#include "settings/testing/memory_store.h"
#include "ui_snap/affinity.h"
#include "ui_snap/annotations.h"
#include "ui_snap/capture.h"
#include "ui_snap/capture_delay.h"
#include "ui_snap/export.h"
#include "ui_snap/geometry.h"
#include "ui_snap/live.h"
#include "ui_snap/overlay.h"
#include "ui_snap/session.h"
#include "ui_snap/testing/fake_desktop.h"
#include "ui_snap/toolbar.h"

#include "overlay_test_support.h"
#include "two_screens.h"

using namespace fv::ui_snap;
using namespace fvtest;
using fv::ui_snap::testing::FakeScreenGrabber;
using fv::ui_snap::testing::ScopedFakeDesktop;
using namespace std::chrono_literals;

namespace {

constexpr auto kLifetimePlusOne = kMarkLifetime + 1s;

// The fixture of the Python tests: in-memory settings, a pinned clock, a desktop posing as
// Windows whose focus never moves (no_focus_moves), and every live drawing closed at the end.
class Rig {
public:
    Rig() = default;
    ~Rig() {
        for (const QPointer<LiveAnnotations>& live : made_) {
            if (live) {
                live->close();
            }
        }
        flushDeletes();
    }
    Rig(const Rig&) = delete;
    Rig& operator=(const Rig&) = delete;

    LiveAnnotations* make(MarkShortcuts shortcuts = {}, QObject* parent = nullptr,
                          QWidget* panel = nullptr) {
        auto* live =
            new LiveAnnotations(parent != nullptr ? parent : &owner, panel, std::move(shortcuts),
                                {.settings = &settings, .clock = &clock});
        made_.append(live);
        return live;
    }

    void close(LiveAnnotations* live) {
        live->close();
        flushDeletes();
    }

    bool keepMarksSaved() const { return settings.get(fv::keys::walkthrough_keep_marks); }
    bool infillSaved() const { return settings.get(fv::keys::snap_box_infill); }

    ScopedFakeDesktop desktop;
    fv::MemorySettingsStore settings;
    fv::FakeClock clock;
    QObject owner;

private:
    QList<QPointer<LiveAnnotations>> made_;
};

LiveCanvas* pointedCanvas(const LiveAnnotations* live) {
    LiveCanvas* canvas = live->canvas(screenUnderCursor());
    REQUIRE(canvas != nullptr);
    return canvas;
}

void press(QWidget* widget, QPoint at, Qt::KeyboardModifiers modifiers = Qt::NoModifier) {
    mouse(widget, QEvent::MouseButtonPress, at.x(), at.y(), Qt::LeftButton, modifiers);
}

void release(QWidget* widget, QPoint at, Qt::KeyboardModifiers modifiers = Qt::NoModifier) {
    mouse(widget, QEvent::MouseButtonRelease, at.x(), at.y(), Qt::NoButton, modifiers);
}

void moveTo(QWidget* widget, QPoint at) {
    mouse(widget, QEvent::MouseMove, at.x(), at.y(), Qt::LeftButton);
}

// QTest.mousePress then QTest.mouseRelease: no move between them.
void stroke(QWidget* widget, QPoint from, QPoint to,
            Qt::KeyboardModifiers modifiers = Qt::NoModifier) {
    press(widget, from);
    release(widget, to, modifiers);
}

void clickButton(SnapToolbar* toolbar, const QString& kind, const QString& value = QString()) {
    const QRect rect = toolbar->buttonRect(kind, value);
    REQUIRE_FALSE(rect.isNull());
    click(toolbar, center(rect));
}

void clickTool(SnapToolbar* toolbar, SnapTool tool) {
    clickButton(toolbar, QStringLiteral("tool"), QLatin1String(toolName(tool)));
}

using Shots = Spy<QScreen*, QList<Annotation>>;

} // namespace

// from tests/test_snap_live.py::test_rectangle_infill_persists_for_live_drawing
TEST_CASE("rectangle infill persists for live drawing", "[test_snap_live]") {
    const SnapTool tool = GENERATE(SnapTool::Box, SnapTool::Ellipse);
    const bool infill = GENERATE(false, true);
    Rig rig;
    rig.settings.save_setting(fv::keys::snap_box_infill.key(), !infill);
    LiveAnnotations* drawing = rig.make();
    drawing->arm(tool);
    clickButton(drawing->toolbar(), QStringLiteral("infill"));
    CHECK(rig.infillSaved() == infill);
    drawing->arm(SnapTool::Pen);
    drawing->arm(tool);
    LiveCanvas* canvas = pointedCanvas(drawing);
    stroke(canvas, {80, 60}, {180, 120});
    REQUIRE_FALSE(canvas->items().empty());
    CHECK(canvas->items().back().infill == infill);
    rig.close(drawing);

    LiveAnnotations* reopened = rig.make();
    CHECK(reopened->boxInfill() == infill);
    CHECK(reopened->toolbar()->boxInfill() == infill);
}

// from tests/test_snap_live.py::test_live_shift_drag_constrains_callouts
TEST_CASE("live shift drag constrains callouts", "[test_snap_live]") {
    const SnapTool tool = GENERATE(SnapTool::Box, SnapTool::Ellipse, SnapTool::Arrow);
    Rig rig;
    LiveAnnotations* drawing = rig.make();
    CHECK_FALSE(drawing->boxInfill());
    drawing->arm(tool);
    LiveCanvas* canvas = pointedCanvas(drawing);
    press(canvas, {80, 60});
    release(canvas, {180, 130}, Qt::ShiftModifier);
    REQUIRE_FALSE(canvas->items().empty());
    const Annotation mark = canvas->items().back();
    CHECK(std::abs((mark.end.x() - mark.start.x()) - (mark.end.y() - mark.start.y())) < 1e-6);
}

// from tests/test_snap_live.py::test_live_redactions_leave_with_their_screenshot_like_other_marks
TEST_CASE("live redactions leave with their screenshot like other marks", "[test_snap_live]") {
    Rig rig;
    LiveAnnotations* drawing = rig.make();
    drawing->arm(SnapTool::Pen);
    LiveCanvas* canvas = pointedCanvas(drawing);
    QTest::keyClick(canvas, Qt::Key_R);
    CHECK(drawing->tool() == SnapTool::Redact);
    Shots drawn(drawing, &LiveAnnotations::drawn);
    const auto redact = [canvas](Qt::KeyboardModifiers modifiers = Qt::NoModifier) {
        stroke(canvas, {80, 60}, {180, 120}, modifiers);
    };

    redact();
    // The screenshot gets the cover; later automatic screenshots do not.
    REQUIRE(drawn.size() == 1);
    const QList<Annotation>& marks = std::get<1>(drawn.calls.back());
    REQUIRE_FALSE(marks.isEmpty());
    CHECK(marks[0].kind == AnnotationKind::Redaction);
    CHECK(drawing->pinnedOn(canvas->canvasScreen()).empty());
    rig.clock.advance(kLifetimePlusOne);
    canvas->expireMarks();
    CHECK(canvas->items().empty());

    // With a capture delay it leaves as the screenshot is taken.
    redact();
    drawing->setCapturePending(true);
    rig.clock.advance(kMarkLifetime + 10s);
    canvas->expireMarks();
    CHECK(canvas->items().size() == 1);
    drawing->setCapturePending(false, true);
    CHECK(canvas->items().empty());

    // Pinning one, or Keep marks, keeps it until Clear marks.
    redact(Qt::ControlModifier);
    const std::vector<Annotation> pinned = drawing->pinnedOn(canvas->canvasScreen());
    REQUIRE(pinned.size() == 1);
    rig.clock.advance(kMarkLifetime + 10s);
    canvas->expireMarks();
    CHECK(canvas->items() == pinned);
    drawing->clear();
    CHECK(drawing->pinnedOn(canvas->canvasScreen()).empty());
}

// from tests/test_snap_live.py::test_live_toolbar_close_is_dimmed_when_it_has_nothing_to_do
TEST_CASE("live toolbar close is dimmed when it has nothing to do", "[test_snap_live]") {
    Rig rig;
    LiveAnnotations* drawing = rig.make();
    SnapToolbar* toolbar = drawing->toolbar();
    Spy<> dropped(toolbar, &SnapToolbar::dropClicked);
    // No tool and no screenshot coming: it is shown as unavailable.
    CHECK_FALSE(toolbar->dropEnabled());
    clickButton(toolbar, QStringLiteral("drop"));
    CHECK(dropped.size() == 0);

    drawing->arm(SnapTool::Pen);
    CHECK(toolbar->dropEnabled());
    clickButton(toolbar, QStringLiteral("drop"));
    CHECK(dropped.size() == 1);
    CHECK_FALSE(drawing->armed());
    CHECK_FALSE(toolbar->dropEnabled());

    // A delayed screenshot of a click can be cancelled with it too.
    Spy<> cancelled(drawing, &LiveAnnotations::captureCancelled);
    drawing->setCaptureCountdown(2.0, 3.0);
    clickButton(toolbar, QStringLiteral("drop"));
    CHECK(cancelled.size() == 1);
    drawing->setCaptureCountdown(0);
    CHECK_FALSE(toolbar->dropEnabled());
}

// from tests/test_snap_live.py::test_resuming_live_drawing_reads_the_infill_choice_from_screenshots
TEST_CASE("resuming live drawing reads the infill choice from screenshots", "[test_snap_live]") {
    Rig rig;
    LiveAnnotations* drawing = rig.make();
    CHECK_FALSE(drawing->boxInfill());
    drawing->setEnabled(false);
    rig.settings.save_setting(fv::keys::snap_box_infill.key(), true);
    drawing->setEnabled(true);
    CHECK(drawing->boxInfill());
    CHECK(drawing->toolbar()->boxInfill());
}

// from
// tests/test_snap_live.py::test_auto_region_stays_click_through_until_closed_and_survives_pause
TEST_CASE("auto region stays click-through until closed and survives pause", "[test_snap_live]") {
    Rig rig;
    LiveAnnotations* drawing = rig.make();
    Spy<> changes(drawing, &LiveAnnotations::captureChanged);
    Shots shots(drawing, &LiveAnnotations::drawn);
    SnapToolbar* toolbar = drawing->toolbar();
    clickButton(toolbar, QStringLiteral("region"));
    CHECK(drawing->armed());
    CHECK(drawing->selectingRegion());
    QScreen* screen = screenUnderCursor();
    LiveCanvas* canvas = pointedCanvas(drawing);
    press(canvas, {180, 160});
    moveTo(canvas, {50, 80});
    release(canvas, {50, 80});
    REQUIRE(drawing->captureRegion().has_value());
    CHECK(drawing->captureRegion()->screen == screen);
    CHECK(drawing->captureRegion()->rect == QRect(50, 80, 130, 80));
    CHECK_FALSE(drawing->armed());
    CHECK(canvas->isVisible());
    CHECK(canvas->windowFlags().testFlag(Qt::WindowTransparentForInput));
    REQUIRE(drawing->regionClose() != nullptr);
    CHECK(drawing->regionClose()->isVisible());
    CHECK(toolbar->regionActive());
    CHECK(shots.size() == 0);

    drawing->selectRegion();
    QTest::keyClick(canvas, Qt::Key_Escape);
    REQUIRE(drawing->captureRegion().has_value());
    CHECK(drawing->captureRegion()->rect == QRect(50, 80, 130, 80));
    drawing->clear(); // Clearing marks keeps the capture region.
    CHECK(canvas->isVisible());
    drawing->setEnabled(false);
    CHECK_FALSE(canvas->isVisible());
    CHECK_FALSE(drawing->regionClose()->isVisible());
    drawing->setEnabled(true);
    CHECK(canvas->isVisible());
    CHECK(drawing->regionClose()->isVisible());

    click(drawing->regionClose(), center(drawing->regionClose()->rect()));
    CHECK_FALSE(drawing->captureRegion().has_value());
    CHECK_FALSE(drawing->regionClose()->isVisible());
    CHECK_FALSE(canvas->isVisible());
    CHECK_FALSE(toolbar->regionActive());
    CHECK(changes.size() == 2);
    CHECK(shots.size() == 0);
}

// from tests/test_snap_live.py::test_tiny_region_drag_does_not_replace_region
TEST_CASE("tiny region drag does not replace region", "[test_snap_live]") {
    Rig rig;
    LiveAnnotations* drawing = rig.make();
    drawing->selectRegion();
    LiveCanvas* canvas = pointedCanvas(drawing);
    click(canvas, QPointF(50, 80));
    CHECK_FALSE(drawing->captureRegion().has_value());
    CHECK(drawing->selectingRegion());
    CHECK(drawing->regionClose() == nullptr);
}

// from tests/test_snap_live.py::
// test_temporary_marks_survive_countdown_then_clear_with_successful_snap
TEST_CASE("temporary marks survive countdown then clear with successful snap", "[test_snap_live]") {
    const bool captured = GENERATE(false, true);
    Rig rig;
    LiveAnnotations* drawing = rig.make();
    drawing->arm(SnapTool::Pen);
    LiveCanvas* canvas = pointedCanvas(drawing);
    stroke(canvas, {50, 80}, {90, 110});
    drawing->setCapturePending(true);
    rig.clock.advance(10s);
    canvas->expireMarks();
    CHECK(canvas->items().size() == 1);
    CHECK_FALSE(canvas->expireTimer().isActive());
    // Holding a temporary mark must not turn it into a pinned one.
    CHECK(canvas->items(true).empty());
    drawing->setCapturePending(false, captured);
    CHECK(canvas->items().size() == (captured ? 0U : 1U));
    CHECK(canvas->expireTimer().isActive() == !captured);
    rig.clock.advance(kMarkLifetime);
    canvas->expireMarks();
    CHECK(canvas->items().empty());
}

// from tests/test_snap_live.py::test_delayed_snap_preserves_kept_and_individually_pinned_marks
TEST_CASE("delayed snap preserves kept and individually pinned marks", "[test_snap_live]") {
    const bool keepMarks = GENERATE(false, true);
    Rig rig;
    rig.settings.save_setting(fv::keys::walkthrough_keep_marks.key(), keepMarks);
    LiveAnnotations* drawing = rig.make();
    drawing->arm(SnapTool::Pen);
    LiveCanvas* canvas = pointedCanvas(drawing);
    stroke(canvas, {50, 80}, {90, 110});
    stroke(canvas, {70, 80}, {110, 110}, Qt::ControlModifier);
    const std::vector<Annotation> pinned = canvas->items(true);
    CHECK(pinned.size() == (keepMarks ? 2U : 1U));
    drawing->setCapturePending(true);
    drawing->setCapturePending(false, true);
    CHECK(canvas->items() == pinned);
    CHECK_FALSE(canvas->expireTimer().isActive());
}

// from tests/test_snap_live.py::test_live_tool_stays_selected_until_switched_or_toggled_off
TEST_CASE("live tool stays selected until switched or toggled off", "[test_snap_live]") {
    Rig rig;
    LiveAnnotations* drawing = rig.make();
    Shots shots(drawing, &LiveAnnotations::drawn);
    const QList<QWidget*> windows = drawing->windows();
    REQUIRE(windows.size() == 1);
    auto* toolbar = qobject_cast<SnapToolbar*>(windows.front());
    REQUIRE(toolbar == drawing->toolbar());
    QScreen* screen = screenUnderCursor();
    const QRect area = screen->geometry();
    CHECK(std::abs(toolbar->geometry().center().x() - area.center().x()) <= 1);
    CHECK(toolbar->geometry().top() == area.top() + kEdgeMargin);
    CHECK_FALSE(drawing->armed());

    clickTool(toolbar, SnapTool::Pen);
    LiveCanvas* canvas = drawing->canvas(screen);
    REQUIRE(canvas != nullptr);
    CHECK_FALSE(canvas->windowFlags().testFlag(Qt::WindowTransparentForInput));
    stroke(canvas, {50, 80}, {90, 110});
    REQUIRE(shots.size() == 1);
    CHECK(std::get<0>(shots.calls[0]) == screen);
    REQUIRE_FALSE(std::get<1>(shots.calls[0]).isEmpty());
    CHECK(std::get<1>(shots.calls[0])[0].kind == AnnotationKind::Stroke);
    CHECK(drawing->tool() == SnapTool::Pen);
    CHECK(toolbar->tool() == SnapTool::Pen);
    CHECK_FALSE(canvas->windowFlags().testFlag(Qt::WindowTransparentForInput));

    stroke(canvas, {50, 80}, {90, 110}, Qt::ControlModifier);
    CHECK(shots.size() == 2);
    CHECK(canvas->expireTimer().isActive());
    rig.clock.advance(kMarkLifetime);
    canvas->expireMarks();
    const std::vector<Annotation> pinned = canvas->items(true);
    CHECK(pinned.size() == 1);
    CHECK(canvas->items() == pinned);

    clickTool(toolbar, SnapTool::Box);
    CHECK(drawing->tool() == SnapTool::Box);
    CHECK(toolbar->tool() == SnapTool::Box);
    clickTool(toolbar, SnapTool::Box);
    CHECK_FALSE(drawing->tool().has_value());
    CHECK_FALSE(toolbar->tool().has_value());
    CHECK(canvas->windowFlags().testFlag(Qt::WindowTransparentForInput));
    CHECK(canvas->items() == pinned);

    clickTool(toolbar, SnapTool::Pen);
    QTest::keyClick(canvas, Qt::Key_C);
    CHECK(canvas->items().empty());
    QTest::keyClick(canvas, Qt::Key_Escape);
    CHECK_FALSE(drawing->armed());
    CHECK_FALSE(canvas->isVisible());
    CHECK(shots.size() == 2);
}

// from tests/test_snap_live.py::test_live_toolbar_keeps_new_marks_and_clears_them_without_arming
TEST_CASE("live toolbar keeps new marks and clears them without arming", "[test_snap_live]") {
    Rig rig;
    LiveAnnotations* drawing = rig.make();
    Shots shots(drawing, &LiveAnnotations::drawn);
    SnapToolbar* toolbar = drawing->toolbar();
    CHECK_FALSE(rig.keepMarksSaved());
    clickButton(toolbar, QStringLiteral("keep_marks"));
    CHECK(rig.keepMarksSaved());
    CHECK_FALSE(drawing->armed());

    drawing->arm(SnapTool::Pen);
    LiveCanvas* canvas = pointedCanvas(drawing);
    stroke(canvas, {50, 80}, {90, 110});
    const std::vector<Annotation> pinned = canvas->items(true);
    REQUIRE(pinned.size() == 1);
    CHECK_FALSE(canvas->expireTimer().isActive());

    QTest::keyClick(canvas, Qt::Key_K);
    CHECK_FALSE(rig.keepMarksSaved());
    stroke(canvas, {60, 90}, {100, 120});
    CHECK(canvas->expireTimer().isActive());
    CHECK(canvas->items().size() == 2);
    rig.clock.advance(kMarkLifetime);
    canvas->expireMarks();
    CHECK(canvas->items() == pinned);

    clickButton(toolbar, QStringLiteral("clear"));
    CHECK(drawing->tool() == SnapTool::Pen);
    CHECK(canvas->isVisible());
    CHECK(canvas->items().empty());
    CHECK_FALSE(canvas->expireTimer().isActive());
    CHECK(shots.size() == 2);
}

// from tests/test_snap_live.py::test_live_toolbar_tooltips_show_the_active_shortcuts
TEST_CASE("live toolbar tooltips show the active shortcuts", "[test_snap_live]") {
    Rig rig;
    LiveAnnotations* drawing = rig.make({{QStringLiteral("keep_marks"), QStringLiteral("F8")},
                                         {QStringLiteral("clear"), QStringLiteral("F9")}});
    const QList<QWidget*> windows = drawing->windows();
    REQUIRE(windows.size() == 1);
    QWidget* toolbar = windows.front();
    CHECK(toolbar->testAttribute(Qt::WA_AlwaysShowToolTips));
    const std::vector<std::pair<QString, QString>> expected{
        {QStringLiteral("keep_marks"), QStringLiteral("Keep marks (F8)")},
        {QStringLiteral("clear"), QStringLiteral("Clear marks (F9)")},
    };
    for (const auto& [button, tip] : expected) {
        const QPoint pos = drawing->toolbar()->buttonRect(button).center();
        QHelpEvent help(QEvent::ToolTip, pos, toolbar->mapToGlobal(pos));
        QApplication::sendEvent(toolbar, &help);
        CHECK(QToolTip::text() == tip);
    }
    QToolTip::hideText();
}

// from tests/test_snap_live.py::test_live_tool_survives_focus_loss_while_capturing
TEST_CASE("live tool survives focus loss while capturing", "[test_snap_live]") {
    Rig rig;
    LiveAnnotations* drawing = rig.make();
    drawing->arm(SnapTool::Pen);
    LiveCanvas* canvas = pointedCanvas(drawing);
    canvas->activateWindow();
    REQUIRE(pumpUntil([canvas] { return QApplication::activeWindow() == canvas; }));

    drawing->setBusy(true);
    canvas->hide();
    QWidget elsewhere;
    elsewhere.resize(40, 40);
    elsewhere.show();
    elsewhere.activateWindow();
    REQUIRE(pumpUntil([&elsewhere] { return QApplication::activeWindow() == &elsewhere; }));
    drawing->checkFocus();
    CHECK(drawing->tool() == SnapTool::Pen);

    canvas->show();
    drawing->setBusy(false);
    QApplication::processEvents();
    CHECK(pumpUntil([canvas] { return QApplication::activeWindow() == canvas; }));
    CHECK(drawing->tool() == SnapTool::Pen);
}

// from tests/test_snap_live.py::test_live_text_saves_on_commit
TEST_CASE("live text saves on commit", "[test_snap_live]") {
    Rig rig;
    LiveAnnotations* drawing = rig.make();
    Shots shots(drawing, &LiveAnnotations::drawn);
    drawing->arm(SnapTool::Text);
    LiveCanvas* canvas = pointedCanvas(drawing);
    click(canvas, QPointF(90, 110));
    CHECK(shots.size() == 0);
    LabelEditor* editor = canvas->labelEditor();
    REQUIRE(editor != nullptr);
    QTest::keyClicks(editor, QStringLiteral("This field"));
    QTest::keyClick(editor, Qt::Key_Return);
    REQUIRE(shots.size() == 1);
    const QList<Annotation>& marks = std::get<1>(shots.calls[0]);
    REQUIRE(marks.size() == 1);
    CHECK(marks[0].kind == AnnotationKind::Label);
    CHECK(marks[0].text == QStringLiteral("This field"));
    CHECK(drawing->tool() == SnapTool::Text);
    flushDeletes();
}

// from tests/test_snap_live.py::test_pinned_marks_follow_the_clicked_screen_and_are_rendered_once
TEST_CASE("pinned marks follow the clicked screen and are rendered once", "[test_snap_live]") {
    Rig rig;
    // No capture exclusion here (OpenWhisper's test poses as macOS): our windows are hidden
    // around the grab. Every native point is on the pointer's monitor.
    rig.desktop.win32.affinityResult = false;
    QScreen* screen = screenUnderCursor();
    const QRect geometry = screen->geometry();
    rig.desktop.win32.monitor =
        NativeRect{geometry.left(), geometry.top(), geometry.right() + 1, geometry.bottom() + 1};
    QWidget panel;
    panel.resize(120, 300);
    panel.show();
    LiveAnnotations* drawing = rig.make({}, &panel, &panel);

    drawing->arm(SnapTool::Pen);
    LiveCanvas* canvas = drawing->canvas(screen);
    REQUIRE(canvas != nullptr);
    stroke(canvas, {50, 80}, {90, 110}, Qt::ControlModifier);
    CHECK(drawing->tool() == SnapTool::Pen);
    QTest::keyClick(canvas, Qt::Key_Escape);
    CHECK_FALSE(drawing->armed());
    const std::vector<Annotation> marks = drawing->pinnedAt(QPoint(-120, 25));
    REQUIRE(marks.size() == 1);
    QList<QPointer<QWidget>> windows{&panel};
    for (QWidget* window : drawing->windows()) {
        windows.append(window);
    }
    QList<QWidget*> visibleAtGrab;
    rig.desktop.grabber.image = [&](QScreen& target) -> std::optional<QImage> {
        CHECK(&target == screen);
        for (const QPointer<QWidget>& window : windows) {
            if (window && window->isVisible()) {
                visibleAtGrab.append(window);
            }
        }
        QImage image(target.geometry().size() * 2, QImage::Format_RGB32);
        image.fill(QColor(QStringLiteral("#194754")));
        return image;
    };

    QPointer<SnapSession> snap =
        startSnap({.keepOut = windows, .take = SnapTake::Screen, .clickAt = QPoint(-120, 25)});
    Spy<QImage> kept(snap.data(), &SnapSession::kept);
    REQUIRE(pumpUntil([&kept] { return kept.size() == 1; }));
    QApplication::processEvents();
    const QImage raw = std::get<0>(kept.calls[0]);
    CHECK(visibleAtGrab.isEmpty());
    CHECK(raw.devicePixelRatio() == 2);
    for (const QPointer<QWidget>& window : windows) {
        REQUIRE(window);
        CHECK(window->isVisible());
    }
    const QImage result = renderKept(raw, logicalRect(raw), marks);
    CHECK(result.pixelColor(140, 190) != raw.pixelColor(140, 190));
    CHECK(result.devicePixelRatio() == 2);
    if (snap && snap->isActive()) {
        snap->drop();
    }
    rig.close(drawing);
    flushDeletes();
}

// from tests/test_snap_live.py::
// test_delay_chip_cancel_keeps_the_tool_and_label_escape_cancels_the_shot
TEST_CASE("delay chip cancel keeps the tool and label escape cancels the shot",
          "[test_snap_live]") {
    Rig rig;
    LiveAnnotations* drawing = rig.make();
    Spy<> cancelled(drawing, &LiveAnnotations::captureCancelled);
    drawing->arm(SnapTool::Pen);
    Q_EMIT drawing->captureDelay()->cancelled();
    CHECK(cancelled.size() == 1);
    CHECK(drawing->tool() == SnapTool::Pen);

    drawing->arm(SnapTool::Text);
    LiveCanvas* canvas = pointedCanvas(drawing);
    click(canvas, QPointF(80, 60));
    REQUIRE(canvas->labelEditor() != nullptr);
    Q_EMIT canvas->labelEditor()->cancelled();
    CHECK(cancelled.size() == 2);
    CHECK_FALSE(drawing->armed());
}

// from tests/test_snap_live.py::test_a_captured_delayed_shot_clears_only_its_own_monitors_marks
TEST_CASE("a captured delayed shot clears only its own monitor's marks", "[test_snap_live]") {
    ScopedScreens screens({QRect(0, 0, 800, 600), QRect(800, 0, 640, 480)});
    Rig rig;
    LiveAnnotations* drawing = rig.make();
    drawing->arm(SnapTool::Pen);
    LiveCanvas* canvas = pointedCanvas(drawing);
    press(canvas, {80, 60});
    moveTo(canvas, {120, 90});
    release(canvas, {180, 120});
    // The screenshot was of another monitor: this one's marks were not in it. Without a second
    // screen, a screen that has gone stands for it.
    QScreen* other = nullptr;
    for (QScreen* screen : QGuiApplication::screens()) {
        if (screen != canvas->canvasScreen()) {
            other = screen;
        }
    }
    drawing->setCapturePending(true);
    drawing->setCapturePending(false, true, other);
    CHECK(canvas->items().size() == 1);
    drawing->setCapturePending(true);
    drawing->setCapturePending(false, true, canvas->canvasScreen());
    CHECK(canvas->items().empty());
}

// ---- what the Python tests do not cover ----

TEST_CASE("arming covers every screen and reads the window edges first", "[test_snap_live]") {
    ScopedScreens screens({QRect(0, 0, 800, 600), QRect(800, 0, 640, 480)});
    Rig rig;
    rig.desktop.lister.add(7, {100, 100, 400, 300});
    rig.desktop.win32.foreground = 42;
    LiveAnnotations* drawing = rig.make();
    Spy<> arming(drawing, &LiveAnnotations::arming);
    drawing->arm(SnapTool::Box);
    drawing->arm(SnapTool::Arrow); // already armed: no second arming
    CHECK(arming.size() == 1);
    CHECK(drawing->canvases().size() == QGuiApplication::screens().size());
    LiveCanvas* canvas = drawing->canvas(QGuiApplication::primaryScreen());
    REQUIRE(canvas != nullptr);
    REQUIRE_FALSE(canvas->targets().isEmpty());
    CHECK(canvas->targets().front() == canvas->rect());
    CHECK(canvas->targets().contains(QRect(100, 100, 300, 200)));
    CHECK(canvas->isVisible());
    CHECK(fv::ui_snap::isKeptOut(canvas));
    // Disarming hands the foreground back to the window that had it, once a canvas has it.
    rig.desktop.win32.foreground = canvas->winId();
    drawing->disarm();
    CHECK(rig.desktop.win32.foreground == 42);
}

TEST_CASE("snapping live applies only to arrows, boxes and ellipses", "[test_snap_live]") {
    Rig rig;
    LiveAnnotations* drawing = rig.make();
    REQUIRE(drawing->snapEnabled());
    drawing->arm(SnapTool::Box);
    LiveCanvas* canvas = pointedCanvas(drawing);
    stroke(canvas, {5, 5}, {120, 90});
    REQUIRE_FALSE(canvas->items().empty());
    CHECK(canvas->items().back().start == QPointF(0, 0));
    drawing->arm(SnapTool::Pen);
    stroke(canvas, {5, 5}, {120, 90});
    CHECK(canvas->items().back().points.front() == QPointF(5, 5));
    // S toggles snapping and saves it.
    QTest::keyClick(canvas, Qt::Key_S);
    CHECK_FALSE(drawing->snapEnabled());
    CHECK_FALSE(rig.settings.get(fv::keys::snap_to_edges));
    drawing->arm(SnapTool::Box);
    stroke(canvas, {5, 5}, {120, 90});
    CHECK(canvas->items().back().start == QPointF(5, 5));
}

TEST_CASE("step numbers count per canvas and Clear starts them again", "[test_snap_live]") {
    Rig rig;
    LiveAnnotations* drawing = rig.make();
    Spy<> cancelled(drawing, &LiveAnnotations::captureCancelled);
    drawing->arm(SnapTool::Step);
    LiveCanvas* canvas = pointedCanvas(drawing);
    click(canvas, QPointF(40, 40));
    click(canvas, QPointF(80, 40));
    REQUIRE(canvas->items().size() == 2);
    CHECK(canvas->items()[1].number == 2);
    CHECK(canvas->nextStep() == 3);
    drawing->clear();
    CHECK(cancelled.size() == 1);
    CHECK(canvas->nextStep() == 1);
    click(canvas, QPointF(40, 40));
    CHECK(canvas->items().back().number == 1);
}

TEST_CASE("the region badge and close control sit where OpenWhisper puts them",
          "[test_snap_live]") {
    Rig rig;
    LiveAnnotations* drawing = rig.make();
    QScreen* screen = screenUnderCursor();
    drawing->selectRegion();
    drawing->setRegion(screen, QRect(50, 80, 130, 80));
    LiveRegionClose* close = drawing->regionClose();
    REQUIRE(close != nullptr);
    CHECK(close->size() == QSize(24, 24));
    CHECK(close->windowFlags().testFlag(Qt::WindowDoesNotAcceptFocus));
    const QRect area = screen->geometry();
    CHECK(close->pos() == QPoint(area.x() + 50 + 130 - 24, area.y() + 80 - 24 - 3));
    CHECK(close->toolTip() == QStringLiteral(u"Close region · capture the whole screen"));
    CHECK(close->accessibleName() == QStringLiteral("Close region and capture the whole screen"));
    // Against the screen's top edge the control stays on screen.
    drawing->setRegion(screen, QRect(0, 0, 60, 60));
    CHECK(close->pos().y() == area.y());
    // A region under 4 px either way does not replace the current one.
    drawing->setRegion(screen, QRect(10, 10, 3, 200));
    CHECK(drawing->captureRegion()->rect == QRect(0, 0, 60, 60));
    CHECK(QString::fromUtf16(kRegionHint) ==
          QStringLiteral(u"Drag a capture region · Esc to cancel"));
}

TEST_CASE("mark shortcuts toggle Keep marks and clear only while shown", "[test_snap_live]") {
    Rig rig;
    LiveAnnotations* drawing = rig.make();
    drawing->handleMarkShortcut("keep_marks");
    CHECK(drawing->keepMarks());
    CHECK(rig.keepMarksSaved());
    Spy<> cancelled(drawing, &LiveAnnotations::captureCancelled);
    drawing->handleMarkShortcut("clear");
    CHECK(cancelled.size() == 1);
    Spy<bool> enabled(drawing, &LiveAnnotations::enabledChanged);
    drawing->setEnabled(false);
    REQUIRE(enabled.size() == 1);
    CHECK_FALSE(std::get<0>(enabled.calls[0]));
    drawing->handleMarkShortcut("keep_marks");
    CHECK(drawing->keepMarks());
    CHECK_FALSE(drawing->toolbar()->isVisible());
}

TEST_CASE("a busy drawing takes no tool and dims its toolbar", "[test_snap_live]") {
    Rig rig;
    LiveAnnotations* drawing = rig.make();
    drawing->setBusy(true);
    CHECK_FALSE(drawing->toolbar()->isEnabled());
    drawing->arm(SnapTool::Pen);
    CHECK_FALSE(drawing->armed());
    drawing->setBusy(false);
    CHECK(drawing->toolbar()->isEnabled());
    drawing->arm(SnapTool::Pen);
    CHECK(drawing->armed());
}

TEST_CASE("a removed screen disarms, loses its region and canvas", "[test_snap_live]") {
    ScopedScreens screens({QRect(0, 0, 800, 600), QRect(800, 0, 640, 480)});
    if (!screens.available()) {
        SKIP("the offscreen platform takes no second screen here");
    }
    Rig rig;
    LiveAnnotations* drawing = rig.make();
    QScreen* second = QGuiApplication::screens().at(1);
    drawing->arm(SnapTool::Pen);
    REQUIRE(drawing->canvas(second) != nullptr);
    drawing->setRegion(second, QRect(10, 10, 100, 100));
    drawing->arm(SnapTool::Pen);
    {
        ScopedScreens one({QRect(0, 0, 800, 600)});
        QApplication::processEvents();
        flushDeletes();
        CHECK_FALSE(drawing->armed());
        CHECK_FALSE(drawing->captureRegion().has_value());
        CHECK(drawing->canvases().size() == QGuiApplication::screens().size());
        CHECK(drawing->toolbar()->isVisible());
    }
}
