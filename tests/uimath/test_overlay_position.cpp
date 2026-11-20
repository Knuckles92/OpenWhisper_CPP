// Ported from tests/test_overlay_position.py, plus the golden vectors of
// ui_qt.utils.overlay_position (which also hold desktop.on_screen_position).
#include <catch2/catch_test_macros.hpp>

#include <vector>

#include "golden/golden.h"
#include "uimath/overlay_position.h"

using fv::Point;
using fv::Rect;

namespace {

constexpr char kModule[] = "ui_qt.utils.overlay_position";

Rect rectArg(const fv::golden::Case& c, std::size_t index) {
    const auto v = c.arg<std::vector<int>>(index);
    return {v.at(0), v.at(1), v.at(2), v.at(3)};
}

Point pointArg(const fv::golden::Case& c, std::size_t index) {
    const auto v = c.arg<std::vector<int>>(index);
    return {v.at(0), v.at(1)};
}

Point pointResult(const fv::golden::Case& c) {
    const auto v = c.result<std::vector<int>>();
    return {v.at(0), v.at(1)};
}

// The optional trailing arguments of a case, or the C++ default.
int optionalArg(const fv::golden::Case& c, std::size_t index, int fallback) {
    return c.arg_count() > index ? c.arg<int>(index) : fallback;
}

const Rect kScreen{0, 0, 1920, 1080};

} // namespace

TEST_CASE("clamp_rect_to_available replays the golden vectors", "[golden][overlay_position]") {
    fv::golden::for_each(kModule, "clamp_rect_to_available", [](const fv::golden::Case& c) {
        const Point got =
            fv::clamp_rect_to_available(c.arg<int>(0), c.arg<int>(1), c.arg<int>(2), c.arg<int>(3),
                                        rectArg(c, 4), optionalArg(c, 5, 8));
        CHECK(got == pointResult(c));
    });
}

TEST_CASE("preferred_overlay_position replays the golden vectors", "[golden][overlay_position]") {
    fv::golden::for_each(kModule, "preferred_overlay_position", [](const fv::golden::Case& c) {
        const Point got = fv::preferred_overlay_position(
            pointArg(c, 0), c.arg<int>(1), c.arg<int>(2), rectArg(c, 3), optionalArg(c, 4, 10),
            optionalArg(c, 5, 8));
        CHECK(got == pointResult(c));
    });
}

TEST_CASE("max_height_for_anchor replays the golden vectors", "[golden][overlay_position]") {
    fv::golden::for_each(kModule, "max_height_for_anchor", [](const fv::golden::Case& c) {
        CHECK(fv::max_height_for_anchor(pointArg(c, 0), rectArg(c, 1), c.arg<int>(2),
                                        optionalArg(c, 3, 10),
                                        optionalArg(c, 4, 8)) == c.result<int>());
    });
}

TEST_CASE("on_screen_position replays the golden vectors", "[golden][overlay_position]") {
    fv::golden::for_each(kModule, "on_screen_position", [](const fv::golden::Case& c) {
        CHECK(fv::on_screen_position(rectArg(c, 0), rectArg(c, 1)) == pointResult(c));
    });
}

// from tests/test_overlay_position.py::TestClampRectToAvailable::test_keeps_in_bounds_position
TEST_CASE("keeps in bounds position", "[test_overlay_position]") {
    CHECK(fv::clamp_rect_to_available(100, 200, 300, 80, kScreen) == Point{100, 200});
}

// from
// tests/test_overlay_position.py::TestClampRectToAvailable::test_clamps_right_and_bottom_overflow
TEST_CASE("clamps right and bottom overflow", "[test_overlay_position]") {
    const Point p = fv::clamp_rect_to_available(1900, 1050, 300, 200, kScreen);
    CHECK(p.x == 1920 - 300 - 8);
    CHECK(p.y == 1080 - 200 - 8);
}

// from tests/test_overlay_position.py::TestClampRectToAvailable::test_clamps_left_and_top_overflow
TEST_CASE("clamps left and top overflow", "[test_overlay_position]") {
    CHECK(fv::clamp_rect_to_available(-50, -20, 300, 80, kScreen) == Point{8, 8});
}

// from
// tests/test_overlay_position.py::TestClampRectToAvailable::test_handles_secondary_monitor_origin
TEST_CASE("handles secondary monitor origin", "[test_overlay_position]") {
    const Rect secondary{1920, 0, 1600, 900};
    const Point p = fv::clamp_rect_to_available(3400, 850, 300, 200, secondary);
    CHECK(p.x == 1920 + 1600 - 300 - 8);
    CHECK(p.y == 900 - 200 - 8);
}

// from
// tests/test_overlay_position.py::TestPreferredOverlayPosition::test_prefers_below_right_of_anchor
TEST_CASE("prefers below right of anchor", "[test_overlay_position]") {
    CHECK(fv::preferred_overlay_position({100, 100}, 300, 80, kScreen) == Point{110, 110});
}

// from
// tests/test_overlay_position.py::TestPreferredOverlayPosition::test_flips_left_near_right_edge
TEST_CASE("flips left near right edge", "[test_overlay_position]") {
    const Point p = fv::preferred_overlay_position({1900, 100}, 300, 80, kScreen);
    CHECK(p.x == 1900 - 300 - 10);
    CHECK(p.y == 110);
}

// from
// tests/test_overlay_position.py::TestPreferredOverlayPosition::test_flips_above_near_bottom_edge
TEST_CASE("flips above near bottom edge", "[test_overlay_position]") {
    const Point p = fv::preferred_overlay_position({100, 1040}, 300, 200, kScreen);
    CHECK(p.x == 110);
    CHECK(p.y == 1040 - 200 - 10);
}

// from
// tests/test_overlay_position.py::TestPreferredOverlayPosition::test_flips_and_clamps_in_corner
TEST_CASE("flips and clamps in corner", "[test_overlay_position]") {
    const Point p = fv::preferred_overlay_position({1900, 1040}, 300, 200, kScreen);
    // Flips left and above the anchor, then stays within the margins.
    CHECK(p.x == 1900 - 300 - 10);
    CHECK(p.y == 1040 - 200 - 10);
    CHECK(p.x >= 8);
    CHECK(p.y >= 8);
    CHECK(p.x + 300 <= 1920 - 8);
    CHECK(p.y + 200 <= 1080 - 8);
}

// from
// tests/test_overlay_position.py::TestPreferredOverlayPosition::test_growing_height_near_bottom_moves_up
TEST_CASE("growing height near bottom moves up", "[test_overlay_position]") {
    const Point anchor{200, 1000};
    const Point shortOverlay = fv::preferred_overlay_position(anchor, 300, 80, kScreen);
    const Point tall = fv::preferred_overlay_position(anchor, 300, 400, kScreen);
    // Even the short overlay flips above: 1000 + 10 + 80 passes the bottom margin.
    CHECK(shortOverlay == Point{210, 1000 - 80 - 10});
    CHECK(tall.y < shortOverlay.y);
    CHECK(tall.y >= 8);
    CHECK(tall.y + 400 <= 1080 - 8);
}

// from
// tests/test_overlay_position.py::TestMaxHeightForAnchor::test_respects_configured_max_when_roomy
TEST_CASE("respects configured max when roomy", "[test_overlay_position]") {
    CHECK(fv::max_height_for_anchor({100, 100}, kScreen, 400) == 400);
}

// from
// tests/test_overlay_position.py::TestMaxHeightForAnchor::test_limits_by_space_on_short_display
TEST_CASE("limits by space on short display", "[test_overlay_position]") {
    const Rect shortScreen{0, 0, 1920, 300};
    const int height = fv::max_height_for_anchor({100, 200}, shortScreen, 400);
    // above = 182, below = 82, so the room is 182.
    CHECK(height == 200 - 10 - 8);
    CHECK(height < 400);
}

// from tests/test_overlay_position.py::TestMaxHeightForAnchor::test_never_returns_below_one
TEST_CASE("never returns below one", "[test_overlay_position]") {
    CHECK(fv::max_height_for_anchor({10, 10}, Rect{0, 0, 100, 20}, 400) >= 1);
}
