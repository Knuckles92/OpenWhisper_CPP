// The main window's size decisions (ui_qt/main_window.py _sync_page_size, _page_width,
// _on_transcription_collapsed, _on_sidebar_width_animated, _save_geometry and
// _restore_compact_geometry), over the cases those lines define.
#include <catch2/catch_test_macros.hpp>

#include <optional>

#include "uimath/window_fit.h"

using fv::FitInput;
using fv::PageWidthInput;

namespace {

// A page on a 1080 px screen under 130 px of title bar, tab bar and footer.
FitInput page(int content) {
    FitInput input;
    input.chrome_height = 130;
    input.content_height = content;
    input.current_height = 680;
    input.available_height = 1080;
    return input;
}

} // namespace

TEST_CASE("the window sizes come from OpenWhisper's config", "[test_window_fit]") {
    CHECK(fv::window_size::kTranscriptionExpandHeight == 840);
    CHECK(fv::window_size::kCompactWidth == 420);
    CHECK(fv::window_size::kCompactHeight == 250);
}

// main_window.py:1372-1378.
TEST_CASE("a short page fits the window to it and lowers the floor", "[test_window_fit]") {
    const fv::FitPlan plan = fv::plan_fit(page(300));
    CHECK(plan.fit == 430);
    CHECK(fv::fit_height(page(300)) == 430);
    // A short page lets the window go no shorter than the page itself.
    CHECK(plan.floor == 430);
    CHECK(plan.target == 430);
    // The floor goes down before the window shrinks.
    CHECK(plan.interim_min_height == 430);
}

TEST_CASE("a long page keeps the floor at the minimum height", "[test_window_fit]") {
    const fv::FitPlan plan = fv::plan_fit(page(700));
    CHECK(plan.fit == 830);
    CHECK(plan.floor == 660);
    CHECK(plan.target == 830);
    // A rise waits until the window is there: the interim floor is the current height's.
    CHECK(plan.interim_min_height == 660);
    CHECK(fv::fit_floor(830, 660) == 660);
    CHECK(fv::fit_floor(500, 660) == 500);
}

TEST_CASE("a page taller than the screen is capped at the available height", "[test_window_fit]") {
    FitInput input = page(2000);
    CHECK(fv::fit_height(input) == 1080);
    input.available_height = 640;
    const fv::FitPlan plan = fv::plan_fit(input);
    CHECK(plan.fit == 640);
    CHECK(plan.floor == 640);
    CHECK(plan.target == 640);
}

TEST_CASE("an elastic page keeps the height the user gives it", "[test_window_fit]") {
    FitInput input = page(400);
    input.elastic = true;
    input.user_sized = true;
    input.user_height = 900;
    CHECK(fv::plan_fit(input).target == 900);
    // The fit wins when the content needs more than the user's height.
    input.user_height = 500;
    CHECK(fv::plan_fit(input).target == 530);
    // Quick Record before any drag: no preferred height, so the fit.
    input.user_sized = false;
    input.user_height = 900;
    CHECK(fv::plan_fit(input).target == 530);
    // A page that is not elastic ignores a height it was given.
    input.elastic = false;
    input.user_sized = true;
    CHECK(fv::plan_fit(input).target == 530);
}

// main_window.py:1309-1331.
TEST_CASE("a page that needs more width widens the window and remembers its own width",
          "[test_window_fit]") {
    PageWidthInput input;
    input.need = 900;
    input.window_width = 680;
    input.content_width = 640;
    const fv::PageWidth widened = fv::page_width(input);
    CHECK(widened.width == 940);
    CHECK(widened.own_width == 680);

    SECTION("the next page that fits gets the width back") {
        PageWidthInput next;
        next.need = 500;
        next.last_need = 900;
        next.window_width = 940;
        next.content_width = 900;
        next.own_width = widened.own_width;
        const fv::PageWidth handedBack = fv::page_width(next);
        CHECK(handedBack.width == 680);
        CHECK_FALSE(handedBack.own_width.has_value());
    }
    SECTION("a wider page keeps the width it started from") {
        PageWidthInput wider;
        wider.need = 1000;
        wider.last_need = 900;
        wider.window_width = 940;
        wider.content_width = 900;
        wider.own_width = 680;
        const fv::PageWidth again = fv::page_width(wider);
        CHECK(again.width == 1040);
        CHECK(again.own_width == 680);
    }
    SECTION("an unchanged need leaves the width the user dragged") {
        PageWidthInput same;
        same.need = 900;
        same.last_need = 900;
        same.window_width = 1100;
        same.content_width = 1060;
        same.own_width = 680;
        const fv::PageWidth kept = fv::page_width(same);
        CHECK(kept.width == 1100);
        CHECK(kept.own_width == 680);
    }
}

TEST_CASE("the width handed back includes an open side panel", "[test_window_fit]") {
    PageWidthInput input;
    input.need = 500;
    input.last_need = 900;
    input.window_width = 1320;
    input.panel_width = 380;
    input.content_width = 900;
    input.own_width = 680;
    const fv::PageWidth handedBack = fv::page_width(input);
    CHECK(handedBack.width == 1060);
    CHECK_FALSE(handedBack.own_width.has_value());

    // Widening with the panel open remembers the width without it.
    PageWidthInput widen;
    widen.need = 900;
    widen.window_width = 1060;
    widen.panel_width = 380;
    widen.content_width = 640;
    const fv::PageWidth widened = fv::page_width(widen);
    CHECK(widened.width == 1320);
    CHECK(widened.own_width == 680);
}

// main_window.py:1251-1259.
TEST_CASE("collapsing a transcript remembers its height and expanding returns to it",
          "[test_window_fit]") {
    const fv::CollapseResize collapse = fv::transcript_collapse(true, 300, 900, 840, 660);
    CHECK(collapse.target_height == 600);
    CHECK(collapse.transcript_height == 900);
    REQUIRE(collapse.min_height.has_value());
    CHECK(*collapse.min_height == 600);

    const fv::CollapseResize expand = fv::transcript_collapse(false, 300, 600, 900, 600);
    CHECK(expand.target_height == 900);
    CHECK(expand.transcript_height == 900);
    CHECK_FALSE(expand.min_height.has_value());

    // A taller card than the remembered height grows past it.
    CHECK(fv::transcript_collapse(false, 400, 600, 900, 600).target_height == 1000);
}

// main_window.py:1844-1852.
TEST_CASE("the side panel adds its width on top of the page's cap", "[test_window_fit]") {
    const fv::PanelStep open = fv::side_panel_step(680, 380);
    CHECK(open.limit == 1660);
    CHECK(open.wanted == 1060);
    const fv::PanelStep wide = fv::side_panel_step(1280, 380);
    CHECK(wide.wanted == 1660);
    const fv::PanelStep closed = fv::side_panel_step(680, 0);
    CHECK(closed.limit == 1280);
    CHECK(closed.wanted == 680);
}

// main_window.py:2302-2310.
TEST_CASE("the saved width leaves out the open panel and a page's widening", "[test_window_fit]") {
    CHECK(fv::saved_window_width(1060, 650, true, std::nullopt) == 680);
    CHECK(fv::saved_window_width(900, 650, true, std::nullopt) == 650);
    CHECK(fv::saved_window_width(940, 650, false, 680) == 680);
    CHECK(fv::saved_window_width(1320, 650, true, 700) == 700);
    CHECK(fv::saved_window_width(700, 650, false, std::nullopt) == 700);
}

// main_window.py:1618-1622.
TEST_CASE("the compact window's saved position is moved onto the screen", "[test_window_fit]") {
    const fv::Rect screen{0, 0, 1920, 1080};
    const fv::Size compact{420, 250};
    CHECK(fv::compact_position({100, 200}, compact, screen) == fv::Point{100, 200});
    CHECK(fv::compact_position({-50, -10}, compact, screen) == fv::Point{0, 0});
    CHECK(fv::compact_position({1800, 1000}, compact, screen) == fv::Point{1500, 830});
    const fv::Rect second{1920, 0, 1280, 800};
    CHECK(fv::compact_position({100, 100}, compact, second) == fv::Point{1920, 100});
    // A screen narrower than the window keeps the window's right edge on it.
    CHECK(fv::compact_position({0, 0}, compact, fv::Rect{0, 0, 400, 200}) == fv::Point{-20, -50});
}
