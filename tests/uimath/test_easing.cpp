// The golden vectors of the easing and timing helpers of ui_qt.overlays.moments.
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <cmath>

#include "overlay_test_support.h"
#include "uimath/easing.h"

using Catch::Matchers::WithinAbs;
using fv::overlay_test::matches_result;

namespace {
constexpr char kModule[] = "ui_qt.overlays.moments";
}

TEST_CASE("phase replays the golden vectors", "[golden][easing]") {
    fv::golden::for_each(kModule, "phase", [](const fv::golden::Case& c) {
        const double got = fv::phase(c.arg<double>(0), c.arg<double>(1), c.arg<double>(2));
        CHECK(matches_result(c, fv::Json(got)));
    });
}

TEST_CASE("ease_out_cubic replays the golden vectors", "[golden][easing]") {
    fv::golden::for_each(kModule, "ease_out_cubic", [](const fv::golden::Case& c) {
        CHECK(matches_result(c, fv::Json(fv::ease_out_cubic(c.arg<double>(0)))));
    });
}

TEST_CASE("ease_out_back replays the golden vectors at several overshoots", "[golden][easing]") {
    fv::golden::for_each(kModule, "ease_out_back", [](const fv::golden::Case& c) {
        const double got = c.arg_count() > 1 ? fv::ease_out_back(c.arg<double>(0), c.arg<double>(1))
                                             : fv::ease_out_back(c.arg<double>(0));
        CHECK(matches_result(c, fv::Json(got)));
    });
}

TEST_CASE("exit_opacity replays the golden vectors", "[golden][easing]") {
    fv::golden::for_each(kModule, "exit_opacity", [](const fv::golden::Case& c) {
        CHECK(matches_result(c, fv::Json(fv::exit_opacity(c.arg<double>(0), c.arg<double>(1)))));
    });
}

TEST_CASE("shake_offset replays the golden vectors", "[golden][easing]") {
    fv::golden::for_each(kModule, "shake_offset", [](const fv::golden::Case& c) {
        const double got = fv::shake_offset(c.arg<double>(0), c.arg<double>(1), c.arg<double>(2),
                                            c.arg<double>(3), c.arg<double>(4));
        CHECK(matches_result(c, fv::Json(got)));
    });
}

TEST_CASE("the easings pin their ends and the disc pop overshoots", "[easing]") {
    CHECK(fv::ease_out_cubic(0.0) == 0.0);
    CHECK(fv::ease_out_cubic(1.0) == 1.0);
    CHECK(fv::ease_out_back(0.0) == 0.0);
    CHECK_THAT(fv::ease_out_back(1.0), WithinAbs(1.0, 1e-15));
    // The pop runs past 1 and settles back.
    CHECK(fv::ease_out_back(0.6) > 1.0);
    CHECK(fv::ease_out_back(0.6, 3.0) > fv::ease_out_back(0.6, 2.2));
}

TEST_CASE("exit_opacity fades only in the last 0.3 seconds", "[easing]") {
    CHECK(fv::exit_opacity(0.0, 1.5) == 1.0);
    CHECK(fv::exit_opacity(1.2, 1.5) == 1.0);
    CHECK_THAT(fv::exit_opacity(1.35, 1.5), WithinAbs(0.5, 1e-12));
    CHECK(fv::exit_opacity(1.5, 1.5) == 0.0);
    CHECK(fv::exit_opacity(2.0, 1.5) == 0.0);
}

TEST_CASE("shake_offset is zero at both ends and swings inside", "[easing]") {
    CHECK(fv::shake_offset(0.0, 0.0, 0.4, 4.0, 3.5) == 0.0);
    CHECK(fv::shake_offset(0.4, 0.0, 0.4, 4.0, 3.5) == 0.0);
    CHECK(fv::shake_offset(0.05, 0.0, 0.4, 4.0, 3.5) != 0.0);
    // 4 * sin(2*pi*3.5*0.25) * 0.75 at a quarter of the way.
    CHECK_THAT(fv::shake_offset(0.1, 0.0, 0.4, 4.0, 3.5),
               WithinAbs(4.0 * std::sin(2.0 * 3.14159265358979323846 * 3.5 * 0.25) * 0.75, 1e-12));
}
