// The pure cases of tests/test_window_fit_and_title_bar.py: Settings fits the screen it opens on.
// The real-dialog cases run in the Settings shell's tests; these run the arithmetic on a stub of
// the dialog's size and minimum, as the Python tests do.
#include <catch2/catch_test_macros.hpp>

#include "uimath/dialog_fit.h"

namespace {

// SettingsDialog.DEFAULT_SIZE and MINIMUM_SIZE.
constexpr fv::Size kDefaultSize{1315, 850};
constexpr fv::Size kMinimumSize{940, 520};

} // namespace

// from
// tests/test_window_fit_and_title_bar.py::test_a_laptop_panel_at_150_percent_shrinks_the_window
TEST_CASE("a laptop panel at 150 percent shrinks the window", "[test_window_fit_and_title_bar]") {
    const fv::FitResult fit =
        fv::fit_to_screen(kDefaultSize, kMinimumSize, fv::Rect{0, 0, 1280, 720});
    CHECK(fit.size.width <= 1280 - 64);
    CHECK(fit.size.height <= 720 - 64);
    CHECK(fit.resized);
    CHECK(fit.minimum == kMinimumSize);
}

// from tests/test_window_fit_and_title_bar.py::test_a_big_screen_leaves_the_window_alone
TEST_CASE("a big screen leaves the window alone", "[test_window_fit_and_title_bar]") {
    const fv::FitResult fit =
        fv::fit_to_screen(kDefaultSize, kMinimumSize, fv::Rect{0, 0, 2560, 1440});
    CHECK(fit.size == kDefaultSize);
    CHECK_FALSE(fit.resized);
    CHECK(fit.minimum == kMinimumSize);
}

// from
// tests/test_window_fit_and_title_bar.py::test_a_screen_smaller_than_the_minimum_lowers_the_minimum
TEST_CASE("a screen smaller than the minimum lowers the minimum",
          "[test_window_fit_and_title_bar]") {
    const fv::FitResult fit =
        fv::fit_to_screen(kDefaultSize, kMinimumSize, fv::Rect{0, 0, 800, 480});
    CHECK(fit.minimum == fv::Size{736, 416});
    CHECK(fit.size == fv::Size{736, 416});
}

TEST_CASE("the room is the available geometry less 32 px a side, at least 1x1", "[dialog_fit]") {
    // The origin of the available rectangle does not matter, only its size.
    const fv::FitResult offset =
        fv::fit_to_screen(kDefaultSize, kMinimumSize, fv::Rect{100, 50, 800, 480});
    CHECK(offset.size == fv::Size{736, 416});
    const fv::FitResult tiny = fv::fit_to_screen({500, 400}, {300, 200}, fv::Rect{0, 0, 40, 60});
    CHECK(tiny.size == fv::Size{1, 1});
    CHECK(tiny.minimum == fv::Size{1, 1});
    const fv::FitResult empty = fv::fit_to_screen({500, 400}, {300, 200}, fv::Rect{});
    CHECK(empty.size == fv::Size{1, 1});
}

TEST_CASE("only the dimension that does not fit shrinks", "[dialog_fit]") {
    // Wide screen, short: the width stays, the height is cut.
    const fv::FitResult fit =
        fv::fit_to_screen(kDefaultSize, kMinimumSize, fv::Rect{0, 0, 2560, 700});
    CHECK(fit.size == fv::Size{1315, 636});
    // The 520 px minimum height fits in the room, so it stays.
    CHECK(fit.minimum == fv::Size{940, 520});
    CHECK(fit.resized);
}

TEST_CASE("an Omarchy session's smaller minimum is the caller's to pass", "[dialog_fit]") {
    const fv::FitResult fit =
        fv::fit_to_screen(kDefaultSize, fv::kOmarchyDialogMinimum, fv::Rect{0, 0, 1280, 720});
    CHECK(fit.minimum == fv::kOmarchyDialogMinimum);
    CHECK(fv::kOmarchyDialogMinimum == fv::Size{360, 240});
}
