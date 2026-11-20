// Sweep B2: destroying InstalledAgents while a known_agent lookup runs waits for the probe it is
// running, not for every probe the lookup had left (up to 6 s per candidate plus 10 s for the
// sign-in). Scripted probes only; nothing is started.

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <memory>
#include <thread>

#include "agents/installed_agents.h"
#include "agents_test_support.h"

using namespace fv;
using namespace fv::agents;
using namespace fv::agents_test;
using namespace std::chrono_literals;

TEST_CASE("destroying the agents during a lookup waits for one probe only",
          "[sweep][installed_agents]") {
    AgentsFixture fx;
    // Two installs: the first answers once its gate opens, the second never answers and would
    // hold the lookup for the whole version timeout.
    const auto first = make_program(exe(fx.path_dir, "claude"));
    const auto second = make_program(exe(fx.home / ".local" / "bin", "claude"));
    auto first_gate = std::make_shared<Gate>();
    fx.launcher->reply(first, {"--version"}, {.out = "2.1.0\n", .gate = first_gate});
    fx.launcher->reply(second, {"--version"}, {.code = std::nullopt});
    fx.launcher->reply(first, {"auth", "status"}, {.code = std::nullopt});

    auto agents = fx.make();
    CHECK_FALSE(agents->known_agent(CLAUDE_CODE).first);
    // Let the lookup reach its first probe.
    const auto until = std::chrono::steady_clock::now() + 5s;
    while (fx.launcher->count(first, {"--version"}) == 0 &&
           std::chrono::steady_clock::now() < until) {
        std::this_thread::sleep_for(5ms);
    }
    REQUIRE(fx.launcher->count(first, {"--version"}) == 1);

    std::thread opener([first_gate] {
        std::this_thread::sleep_for(200ms);
        first_gate->open();
    });
    const auto started = std::chrono::steady_clock::now();
    agents.reset();
    const auto took = std::chrono::steady_clock::now() - started;
    opener.join();
    CHECK(took < 3s);
    // No other probe started.
    CHECK(fx.launcher->count(second, {"--version"}) == 0);
    CHECK(fx.launcher->count(first, {"auth", "status"}) == 0);
}
