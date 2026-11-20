// The Settings window fits its screen (tests/test_window_fit_and_title_bar.py). A 1920x1080
// panel at 150 % is 1280x720, smaller than Settings' default, so the window was cut off on the
// right and Hyprland placed it at y=-34. The screen is the dialog's availableScreenRect, where
// Python passed a fake screen.
#include <catch2/catch_test_macros.hpp>

#include <QApplication>
#include <QPushButton>
#include <QRect>
#include <QScreen>
#include <QSize>

#include "settings_support.h"
#include "ui_widgets/buttons.h"
#include "uimath/overlay_position.h"

using fvtest::SettingsRig;

namespace {

std::unique_ptr<fv::SettingsDialog> onScreen(SettingsRig& rig, QRect available) {
    auto dialog = rig.dialog();
    dialog->availableScreenRect = [available] {
        return available;
    };
    return dialog;
}

fv::Rect rect(const QRect& from) {
    return fv::Rect{from.x(), from.y(), from.width(), from.height()};
}

} // namespace

// from
// tests/test_window_fit_and_title_bar.py::test_a_laptop_panel_at_150_percent_shrinks_the_window
TEST_CASE("test_a_laptop_panel_at_150_percent_shrinks_the_window",
          "[test_window_fit_and_title_bar][widget]") {
    SettingsRig rig;
    auto dialog = onScreen(rig, QRect(0, 0, 1280, 720));
    REQUIRE(dialog->size() == fv::SettingsDialog::kDefaultSize);

    dialog->fitToScreen();

    CHECK(dialog->width() <= 1280 - 64);
    CHECK(dialog->height() <= 720 - 64);
    CHECK(dialog->minimumSize() == fv::SettingsDialog::kMinimumSize);
}

// from tests/test_window_fit_and_title_bar.py::test_a_big_screen_leaves_the_window_alone
TEST_CASE("test_a_big_screen_leaves_the_window_alone", "[test_window_fit_and_title_bar][widget]") {
    SettingsRig rig;
    auto dialog = onScreen(rig, QRect(0, 0, 2560, 1440));

    dialog->fitToScreen();

    CHECK(dialog->size() == fv::SettingsDialog::kDefaultSize);
}

// from
// tests/test_window_fit_and_title_bar.py::test_a_screen_smaller_than_the_minimum_lowers_the_minimum
TEST_CASE("test_a_screen_smaller_than_the_minimum_lowers_the_minimum",
          "[test_window_fit_and_title_bar][widget]") {
    SettingsRig rig;
    auto dialog = onScreen(rig, QRect(0, 0, 800, 480));

    dialog->fitToScreen();

    CHECK(dialog->minimumSize() == QSize(736, 416));
    CHECK(dialog->size() == QSize(736, 416));
}

// from tests/test_window_fit_and_title_bar.py::test_the_real_dialog_opens_inside_the_screen
TEST_CASE("test_the_real_dialog_opens_inside_the_screen",
          "[test_window_fit_and_title_bar][widget]") {
    SettingsRig rig;
    rig.registerStandIns();
    fv::SettingsDialog dialog(rig.context());
    dialog.show();
    QApplication::processEvents();
    REQUIRE(dialog.screen() != nullptr);
    const QRect available = dialog.screen()->availableGeometry();
    CHECK(dialog.width() <= available.width());
    CHECK(dialog.height() <= available.height());
    dialog.close();
}

// from tests/test_window_fit_and_title_bar.py::test_a_window_already_on_screen_stays_put
TEST_CASE("test_a_window_already_on_screen_stays_put", "[test_window_fit_and_title_bar]") {
    const QRect frame(40, 30, 900, 500);
    const fv::Point position = fv::on_screen_position(rect(frame), rect(QRect(0, 0, 1024, 640)));
    CHECK(QPoint(position.x, position.y) == frame.topLeft());
}

// from
// tests/test_window_fit_and_title_bar.py::test_a_window_off_the_right_edge_is_centred_on_the_screen
TEST_CASE("test_a_window_off_the_right_edge_is_centred_on_the_screen",
          "[test_window_fit_and_title_bar]") {
    // The Linux report: at 1024x640 Settings opened off the right edge.
    const QRect available(0, 0, 1024, 640);
    const QRect frame(300, 120, 960, 576);
    const fv::Point position = fv::on_screen_position(rect(frame), rect(available));
    CHECK(QPoint(position.x, position.y) == QPoint(32, 32));
    CHECK(available.contains(QRect(QPoint(position.x, position.y), frame.size())));
}

// from
// tests/test_window_fit_and_title_bar.py::test_a_window_bigger_than_the_screen_keeps_its_title_bar_on_it
TEST_CASE("test_a_window_bigger_than_the_screen_keeps_its_title_bar_on_it",
          "[test_window_fit_and_title_bar]") {
    const QRect available(1920, 24, 1024, 616);
    const fv::Point position =
        fv::on_screen_position(rect(QRect(2500, 300, 1315, 850)), rect(available));
    CHECK(QPoint(position.x, position.y) == QPoint(1920, 24));
}

// from
// tests/test_window_fit_and_title_bar.py::test_the_real_dialog_moves_wholly_onto_a_small_screen
TEST_CASE("test_the_real_dialog_moves_wholly_onto_a_small_screen",
          "[test_window_fit_and_title_bar][widget]") {
    SettingsRig rig;
    rig.registerStandIns();
    auto dialog = rig.dialog();
    const QRect available(0, 0, 1024, 640);
    dialog->show();
    QApplication::processEvents();
    dialog->resize(960, 560);
    dialog->move(500, 300);
    QApplication::processEvents();

    dialog->placeOnScreen(available);
    QApplication::processEvents();

    QRect frame = dialog->frameGeometry();
    if (frame == dialog->geometry()) {
        frame.adjust(0, -fv::SettingsDialog::kTitleBarAllowance, 0, 0);
    }
    CHECK(available.contains(frame));
    auto* done = dialog->findChild<QPushButton*>(QStringLiteral("modelManagerCloseButton"));
    REQUIRE(done != nullptr);
    CHECK(done->toolTip() == QStringLiteral("Close Settings (Esc)"));
    dialog->close();
}

TEST_CASE("the first show fits the window to its screen", "[widget]") {
    SettingsRig rig;
    rig.registerStandIns();
    auto dialog = onScreen(rig, QRect(0, 0, 1280, 720));
    dialog->show();
    QApplication::processEvents();
    CHECK(dialog->size() == QSize(1216, 656));
    CHECK(dialog->minimumSize() == fv::SettingsDialog::kMinimumSize);
    dialog->close();
}
