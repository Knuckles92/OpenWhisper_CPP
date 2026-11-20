// The golden vectors of ui_qt.utils.loudness (OpenWhisper has no test file of its own for it).
#include <catch2/catch_test_macros.hpp>

#include "base/json.h"
#include "golden/golden.h"
#include "uimath/loudness.h"

TEST_CASE("level_to_height replays the golden vectors", "[golden][loudness]") {
    fv::golden::for_each("ui_qt.utils.loudness", "level_to_height", [](const fv::golden::Case& c) {
        const double got = fv::level_to_height(c.arg<double>(0));
        CHECK(fv::golden::matches(c.raw().at("result"), fv::Json(got), c.float_tolerance()));
    });
}

TEST_CASE("the floor and ceiling bound the loudness", "[loudness]") {
    CHECK(fv::level_to_height(0.0) == 0.0);
    CHECK(fv::level_to_height(0.001) == 0.0);
    CHECK(fv::level_to_height(1.0) == 1.0);
    CHECK(fv::level_to_height(0.5) == 1.0);
}
