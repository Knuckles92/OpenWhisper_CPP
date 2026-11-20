// The snap overlay: selecting, keys, labels, the toolbar, keep and drop
// (tests/test_snap_overlay.py), offscreen on synthetic frozen screens.
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <filesystem>
#include <memory>
#include <string>
#include <tuple>
#include <vector>

#include <QApplication>
#include <QCursor>
#include <QLineEdit>
#include <QTest>
#include <QWindow>

#include "base/json.h"
#include "settings/keys.h"
#include "settings/store.h"
#include "testkit/testkit.h"
#include "ui_snap/annotations.h"
#include "ui_snap/chrome.h"
#include "ui_snap/overlay.h"
#include "ui_snap/toolbar.h"
#include "ui_theme/palette.h"

#include "overlay_test_support.h"

using namespace fv::ui_snap;
using namespace fvtest;

namespace {

// The fixture's make_overlay: shown overlays on the 640 x 400 frozen screen, closed at the end.
class Overlays {
public:
    SnapOverlay* make(double dpr = 1.0, SnapOverlay::Options options = {}) {
        auto overlay = std::make_unique<SnapOverlay>(frozenScreen(dpr), nullptr, options);
        overlay->show();
        created_.push_back(std::move(overlay));
        return created_.back().get();
    }
    ~Overlays() {
        for (const auto& overlay : created_) {
            overlay->hide();
        }
    }

private:
    std::vector<std::unique_ptr<SnapOverlay>> created_;
};

QColor swatch(int index) {
    return QColor(QString::fromLatin1(kSwatches[index].color));
}

QRect whole() {
    return QRect(0, 0, kWidth, kHeight);
}

} // namespace

// from
// tests/test_snap_overlay.py::test_selection_snaps_to_window_and_screen_corners_until_toggled_off
TEST_CASE("selection snaps to window and screen corners until toggled off", "[test_snap_overlay]") {
    Overlays overlays;
    SnapOverlay* overlay =
        overlays.make(1.0, {.snapEnabled = true, .targets = {QRect(110, 90, 250, 160)}});
    drag(overlay, {5, 8}, {354, 248});
    CHECK(overlay->selection() == QRect(0, 0, 360, 250));

    overlay->clearSelection();
    QTest::keyClick(overlay, Qt::Key_S);
    drag(overlay, {5, 8}, {354, 248});
    CHECK(overlay->selection() == QRect(5, 8, 349, 240));
}

// ---- TestKeepAndDrop ----

// from
// tests/test_snap_overlay.py::TestKeepAndDrop::test_enter_with_no_selection_keeps_the_whole_screen
TEST_CASE("enter with no selection keeps the whole screen", "[test_snap_overlay]") {
    Overlays overlays;
    SnapOverlay* overlay = overlays.make(1.5);
    Spy<QImage> kept(overlay, &SnapOverlay::kept);

    QTest::keyClick(overlay, Qt::Key_Return);

    REQUIRE(kept.size() == 1);
    const QImage& image = std::get<0>(kept.calls[0]);
    CHECK(image.size() == QSize(960, 600));
    CHECK(image.devicePixelRatio() == 1.5);
}

// from
// tests/test_snap_overlay.py::TestKeepAndDrop::test_enter_keeps_the_dragged_selection_in_device_pixels
TEST_CASE("enter keeps the dragged selection in device pixels", "[test_snap_overlay]") {
    Overlays overlays;
    SnapOverlay* overlay = overlays.make(2.0);
    Spy<QImage> kept(overlay, &SnapOverlay::kept);

    drag(overlay, {100, 50}, {300, 150});
    CHECK(overlay->phase() == SnapPhase::Annotate);
    CHECK(overlay->selection() == QRect(100, 50, 200, 100));
    QTest::keyClick(overlay, Qt::Key_Enter, Qt::KeypadModifier);
    QTest::keyClick(overlay, Qt::Key_Return);

    REQUIRE(kept.size() == 1);
    CHECK(std::get<0>(kept.calls[0]).size() == QSize(400, 200));
}

// from tests/test_snap_overlay.py::TestKeepAndDrop::test_escape_drops_exactly_once
TEST_CASE("escape drops exactly once", "[test_snap_overlay]") {
    Overlays overlays;
    SnapOverlay* overlay = overlays.make();
    Spy<> dropped(overlay, &SnapOverlay::dropped);
    Spy<QImage> kept(overlay, &SnapOverlay::kept);

    QTest::keyClick(overlay, Qt::Key_Escape);
    QTest::keyClick(overlay, Qt::Key_Escape);
    QTest::keyClick(overlay, Qt::Key_Return);

    CHECK(dropped.size() == 1);
    CHECK(kept.size() == 0);
}

// from tests/test_snap_overlay.py::TestKeepAndDrop::test_keep_and_drop_methods_report_once
TEST_CASE("keep and drop methods report once", "[test_snap_overlay]") {
    Overlays overlays;
    SnapOverlay* first = overlays.make();
    SnapOverlay* second = overlays.make();
    Spy<QImage> kept(first, &SnapOverlay::kept);
    Spy<> notDropped(first, &SnapOverlay::dropped);
    Spy<> dropped(second, &SnapOverlay::dropped);
    Spy<QImage> notKept(second, &SnapOverlay::kept);

    first->keep();
    first->keep();
    first->drop();
    second->drop();
    second->keep();

    CHECK(kept.size() == 1);
    CHECK(notDropped.size() == 0);
    CHECK(dropped.size() == 1);
    CHECK(notKept.size() == 0);
}

// from tests/test_snap_overlay.py::TestKeepAndDrop::test_closing_the_window_is_a_cancel
TEST_CASE("closing the window is a cancel", "[test_snap_overlay]") {
    Overlays overlays;
    SnapOverlay* overlay = overlays.make();
    Spy<> dropped(overlay, &SnapOverlay::dropped);

    overlay->close();

    CHECK(dropped.size() == 1);
}

// from tests/test_snap_overlay.py::TestKeepAndDrop::test_the_kept_image_carries_the_marks
TEST_CASE("the kept image carries the marks", "[test_snap_overlay]") {
    Overlays overlays;
    SnapOverlay* overlay = overlays.make(2.0);
    Spy<QImage> kept(overlay, &SnapOverlay::kept);
    drag(overlay, {100, 50}, {300, 150});
    overlay->setTool(SnapTool::Box);
    drag(overlay, {120, 70}, {200, 120});

    overlay->keep();

    REQUIRE(kept.size() == 1);
    const QImage& image = std::get<0>(kept.calls[0]);
    // The box's left edge: local x = 20, mid-height local y = 45, at 2x.
    const QColor edge = image.pixelColor(40, 90);
    CHECK(edge.red() > 200);
    CHECK(edge.green() < 140);
    CHECK(image.pixelColor(300, 180) == QColor(QStringLiteral("#f4efe9")));
}

// ---- TestSelecting ----

// from tests/test_snap_overlay.py::TestSelecting::test_a_click_is_not_a_selection
TEST_CASE("a click is not a selection", "[test_snap_overlay]") {
    Overlays overlays;
    SnapOverlay* overlay = overlays.make();

    click(overlay, {200, 200});

    CHECK(overlay->phase() == SnapPhase::Select);
    CHECK_FALSE(overlay->selection().has_value());
    CHECK(overlay->toolbar()->isHidden());
}

// from
// tests/test_snap_overlay.py::TestSelecting::test_double_click_with_no_selection_takes_the_whole_screen
TEST_CASE("double click with no selection takes the whole screen", "[test_snap_overlay]") {
    Overlays overlays;
    SnapOverlay* overlay = overlays.make();

    click(overlay, {200, 200});
    click(overlay, {200, 200}, QEvent::MouseButtonDblClick);

    CHECK(overlay->phase() == SnapPhase::Annotate);
    CHECK(overlay->selection() == whole());
    CHECK_FALSE(overlay->toolbar()->isHidden());
}

// from tests/test_snap_overlay.py::TestSelecting::test_drags_are_clamped_to_the_screen
TEST_CASE("drags are clamped to the screen", "[test_snap_overlay]") {
    Overlays overlays;
    SnapOverlay* overlay = overlays.make();

    drag(overlay, {500, 300}, {900, 700});

    CHECK(overlay->selection() == QRect(500, 300, 140, 100));
}

// from
// tests/test_snap_overlay.py::TestSelecting::test_dragging_outside_an_unmarked_selection_starts_over
TEST_CASE("dragging outside an unmarked selection starts over", "[test_snap_overlay]") {
    Overlays overlays;
    SnapOverlay* overlay = overlays.make();
    drag(overlay, {100, 50}, {300, 150});

    drag(overlay, {400, 200}, {500, 300});

    CHECK(overlay->selection() == QRect(400, 200, 100, 100));
    CHECK(overlay->phase() == SnapPhase::Annotate);
}

// from
// tests/test_snap_overlay.py::TestSelecting::test_a_marked_selection_is_kept_when_dragging_outside_it
TEST_CASE("a marked selection is kept when dragging outside it", "[test_snap_overlay]") {
    Overlays overlays;
    SnapOverlay* overlay = overlays.make();
    drag(overlay, {100, 50}, {300, 150});
    drag(overlay, {120, 70}, {200, 120});

    drag(overlay, {400, 200}, {500, 300});

    CHECK(overlay->selection() == QRect(100, 50, 200, 100));
    CHECK(overlay->annotations().size() == 1);
}

// ---- TestToolbar ----

// from tests/test_snap_overlay.py::TestToolbar::test_wrapped_tools_stay_visible_and_clickable
TEST_CASE("wrapped tools stay visible and clickable", "[test_snap_overlay]") {
    const bool live = GENERATE(false, true);
    const int width = GENERATE(320, 480, 640, 800, 1280);
    CAPTURE(live, width);
    SnapToolbar toolbar(nullptr, fv::ui::darkPalette(),
                        live ? SnapToolbar::Variant::Live : SnapToolbar::Variant::Overlay);
    // A stand-in for the live toolbar's delay chip (CaptureDelay comes with live drawing): the
    // slot is what is checked here.
    auto* chip = new QWidget;
    if (live) {
        chip->resize(toolbar.buttonRect(QStringLiteral("delay")).size());
        toolbar.setCaptureDelay(chip);
    } else {
        delete chip;
    }
    toolbar.fitToWidth(width);
    CHECK(toolbar.width() <= width);
    const std::vector<ToolbarButton>& buttons = toolbar.buttons();
    for (std::size_t index = 0; index < buttons.size(); ++index) {
        CHECK(toolbar.rect().contains(buttons[index].rect));
        for (std::size_t other = index + 1; other < buttons.size(); ++other) {
            CHECK_FALSE(buttons[index].rect.intersects(buttons[other].rect));
        }
    }
    Spy<SnapTool> chosen(&toolbar, &SnapToolbar::toolChosen);
    click(&toolbar, center(toolbar.buttonRect(SnapTool::Redact)));
    REQUIRE(chosen.size() == 1);
    CHECK(std::get<0>(chosen.calls[0]) == SnapTool::Redact);
    if (live) {
        CHECK(toolbar.rect().contains(toolbar.captureDelay()->geometry()));
        CHECK(toolbar.captureDelay()->pos() ==
              toolbar.buttonRect(QStringLiteral("delay")).topLeft());
    }
}

// from tests/test_snap_overlay.py::TestToolbar::test_unsaved_infill_starts_off
TEST_CASE("unsaved infill starts off", "[test_snap_overlay]") {
    fv::testkit::TempDataRoot root;
    fv::SettingsStore settings(root.path() / "settings.json");
    Overlays overlays;
    SnapOverlay* overlay = overlays.make(1.0, {.settings = &settings});
    overlay->startDrawing();
    overlay->setTool(SnapTool::Box);
    drag(overlay, {80, 60}, {180, 120});
    CHECK_FALSE(overlay->toolbar()->boxInfill());
    REQUIRE(overlay->annotations().size() > 0);
    CHECK_FALSE(overlay->annotations().items().back().infill);
}

// from
// tests/test_snap_overlay.py::TestToolbar::test_box_infill_is_saved_and_restored_for_new_overlays
TEST_CASE("box infill is saved and restored for new overlays", "[test_snap_overlay]") {
    const SnapTool tool = GENERATE(SnapTool::Box, SnapTool::Ellipse);
    const bool infill = GENERATE(false, true);
    CAPTURE(toolName(tool), infill);
    fv::testkit::TempDataRoot root;
    const std::filesystem::path file = root.path() / "settings.json";
    fv::SettingsStore settings(file);
    settings.save_setting(fv::keys::snap_box_infill.key(), !infill);

    Overlays overlays;
    SnapOverlay* overlay = overlays.make(1.0, {.settings = &settings});
    overlay->startDrawing();
    SnapToolbar* toolbar = overlay->toolbar();
    // Infill applies only to rectangles, and switching tools preserves it.
    click(toolbar, center(toolbar->buttonRect(QStringLiteral("infill"))));
    CHECK(toolbar->boxInfill() != infill);
    overlay->setTool(tool);
    click(toolbar, center(toolbar->buttonRect(QStringLiteral("infill"))));
    overlay->setTool(SnapTool::Pen);
    overlay->setTool(tool);
    drag(overlay, {80, 60}, {180, 120});
    REQUIRE(overlay->annotations().size() > 0);
    CHECK(overlay->annotations().items().back().infill == infill);
    const fv::SettingsStore reread(file);
    CHECK(reread.snapshot()->at(std::string(fv::keys::snap_box_infill.key())) == infill);

    SnapOverlay* reopened = overlays.make(1.0, {.settings = &settings});
    reopened->startDrawing();
    reopened->setTool(tool);
    drag(reopened, {80, 60}, {180, 120});
    CHECK(reopened->toolbar()->boxInfill() == infill);
    REQUIRE(reopened->annotations().size() > 0);
    CHECK(reopened->annotations().items().back().infill == infill);
}

// from tests/test_snap_overlay.py::TestToolbar::test_sits_under_the_selection
TEST_CASE("sits under the selection", "[test_snap_overlay]") {
    Overlays overlays;
    SnapOverlay* overlay = overlays.make();
    drag(overlay, {100, 40}, {500, 160});

    const QRect panel = overlay->toolbar()->geometry();

    CHECK(panel.top() > overlay->selection()->bottom());
    CHECK(whole().contains(panel));
}

// from tests/test_snap_overlay.py::TestToolbar::test_flips_above_a_selection_touching_the_bottom
TEST_CASE("flips above a selection touching the bottom", "[test_snap_overlay]") {
    Overlays overlays;
    SnapOverlay* overlay = overlays.make();
    drag(overlay, {100, 250}, {500, kHeight});

    const QRect panel = overlay->toolbar()->geometry();

    CHECK(panel.bottom() < overlay->selection()->top());
    CHECK(whole().contains(panel));
}

// from
// tests/test_snap_overlay.py::TestToolbar::test_clicks_right_beside_the_panel_reach_the_screenshot
TEST_CASE("clicks right beside the panel reach the screenshot", "[test_snap_overlay]") {
    Overlays overlays;
    SnapOverlay* overlay = overlays.make();
    drag(overlay, {0, 0}, {kWidth, kHeight});
    overlay->setTool(SnapTool::Step);
    const QRect panel = overlay->toolbar()->geometry();
    REQUIRE(overlay->selection()->contains(panel));

    // Through the window, so the click lands on whichever widget is there.
    QTest::mouseClick(overlay->windowHandle(), Qt::LeftButton, Qt::NoModifier,
                      QPoint(panel.center().x(), panel.top() - 4));
    QApplication::processEvents();

    CHECK(overlay->annotations().size() == 1);
}

// from tests/test_snap_overlay.py::TestToolbar::test_its_buttons_drive_the_overlay
TEST_CASE("its buttons drive the overlay", "[test_snap_overlay]") {
    Overlays overlays;
    SnapOverlay* overlay = overlays.make();
    Spy<QImage> kept(overlay, &SnapOverlay::kept);
    drag(overlay, {100, 40}, {500, 160});
    SnapToolbar* toolbar = overlay->toolbar();

    click(toolbar, center(toolbar->buttonRect(SnapTool::Step)));
    click(toolbar, center(toolbar->buttonRect(QStringLiteral("color"), QStringLiteral("2"))));
    CHECK(overlay->tool() == SnapTool::Step);
    CHECK(overlay->colorIndex() == 2);
    click(toolbar, center(toolbar->buttonRect(QStringLiteral("keep"))));

    CHECK(kept.size() == 1);
}

// ---- TestShortcuts ----

// from tests/test_snap_overlay.py::TestShortcuts::test_letters_pick_tools
TEST_CASE("letters pick tools", "[test_snap_overlay]") {
    const auto [key, tool] = GENERATE(table<Qt::Key, SnapTool>({
        {Qt::Key_B, SnapTool::Box},
        {Qt::Key_O, SnapTool::Ellipse},
        {Qt::Key_R, SnapTool::Redact},
        {Qt::Key_P, SnapTool::Pen},
        {Qt::Key_H, SnapTool::Highlighter},
        {Qt::Key_T, SnapTool::Text},
        {Qt::Key_N, SnapTool::Step},
        {Qt::Key_A, SnapTool::Arrow},
    }));
    CAPTURE(toolName(tool));
    Overlays overlays;
    SnapOverlay* overlay = overlays.make();
    overlay->setTool(tool != SnapTool::Pen ? SnapTool::Pen : SnapTool::Box);

    QTest::keyClick(overlay, key);

    CHECK(overlay->tool() == tool);
}

// from tests/test_snap_overlay.py::TestShortcuts::test_digits_pick_colours
TEST_CASE("digits pick colours", "[test_snap_overlay]") {
    Overlays overlays;
    SnapOverlay* overlay = overlays.make();
    std::vector<int> picked;
    for (const Qt::Key key : {Qt::Key_3, Qt::Key_2, Qt::Key_4, Qt::Key_1}) {
        QTest::keyClick(overlay, key);
        picked.push_back(overlay->colorIndex());
    }

    CHECK(picked == std::vector<int>{2, 1, 3, 0});
    CHECK(overlay->currentColor() == swatch(0));
}

// from tests/test_snap_overlay.py::TestShortcuts::test_undo_and_redo
TEST_CASE("undo and redo", "[test_snap_overlay]") {
    Overlays overlays;
    SnapOverlay* overlay = overlays.make();
    drag(overlay, {100, 50}, {500, 300});
    drag(overlay, {150, 100}, {300, 200});
    drag(overlay, {150, 150}, {300, 250});
    const Qt::KeyboardModifiers ctrl = Qt::ControlModifier;
    const Qt::KeyboardModifiers ctrlShift = Qt::ControlModifier | Qt::ShiftModifier;
    std::vector<std::size_t> counts;

    for (const auto& [key, modifiers] : std::vector<std::pair<Qt::Key, Qt::KeyboardModifiers>>{
             {Qt::Key_Z, ctrl},
             {Qt::Key_Z, ctrlShift},
             {Qt::Key_Z, ctrl},
             {Qt::Key_Z, ctrl},
             {Qt::Key_Y, ctrl},
         }) {
        QTest::keyClick(overlay, key, modifiers);
        counts.push_back(overlay->annotations().size());
    }

    CHECK(counts == std::vector<std::size_t>{1, 2, 1, 0, 1});
    for (const Annotation& item : overlay->annotations().items()) {
        CHECK(item.kind == AnnotationKind::Arrow);
    }
}

// from tests/test_snap_overlay.py::TestShortcuts::test_shifted_letters_are_not_shortcuts
TEST_CASE("shifted letters are not shortcuts", "[test_snap_overlay]") {
    Overlays overlays;
    SnapOverlay* overlay = overlays.make();

    QTest::keyClick(overlay, Qt::Key_B, Qt::ShiftModifier);

    CHECK(overlay->tool() == SnapTool::Arrow);
}

// ---- TestMarks ----

// from tests/test_snap_overlay.py::TestMarks::test_shift_drag_constrains_callouts
TEST_CASE("shift drag constrains callouts", "[test_snap_overlay]") {
    const SnapTool tool = GENERATE(SnapTool::Box, SnapTool::Ellipse, SnapTool::Arrow);
    CAPTURE(toolName(tool));
    Overlays overlays;
    SnapOverlay* overlay = overlays.make();
    overlay->startDrawing();
    overlay->setTool(tool);
    drag(overlay, {80, 60}, {180, 130}, Qt::ShiftModifier);
    REQUIRE(overlay->annotations().size() > 0);
    const Annotation& mark = overlay->annotations().items().back();
    CHECK(mark.end.x() - mark.start.x() == Catch::Approx(mark.end.y() - mark.start.y()));
}

// from tests/test_snap_overlay.py::TestMarks::test_ellipse_toolbar_button_draws_an_oval
TEST_CASE("ellipse toolbar button draws an oval", "[test_snap_overlay]") {
    Overlays overlays;
    SnapOverlay* overlay = overlays.make();
    overlay->startDrawing();
    click(overlay->toolbar(), center(overlay->toolbar()->buttonRect(SnapTool::Ellipse)));
    drag(overlay, {80, 60}, {180, 120});
    REQUIRE(overlay->annotations().size() == 1);
    const Annotation& ellipse = overlay->annotations().items().front();
    CHECK(ellipse.kind == AnnotationKind::Ellipse);
    CHECK(annotationRect(ellipse) == QRectF(80, 60, 100, 60));
}

// from
// tests/test_snap_overlay.py::TestMarks::test_redact_tool_exports_an_opaque_cover_and_supports_undo
TEST_CASE("redact tool exports an opaque cover and supports undo", "[test_snap_overlay]") {
    Overlays overlays;
    SnapOverlay* overlay = overlays.make(2.0);
    overlay->startDrawing();
    overlay->setColor(2);
    SnapToolbar* toolbar = overlay->toolbar();
    click(toolbar, center(toolbar->buttonRect(SnapTool::Redact)));
    click(toolbar, center(toolbar->buttonRect(QStringLiteral("color"), QStringLiteral("1"))));
    CHECK(overlay->colorIndex() == 2); // Swatches don't recolour a redaction.
    drag(overlay, {80, 60}, {180, 120});
    REQUIRE(overlay->annotations().size() > 0);
    CHECK(overlay->annotations().items().back().kind == AnnotationKind::Redaction);
    overlay->undo();
    CHECK(overlay->annotations().size() == 0);
    overlay->redo();
    CHECK(overlay->annotations().size() == 1);
    Spy<QImage> kept(overlay, &SnapOverlay::kept);
    overlay->keep();
    REQUIRE(kept.size() == 1);
    CHECK(std::get<0>(kept.calls[0]).pixelColor(200, 160) ==
          QColor(QString::fromLatin1(kRedactionColor)));
}

// from tests/test_snap_overlay.py::TestMarks::test_steps_number_themselves_even_on_quick_clicks
TEST_CASE("steps number themselves even on quick clicks", "[test_snap_overlay]") {
    Overlays overlays;
    SnapOverlay* overlay = overlays.make();
    drag(overlay, {100, 50}, {500, 300});
    overlay->setTool(SnapTool::Step);

    click(overlay, {150, 100});
    click(overlay, {200, 100}, QEvent::MouseButtonDblClick);

    std::vector<Annotation> steps;
    for (const Annotation& item : overlay->annotations().items()) {
        if (item.kind == AnnotationKind::Step) {
            steps.push_back(item);
        }
    }
    REQUIRE(steps.size() == 2);
    CHECK(steps[0].number == 1);
    CHECK(steps[1].number == 2);
    CHECK(steps[0].center == QPointF(150, 100));
}

// from tests/test_snap_overlay.py::TestMarks::test_box_marks_are_pinned_to_the_screen
TEST_CASE("box marks are pinned to the screen", "[test_snap_overlay]") {
    Overlays overlays;
    SnapOverlay* overlay = overlays.make();
    drag(overlay, {100, 50}, {500, 300});
    overlay->setTool(SnapTool::Box);

    drag(overlay, {120, 70}, {220, 170});

    REQUIRE(overlay->annotations().size() == 1);
    const Annotation& box = overlay->annotations().items().front();
    CHECK(box.kind == AnnotationKind::Box);
    CHECK(annotationRect(box) == QRectF(120, 70, 100, 100));
}

// ---- TestAdjusting ----

// from
// tests/test_snap_overlay.py::TestAdjusting::test_dragging_a_corner_resizes_against_the_opposite_one
TEST_CASE("dragging a corner resizes against the opposite one", "[test_snap_overlay]") {
    Overlays overlays;
    SnapOverlay* overlay = overlays.make();
    drag(overlay, {100, 50}, {300, 150});

    drag(overlay, {300, 150}, {400, 250});

    CHECK(overlay->selection() == QRect(100, 50, 300, 200));
    CHECK(overlay->phase() == SnapPhase::Annotate);
    CHECK_FALSE(overlay->toolbar()->isHidden());
    CHECK(overlay->toolbar()->geometry().top() > overlay->selection()->bottom());
}

// from
// tests/test_snap_overlay.py::TestAdjusting::test_the_top_left_corner_is_grabbed_from_just_outside
TEST_CASE("the top left corner is grabbed from just outside", "[test_snap_overlay]") {
    Overlays overlays;
    SnapOverlay* overlay = overlays.make();
    drag(overlay, {100, 50}, {300, 150});

    drag(overlay, {94, 44}, {60, 20});

    CHECK(overlay->selection() == QRect(66, 26, 234, 124));
}

// from tests/test_snap_overlay.py::TestAdjusting::test_an_edge_moves_only_its_own_side
TEST_CASE("an edge moves only its own side", "[test_snap_overlay]") {
    Overlays overlays;
    SnapOverlay* overlay = overlays.make();
    drag(overlay, {100, 50}, {300, 150});

    drag(overlay, {200, 52}, {200, 10});

    CHECK(overlay->selection() == QRect(100, 8, 200, 142));
}

// from tests/test_snap_overlay.py::TestAdjusting::test_marks_stay_put_and_the_kept_crop_follows
TEST_CASE("marks stay put and the kept crop follows", "[test_snap_overlay]") {
    Overlays overlays;
    SnapOverlay* overlay = overlays.make();
    Spy<QImage> kept(overlay, &SnapOverlay::kept);
    drag(overlay, {100, 50}, {300, 150});
    overlay->setTool(SnapTool::Box);
    drag(overlay, {150, 80}, {250, 120});
    REQUIRE(overlay->annotations().size() == 1);
    const QRectF before = annotationRect(overlay->annotations().items().front());

    drag(overlay, {100, 50}, {50, 30});
    overlay->keep();

    CHECK(overlay->selection() == QRect(50, 30, 250, 120));
    CHECK(annotationRect(overlay->annotations().items().front()) == before);
    REQUIRE(kept.size() == 1);
    // The box's left edge is screen x = 150: x = 100 in the grown crop.
    const QColor edge = std::get<0>(kept.calls[0]).pixelColor(100, 70);
    CHECK(edge.red() > 200);
    CHECK(edge.green() < 140);
}

// from
// tests/test_snap_overlay.py::TestAdjusting::test_shrinking_below_the_minimum_stops_at_the_last_size
TEST_CASE("shrinking below the minimum stops at the last size", "[test_snap_overlay]") {
    Overlays overlays;
    SnapOverlay* overlay = overlays.make();
    drag(overlay, {100, 50}, {300, 150});

    drag(overlay, {300, 150}, {102, 52});

    const QRect selection = *overlay->selection();
    CHECK(selection.width() >= kMinSelection);
    CHECK(selection.height() >= kMinSelection);
}

// from tests/test_snap_overlay.py::TestAdjusting::test_hovering_an_edge_shows_a_resize_cursor
TEST_CASE("hovering an edge shows a resize cursor", "[test_snap_overlay]") {
    Overlays overlays;
    SnapOverlay* overlay = overlays.make();
    drag(overlay, {100, 50}, {300, 150});

    std::vector<Qt::CursorShape> shapes;
    for (const QPoint point : {QPoint(300, 150), QPoint(100, 150), QPoint(200, 50),
                               QPoint(300, 100), QPoint(200, 100)}) {
        mouse(overlay, QEvent::MouseMove, point.x(), point.y(), Qt::NoButton);
        shapes.push_back(overlay->cursor().shape());
    }

    CHECK(shapes == std::vector<Qt::CursorShape>{Qt::SizeFDiagCursor, Qt::SizeBDiagCursor,
                                                 Qt::SizeVerCursor, Qt::SizeHorCursor,
                                                 Qt::CrossCursor});
}

// ---- TestLabels ----

namespace {

LabelEditor* startLabel(SnapOverlay* overlay) {
    drag(overlay, {100, 50}, {500, 300});
    overlay->setTool(SnapTool::Text);
    click(overlay, {200, 150});
    REQUIRE(overlay->labelEditor() != nullptr);
    return overlay->labelEditor();
}

std::vector<QString> labelTexts(const SnapOverlay* overlay) {
    std::vector<QString> texts;
    for (const Annotation& item : overlay->annotations().items()) {
        texts.push_back(item.text);
    }
    return texts;
}

} // namespace

// from tests/test_snap_overlay.py::TestLabels::test_escape_while_typing_cancels_only_the_label
TEST_CASE("escape while typing cancels only the label", "[test_snap_overlay]") {
    Overlays overlays;
    SnapOverlay* overlay = overlays.make();
    Spy<> dropped(overlay, &SnapOverlay::dropped);
    LabelEditor* editor = startLabel(overlay);

    QTest::keyClicks(editor, QStringLiteral("Click here"));
    QTest::keyClick(editor, Qt::Key_Escape);

    CHECK(overlay->labelEditor() == nullptr);
    CHECK(overlay->annotations().size() == 0);
    CHECK(dropped.size() == 0);
    QTest::keyClick(overlay, Qt::Key_Escape);
    CHECK(dropped.size() == 1);
}

// from
// tests/test_snap_overlay.py::TestLabels::test_enter_while_typing_commits_the_label_without_keeping
TEST_CASE("enter while typing commits the label without keeping", "[test_snap_overlay]") {
    Overlays overlays;
    SnapOverlay* overlay = overlays.make();
    Spy<QImage> kept(overlay, &SnapOverlay::kept);
    LabelEditor* editor = startLabel(overlay);

    QTest::keyClicks(editor, QStringLiteral("Click here"));
    QTest::keyClick(editor, Qt::Key_Return);

    CHECK(kept.size() == 0);
    REQUIRE(overlay->annotations().size() == 1);
    const Annotation& label = overlay->annotations().items().front();
    CHECK(label.kind == AnnotationKind::Label);
    CHECK(label.text == QStringLiteral("Click here"));
    CHECK(label.color == swatch(0));
    QTest::keyClick(overlay, Qt::Key_Return);
    CHECK(kept.size() == 1);
}

// from tests/test_snap_overlay.py::TestLabels::test_letters_typed_into_a_label_are_not_shortcuts
TEST_CASE("letters typed into a label are not shortcuts", "[test_snap_overlay]") {
    Overlays overlays;
    SnapOverlay* overlay = overlays.make();
    LabelEditor* editor = startLabel(overlay);

    QTest::keyClicks(editor, QStringLiteral("abt 2"));

    CHECK(overlay->tool() == SnapTool::Text);
    CHECK(overlay->colorIndex() == 0);
    CHECK(editor->text() == QStringLiteral("abt 2"));
}

// from tests/test_snap_overlay.py::TestLabels::test_a_colour_change_recolours_the_label_being_typed
TEST_CASE("a colour change recolours the label being typed", "[test_snap_overlay]") {
    Overlays overlays;
    SnapOverlay* overlay = overlays.make();
    LabelEditor* editor = startLabel(overlay);
    QTest::keyClicks(editor, QStringLiteral("Note"));

    overlay->setColor(2);
    QTest::keyClick(editor, Qt::Key_Return);

    REQUIRE(overlay->annotations().size() == 1);
    CHECK(overlay->annotations().items().front().color == swatch(2));
}

// from tests/test_snap_overlay.py::TestLabels::test_keep_while_typing_keeps_the_label_too
TEST_CASE("keep while typing keeps the label too", "[test_snap_overlay]") {
    Overlays overlays;
    SnapOverlay* overlay = overlays.make();
    Spy<QImage> kept(overlay, &SnapOverlay::kept);
    LabelEditor* editor = startLabel(overlay);
    QTest::keyClicks(editor, QStringLiteral("Note"));

    overlay->keep();

    CHECK(kept.size() == 1);
    CHECK(labelTexts(overlay) == std::vector<QString>{QStringLiteral("Note")});
}

// from tests/test_snap_overlay.py::TestLabels::test_a_click_away_finishes_the_label
TEST_CASE("a click away finishes the label", "[test_snap_overlay]") {
    Overlays overlays;
    SnapOverlay* overlay = overlays.make();
    LabelEditor* editor = startLabel(overlay);
    QTest::keyClicks(editor, QStringLiteral("Note"));

    click(overlay, {400, 250});

    CHECK(overlay->labelEditor() == nullptr);
    CHECK(labelTexts(overlay) == std::vector<QString>{QStringLiteral("Note")});
}

// from tests/test_snap_overlay.py::TestLabels::test_tab_keeps_the_keyboard_in_the_label
TEST_CASE("tab keeps the keyboard in the label", "[test_snap_overlay]") {
    Overlays overlays;
    SnapOverlay* overlay = overlays.make();
    overlay->activateWindow();
    LabelEditor* editor = startLabel(overlay);
    editor->setFocus();
    REQUIRE(pumpUntil([editor] { return editor->hasFocus(); }));

    QTest::keyClick(editor, Qt::Key_Tab);

    CHECK(editor->hasFocus());
    CHECK(overlay->labelEditor() == editor);
}

// from tests/test_snap_overlay.py::TestLabels::test_a_long_label_stays_on_screen
TEST_CASE("a long label stays on screen", "[test_snap_overlay]") {
    Overlays overlays;
    SnapOverlay* overlay = overlays.make();
    drag(overlay, {100, 50}, {kWidth, 300});
    overlay->setTool(SnapTool::Text);
    click(overlay, {kWidth - 60, 150});
    REQUIRE(overlay->labelEditor() != nullptr);

    QTest::keyClicks(overlay->labelEditor(), QStringLiteral("A rather long note ").repeated(6));

    CHECK(overlay->labelEditor()->geometry().right() < kWidth);
}

// from tests/test_snap_overlay.py::TestLabels::test_undo_while_typing_discards_the_label
TEST_CASE("undo while typing discards the label", "[test_snap_overlay]") {
    Overlays overlays;
    SnapOverlay* overlay = overlays.make();
    LabelEditor* editor = startLabel(overlay);
    QTest::keyClicks(editor, QStringLiteral("Note"));

    overlay->undo();

    CHECK(overlay->labelEditor() == nullptr);
    CHECK(overlay->annotations().size() == 0);
}

// from tests/test_snap_overlay.py::test_window_flags_suit_each_desktop
TEST_CASE("window flags suit each desktop", "[test_snap_overlay]") {
    // Every row of OpenWhisper's table: the decision takes the system and Wayland as arguments,
    // so the other desktops' rows run here too.
    const auto [system, wayland, kind, onTop] =
        GENERATE(table<std::string, bool, Qt::WindowType, bool>({
            {"win32", false, Qt::Tool, true},
            {"darwin", false, Qt::Tool, true},
            {"linux", false, Qt::Window, true},
            {"linux", true, Qt::Window, false},
        }));
    CAPTURE(system, wayland);

    const Qt::WindowFlags flags = snapWindowFlags(system, wayland);

    CHECK((flags & Qt::WindowType_Mask) == kind);
    CHECK(flags.testFlag(Qt::FramelessWindowHint));
    CHECK(flags.testFlag(Qt::WindowStaysOnTopHint) == onTop);
}

TEST_CASE("the overlay window is this desktop's snap window", "[test_snap_overlay]") {
    Overlays overlays;
    SnapOverlay* overlay = overlays.make();
    CHECK(overlay->windowFlags() == snapWindowFlags());
    CHECK(overlay->objectName() == QStringLiteral("snapOverlay"));
    CHECK(overlay->windowTitle() == QStringLiteral("FioriVoce Snap"));
    CHECK(overlay->testAttribute(Qt::WA_OpaquePaintEvent));
    CHECK(overlay->testAttribute(Qt::WA_NoSystemBackground));
    CHECK_FALSE(overlay->testAttribute(Qt::WA_QuitOnClose));
    CHECK(overlay->property("fiorivoceHotkeyWindow").toBool());
    CHECK(overlay->cursor().shape() == Qt::CrossCursor);
    CHECK(overlay->focusPolicy() == Qt::StrongFocus);
}

// ---- TestHintPill ----

// from
// tests/test_snap_overlay.py::TestHintPill::test_a_long_hint_is_cut_short_so_the_keys_still_fit
TEST_CASE("a long hint is cut short so the keys still fit", "[test_snap_overlay]") {
    const QFont font = QApplication::font();
    const int keysOnly = HintPill(QString(), font, fv::ui::darkPalette(), 600).size().width();
    const int shortHint =
        HintPill(QStringLiteral("Snap 2"), font, fv::ui::darkPalette(), 600).size().width();
    const int longHint =
        HintPill(QStringLiteral("Walkthrough step ").repeated(40), font, fv::ui::darkPalette(), 600)
            .size()
            .width();

    CHECK(keysOnly < shortHint);
    CHECK(shortHint < longHint);
    CHECK(longHint <= 600);
}

// from tests/test_snap_overlay.py::TestHintPill::test_without_room_for_the_hint_only_the_keys_show
TEST_CASE("without room for the hint only the keys show", "[test_snap_overlay]") {
    const QFont font = QApplication::font();
    const int keysOnly = HintPill(QString(), font, fv::ui::darkPalette(), 0).size().width();

    CHECK(HintPill(QStringLiteral("Walkthrough"), font, fv::ui::darkPalette(), keysOnly + 10)
              .size()
              .width() == keysOnly);
}

// ---- LabelEditor and label_origin ----

TEST_CASE("the label editor is styled as the tag it becomes", "[test_snap_overlay]") {
    QWidget parent;
    parent.resize(640, 400);
    LabelEditor editor(&parent, swatch(1));

    CHECK(editor.objectName() == QStringLiteral("snapLabelEditor"));
    CHECK(editor.placeholderText() == QStringLiteral("Label"));
    CHECK(editor.maxLength() == 200);
    CHECK_FALSE(editor.hasFrame());
    // Amber is light: dark type, a black wash.
    const QString sheet = editor.property("unscaledStyleSheet").toString();
    CHECK(sheet ==
          QStringLiteral("QLineEdit#snapLabelEditor { background: #f5a524; color: #2a1a06; "
                         "border: none; border-radius: 6px; padding: 3px 6px; font-size: 16px; "
                         "font-weight: 600; selection-background-color: rgba(0, 0, 0, 0.35); "
                         "selection-color: #2a1a06; }"));
    CHECK(editor.width() >= 48);
}

TEST_CASE("a label starts centred on the click inside the area", "[test_snap_overlay]") {
    const QRect area(100, 50, 400, 250);
    CHECK(labelOrigin(area, QPointF(200, 150), QSize(60, 28)) == QPoint(200, 136));
    // Pushed back inside at the right and bottom edges, and at the top.
    CHECK(labelOrigin(area, QPointF(480, 295), QSize(60, 28)) == QPoint(440, 272));
    CHECK(labelOrigin(area, QPointF(90, 52), QSize(60, 28)) == QPoint(100, 50));
    // Wider than the area: its left edge.
    CHECK(labelOrigin(area, QPointF(300, 150), QSize(600, 28)) == QPoint(100, 136));
}
