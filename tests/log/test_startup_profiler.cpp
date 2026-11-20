// Startup profiling hooks.
// from tests/test_main_entrypoint.py

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <string>

#include "base/testing/fake_clock.h"
#include "log/startup_profiler.h"
#include "log_test_support.h"

using fv::log::StartupProfiler;

// from tests/test_main_entrypoint.py::test_startup_profiler_records_elapsed_times
TEST_CASE("startup profiler records elapsed times", "[test_main_entrypoint]") {
    fv::FakeClock clock;
    StartupProfiler profiler(clock);
    clock.advance(std::chrono::milliseconds(250));
    profiler.mark("first");
    clock.advance(std::chrono::milliseconds(500));
    profiler.mark("second");

    CHECK(profiler.events() == StartupProfiler::Events{{"first", 0.25}, {"second", 0.75}});
}

// from tests/test_main_entrypoint.py::test_startup_profiler_logs_totals_and_deltas
TEST_CASE("startup profiler logs totals and deltas", "[test_main_entrypoint]") {
    fv::FakeClock clock;
    const StartupProfiler profiler(clock, {{"first", 0.5}, {"second", 0.8}});

    fv::logtest::CapturedLog captured;
    profiler.log_summary();
    const std::string text = captured.sink().text();

    CHECK(text.find("Startup timing summary:") != std::string::npos);
    CHECK(text.find("first") != std::string::npos);
    CHECK(text.find("total=  0.500s") != std::string::npos);
    CHECK(text.find("delta=  0.500s") != std::string::npos);
    CHECK(text.find("second") != std::string::npos);
    CHECK(text.find("total=  0.800s") != std::string::npos);
    CHECK(text.find("delta=  0.300s") != std::string::npos);
}

TEST_CASE("the summary lines are Python's %-32s and %7.3f, from ui_qt.startup_profiler at INFO",
          "[test_main_entrypoint]") {
    fv::FakeClock clock;
    const StartupProfiler profiler(clock, {{"main_window_shown", 1.23456}, {"é", 12.5}});
    fv::logtest::CapturedLog captured;
    profiler.log_summary();
    const auto entries = captured.sink().entries();
    REQUIRE(entries.size() == 3);
    for (const auto& entry : entries) {
        CHECK(entry.logger == "ui_qt.startup_profiler");
        CHECK(entry.level == fv::log::Level::Info);
    }
    CHECK(entries[0].message == "Startup timing summary:");
    CHECK(entries[1].message == "  main_window_shown                total=  1.235s delta=  1.235s");
    // Padding counts code points, as Python's str does.
    CHECK(entries[2].message == "  é                                total= 12.500s delta= 11.265s");
}
