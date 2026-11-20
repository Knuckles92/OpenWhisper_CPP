// The main window's clamp and restored-width maths, replaying the golden vectors of
// ui_qt.main_window (MainWindow._clamp_geometry and _restored_width on a stub window with the
// window's limits), plus the cases Python would have refused only later, at QRect.
#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <limits>
#include <stdexcept>
#include <vector>

#include "golden/golden.h"
#include "uimath/window_geometry.h"

using fv::Rect;
using fv::Size;

namespace {

constexpr char kModule[] = "ui_qt.main_window";
constexpr Size kMin{fv::window_size::kMinWidth, fv::window_size::kMinHeight};
// The stub's maximum height is never read; any value does.
constexpr Size kMax{fv::window_size::kMaxWidth, 16777215};

} // namespace

TEST_CASE("golden ui_qt.main_window _clamp_geometry", "[golden]") {
    fv::golden::for_each(kModule, "_clamp_geometry", [](const fv::golden::Case& c) {
        const auto available = c.arg<std::vector<int>>(4);
        const Rect candidate{c.arg<int>(0), c.arg<int>(1), c.arg<int>(2), c.arg<int>(3)};
        const Rect area{available.at(0), available.at(1), available.at(2), available.at(3)};
        const auto expected = c.result<std::vector<int>>();
        CHECK(fv::clamp_geometry(candidate, area, kMin, kMax) ==
              Rect{expected.at(0), expected.at(1), expected.at(2), expected.at(3)});
    });
}

TEST_CASE("golden ui_qt.main_window _restored_width", "[golden]") {
    fv::golden::for_each(kModule, "_restored_width", [](const fv::golden::Case& c) {
        const fv::Json geometry = c.arg<fv::Json>(0);
        if (c.raises()) {
            CHECK_THROWS_AS(
                fv::restored_width(geometry, kMin.width, kMax.width, fv::kCollapsedContentFormat),
                std::invalid_argument);
            return;
        }
        CHECK(fv::restored_width(geometry, kMin.width, kMax.width, fv::kCollapsedContentFormat) ==
              c.result<int>());
    });
}

TEST_CASE("a width that is not a whole number is refused", "[unit]") {
    const auto width = [](const fv::Json& value) {
        return fv::restored_width(fv::Json{{"width", value}}, 650, 1280, "collapsed_content_v1");
    };
    CHECK_THROWS_AS(width(700.5), std::invalid_argument);
    CHECK_THROWS_AS(width("700"), std::invalid_argument);
    CHECK_THROWS_AS(width(nullptr), std::invalid_argument);
    CHECK_THROWS_AS(width(static_cast<std::int64_t>(std::numeric_limits<int>::max()) + 1),
                    std::invalid_argument);
    CHECK(width(false) == 680);
    CHECK_THROWS_AS(fv::restored_width(fv::Json::array(), 650, 1280, "collapsed_content_v1"),
                    std::invalid_argument);
}

TEST_CASE("geometry_int takes integers and bools", "[unit]") {
    const fv::Json geometry{{"x", -5}, {"y", true}, {"w", 1.0}, {"h", 3000000000LL}};
    CHECK(fv::geometry_int(geometry, "x") == -5);
    CHECK(fv::geometry_int(geometry, "y") == 1);
    CHECK_THROWS_AS(fv::geometry_int(geometry, "w"), std::invalid_argument);
    CHECK_THROWS_AS(fv::geometry_int(geometry, "h"), std::invalid_argument);
    CHECK_THROWS_AS(fv::geometry_int(geometry, "missing"), std::invalid_argument);
}

TEST_CASE("the clamp pins a window larger than its screen to the top-left", "[unit]") {
    CHECK(fv::clamp_geometry(Rect{300, 200, 680, 680}, Rect{100, 50, 600, 500}, kMin, kMax) ==
          Rect{100, 50, 650, 660});
}
