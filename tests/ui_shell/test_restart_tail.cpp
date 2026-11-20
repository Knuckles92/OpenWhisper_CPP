// main()'s tail (main.py:398-416): a run that opened the app and ended with the restart exit code
// releases the launch gate, starts the app again and exits 0, or says why it could not and exits
// 1; anything else passes through. The start goes through a recording backup::ProcessStarter, so
// nothing is launched.
#include <catch2/catch_test_macros.hpp>

#include <expected>
#include <filesystem>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "backup/restart.h"
#include "base/identity.h"
#include "ui_shell/restart_tail.h"

namespace {

// What happened, in order: "release", "start <exe> <args...>", "box <message>".
struct Rig {
    std::vector<std::string> events;
    std::string failure; // a start fails with this sentence when not empty
    bool throws = false;

    struct Starter final : fv::backup::ProcessStarter {
        explicit Starter(Rig& rig) : rig(rig) {}
        std::expected<void, std::string> start(const std::filesystem::path& exe,
                                               std::span<const std::string> args) override {
            std::string event = "start " + exe.generic_string();
            for (const std::string& arg : args) {
                event += " " + arg;
            }
            rig.events.push_back(event);
            if (rig.throws) {
                throw std::runtime_error("The process table is full.");
            }
            if (!rig.failure.empty()) {
                return std::unexpected(rig.failure);
            }
            return {};
        }
        Rig& rig;
    };

    int finish(int code, bool opensApp) {
        Starter starter(*this);
        return fv::finishRun(
            code, opensApp, [this] { events.push_back("release"); },
            std::filesystem::path("/apps/fv/fiorivoce"), starter,
            [this](const std::string& message) { events.push_back("box " + message); });
    }
};

} // namespace

TEST_CASE("a restart exit code starts the app again alone and exits 0", "[unit]") {
    Rig rig;
    CHECK(rig.finish(fv::backup::restart_exit_code, true) == 0);
    // The gate's mutexes go first: the fresh process takes them without waiting.
    CHECK(rig.events == std::vector<std::string>{"release", "start /apps/fv/fiorivoce"});
}

TEST_CASE("a restart that cannot start says so and exits 1", "[unit]") {
    Rig rig;
    rig.failure = "Access is denied.";
    CHECK(rig.finish(fv::backup::restart_exit_code, true) == 1);
    const std::string product(fv::identity().product);
    CHECK(rig.events == std::vector<std::string>{"release", "start /apps/fv/fiorivoce",
                                                 "box Could not restart " + product +
                                                     ": Access is denied. Please start " + product +
                                                     " again manually."});
}

TEST_CASE("a restart whose start throws says so and exits 1", "[unit]") {
    Rig rig;
    rig.throws = true;
    CHECK(rig.finish(fv::backup::restart_exit_code, true) == 1);
    REQUIRE(rig.events.size() == 3);
    const std::string product(fv::identity().product);
    CHECK(rig.events[2] == "box Could not restart " + product +
                               ": The process table is full. Please start " + product +
                               " again manually.");
}

TEST_CASE("any other exit code passes through without a restart", "[unit]") {
    for (const int code : {0, 1, 2, 72, 74}) {
        Rig rig;
        CHECK(rig.finish(code, true) == code);
        CHECK(rig.events.empty());
    }
    // An early command (--diagnostics and the like) never restarts, whatever it returned.
    Rig early;
    CHECK(early.finish(fv::backup::restart_exit_code, false) == fv::backup::restart_exit_code);
    CHECK(early.events.empty());
}
