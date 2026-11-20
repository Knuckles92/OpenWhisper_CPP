// "Starting application cleanup..." is logged once at quit, as OpenWhisper's cleanup() logs it
// once: by the shutdown plan's run, not again by the controller's first step.
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "appcore/shutdown_plan.h"
#include "base/log.h"
#include "controller_support.h"

namespace {

struct Lines final : fv::log::Sink {
    void write(const fv::log::Record& record) override {
        std::lock_guard lock(mutex);
        messages.push_back(std::string(record.logger) + "|" + std::string(record.message));
    }
    std::mutex mutex;
    std::vector<std::string> messages;
};

} // namespace

TEST_CASE("quitting logs the start of application cleanup once", "[widget]") {
    auto lines = std::make_shared<Lines>();
    {
        fvtest::ControllerRig rig;
        fv::log::add_sink(lines);
        rig.plan().run();
    }
    fv::log::remove_sink(lines);
    std::lock_guard lock(lines->mutex);
    CHECK(std::ranges::count(lines->messages,
                             std::string("services.application_controller|Starting application "
                                         "cleanup...")) == 1);
    CHECK(std::ranges::count(lines->messages,
                             std::string("services.application_controller|Application controller "
                                         "cleaned up")) == 1);
}
