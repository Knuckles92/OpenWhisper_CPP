// The shutdown order and _attempt of services/application_controller.py (cleanup, 1298-1357).
#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "appcore/shutdown_plan.h"
#include "base/log.h"
#include "base/metrics.h"
#include "base/testing/fake_clock.h"

using fv::ShutdownPlan;
using fv::ShutdownStep;

namespace {

class CaptureSink final : public fv::log::Sink {
public:
    void write(const fv::log::Record& record) override {
        std::lock_guard lock(mutex);
        lines.emplace_back(std::string(record.logger) + "|" + std::string(record.message));
        levels.push_back(record.level);
    }
    std::mutex mutex;
    std::vector<std::string> lines;
    std::vector<fv::log::Level> levels;
};

struct Captured {
    std::shared_ptr<CaptureSink> sink = std::make_shared<CaptureSink>();
    Captured() { fv::log::add_sink(sink); }
    ~Captured() { fv::log::remove_sink(sink); }
};

} // namespace

TEST_CASE("steps run in the A03-29 order whatever order they were added in", "[shutdown_plan]") {
    fv::FakeClock clock;
    ShutdownPlan plan(clock);
    std::vector<std::string> ran;
    auto step = [&](std::string name) {
        return [&ran, name] {
            ran.push_back(name);
        };
    };
    plan.add(ShutdownStep::Database, "Error closing database", step("database"));
    plan.add(ShutdownStep::Persistence, "", step("persistence"));
    plan.add(ShutdownStep::Strands, "Error during executor shutdown", step("jobs"));
    plan.add(ShutdownStep::InterruptBackends, "", step("interrupt"));
    plan.add(ShutdownStep::Strands, "Error during executor shutdown", step("components"));
    plan.add(ShutdownStep::HotkeyRuntime, "Error during hotkey runtime cleanup", step("hotkeys"));
    plan.add(ShutdownStep::Timers, "Error stopping watchdog timers", step("timers"));
    plan.add(ShutdownStep::CancelRoot, "", step("cancel"));
    plan.add(ShutdownStep::Recorder, "Error during recorder cleanup", step("recorder"));
    plan.add(ShutdownStep::UiController, "Error during UI controller cleanup", step("ui"));
    plan.add(ShutdownStep::Backends, "Error during transcription backends cleanup",
             step("backends"));
    CHECK(plan.size() == 11);
    CHECK_FALSE(plan.started());

    plan.run();

    // Backends are interrupted before the strands are joined; the history queue drains before
    // the database closes.
    CHECK(ran == std::vector<std::string>{"cancel", "timers", "interrupt", "hotkeys", "recorder",
                                          "jobs", "components", "persistence", "backends", "ui",
                                          "database"});
    CHECK(plan.started());
}

TEST_CASE("a failing attempted step is logged as failure: error and the rest still run",
          "[shutdown_plan]") {
    Captured log;
    fv::FakeClock clock;
    ShutdownPlan plan(clock);
    std::vector<std::string> ran;
    plan.add(ShutdownStep::Recorder, "Error during recorder cleanup",
             [] { throw std::runtime_error("device lost"); });
    plan.add(ShutdownStep::Database, "Error closing database", [&] { ran.emplace_back("db"); });

    plan.run();

    CHECK(ran == std::vector<std::string>{"db"});
    std::lock_guard lock(log.sink->mutex);
    CHECK(log.sink->lines ==
          std::vector<std::string>{
              "services.application_controller|Starting application cleanup...",
              "services.application_controller|Error during recorder cleanup: device lost",
              "services.application_controller|Application controller cleaned up"});
    CHECK(log.sink->levels[1] == fv::log::Level::Warning);
}

TEST_CASE("a step without a failure label is not attempted: its error ends the run",
          "[shutdown_plan]") {
    fv::FakeClock clock;
    ShutdownPlan plan(clock);
    bool database_closed = false;
    plan.add(ShutdownStep::InterruptBackends, "", [] { throw std::runtime_error("stuck"); });
    plan.add(ShutdownStep::Database, "Error closing database", [&] { database_closed = true; });
    CHECK_THROWS_AS(plan.run(), std::runtime_error);
    CHECK_FALSE(database_closed);
}

TEST_CASE("the run records shutdown_s and runs only once", "[shutdown_plan]") {
    fv::FakeClock clock;
    ShutdownPlan plan(clock);
    int runs = 0;
    plan.add(ShutdownStep::Persistence, "", [&] {
        ++runs;
        clock.advance(std::chrono::milliseconds(1500));
    });
    std::vector<std::pair<std::string, double>> metrics;
    fv::metrics::set_sink([&](std::string_view name, double value) {
        metrics.emplace_back(std::string(name), value);
    });
    plan.run();
    plan.run();
    fv::metrics::set_sink({});
    CHECK(runs == 1);
    REQUIRE(metrics.size() == 1);
    CHECK(metrics[0].first == "shutdown_s");
    CHECK(metrics[0].second == 1.5);
}
