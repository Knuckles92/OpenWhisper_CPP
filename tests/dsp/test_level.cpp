// block_level_db (services/recorder.py). Its golden vectors belong to services.recorder's case
// file, written with the recorder; these pin the formula and its edges.
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <cmath>
#include <cstdint>
#include <limits>
#include <numbers>
#include <vector>

#include "dsp/level.h"

using Catch::Matchers::WithinAbs;
using fv::dsp::block_level_db;

TEST_CASE("an empty or silent block is the histogram floor", "[level]") {
    CHECK(fv::dsp::LEVEL_HISTOGRAM_MIN_DB == -120);
    CHECK(block_level_db(std::vector<std::int16_t>{}) == -120.0);
    CHECK(block_level_db(std::vector<float>{}) == -120.0);
    CHECK(block_level_db(std::vector<std::int16_t>(1024, 0)) == -120.0);
    CHECK(block_level_db(std::vector<float>(1024, 0.0f)) == -120.0);
}

TEST_CASE("a full-scale sine is about -3 dBFS", "[level]") {
    // The block OpenWhisper's test_block_level_db measures: 1 kHz at 44.1 kHz, peak 32767.
    std::vector<std::int16_t> sine(1024);
    for (std::size_t i = 0; i < sine.size(); ++i) {
        const double t = static_cast<double>(i) / 44100;
        sine[i] = static_cast<std::int16_t>(std::sin(2 * std::numbers::pi * 1000 * t) * 32767);
    }
    CHECK_THAT(block_level_db(sine), WithinAbs(-3.0, 0.1));
}

TEST_CASE("int16 is measured against 32768 and float against 1.0", "[level]") {
    CHECK(block_level_db(std::vector<std::int16_t>(512, -32768)) == 0.0);
    CHECK(block_level_db(std::vector<float>(512, 1.0f)) == 0.0);
    CHECK_THAT(block_level_db(std::vector<float>(1024, 0.1f)), WithinAbs(-20.0, 0.01));
    CHECK_THAT(block_level_db(std::vector<std::int16_t>(64, 16384)),
               WithinAbs(20.0 * std::log10(0.5), 1e-12));
    // One LSB: 10 * log10(1) - 20 * log10(32768).
    CHECK(block_level_db(std::vector<std::int16_t>(10, 1)) == -20.0 * std::log10(32768.0));
    // The mean square is of the whole block: one sample of 32768 in four is -6.02 dB.
    CHECK_THAT(block_level_db(std::vector<std::int16_t>{-32768, 0, 0, 0}),
               WithinAbs(10.0 * std::log10(0.25), 1e-12));
}

TEST_CASE("levels under the floor are floored", "[level]") {
    CHECK(block_level_db(std::vector<float>(100, 1e-7f)) == -120.0);
    CHECK(block_level_db(std::vector<float>{std::numeric_limits<float>::quiet_NaN()}) == -120.0);
    CHECK(block_level_db(std::vector<float>{std::numeric_limits<float>::infinity()}) ==
          std::numeric_limits<double>::infinity());
    // Above full scale is not capped.
    CHECK_THAT(block_level_db(std::vector<float>(100, 2.0f)),
               WithinAbs(20.0 * std::log10(2.0), 1e-12));
}
