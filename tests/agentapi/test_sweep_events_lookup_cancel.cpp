// MCP Events looks a peer's callback host up with getaddrinfo, which has no timeout: the lookup
// system_network makes ends when the token it was given is cancelled (the app's root token,
// which Quit cancels before it waits for the request threads), not when the name server answers.
#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <condition_variable>
#include <future>
#include <memory>
#include <mutex>
#include <string>

#include "agentapi/mcp_events.h"
#include "base/cancel.h"
#include "net/testing/fake_lookup.h"

using namespace std::chrono_literals;

TEST_CASE("sweep: the events lookup of a peer's host ends when its token is cancelled") {
    struct Gate {
        std::mutex mutex;
        std::condition_variable changed;
        bool entered = false;
        bool open = false;
    };
    auto gate = std::make_shared<Gate>();
    fv::net::testing::ScopedLookup silent(
        [gate](const std::string&) -> std::expected<std::vector<fv::net::IpAddress>, std::string> {
            std::unique_lock lock(gate->mutex);
            gate->entered = true;
            gate->changed.notify_all();
            gate->changed.wait(lock, [&] { return gate->open; });
            return std::unexpected(std::string("no answer"));
        });
    fv::CancelSource quit;
    const fv::agentapi::events_ext::Network network =
        fv::agentapi::events_ext::system_network(quit.token());
    auto destination = std::async(std::launch::async, [&network] {
        return fv::agentapi::events_ext::destination("https://hooks.example.com/in", network);
    });
    {
        std::unique_lock lock(gate->mutex);
        REQUIRE(gate->changed.wait_for(lock, 5s, [&] { return gate->entered; }));
    }
    quit.cancel();
    const bool returned = destination.wait_for(2s) == std::future_status::ready;
    {
        std::lock_guard lock(gate->mutex);
        gate->open = true;
    }
    gate->changed.notify_all();
    REQUIRE(returned);
    CHECK(destination.get().destination == fv::agentapi::events_ext::Destination::Unresolved);
}
