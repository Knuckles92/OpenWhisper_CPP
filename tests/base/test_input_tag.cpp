#include <catch2/catch_test_macros.hpp>

#include <cstdint>

#include "base/input_tag.h"

using namespace fv;

TEST_CASE("the synthetic input tag fits a 32-bit extra info") {
    STATIC_CHECK(kSyntheticInputTag != 0);
    STATIC_CHECK(kSyntheticInputTag <= UINT32_MAX);
}

TEST_CASE("only injected input carrying the tag is our own") {
    CHECK(is_own_synthetic_input(true, kSyntheticInputTag));
    // Another program's injection, with no extra info or its own.
    CHECK_FALSE(is_own_synthetic_input(true, 0));
    CHECK_FALSE(is_own_synthetic_input(true, kSyntheticInputTag + 1));
    // A device's input is never injected.
    CHECK_FALSE(is_own_synthetic_input(false, kSyntheticInputTag));
    CHECK_FALSE(is_own_synthetic_input(false, 0));
}
