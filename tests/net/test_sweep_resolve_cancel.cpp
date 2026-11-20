// A name lookup that never answers (getaddrinfo has no timeout and cannot be interrupted) does
// not hold its caller once the caller's token is cancelled: Quit cancels the root token and then
// waits for the io task that is looking the name up.
#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <condition_variable>
#include <future>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include "base/cancel.h"
#include "net/address.h"
#include "net/testing/fake_lookup.h"

using namespace std::chrono_literals;

namespace {

// A name server that answers only when the test lets it, so the lookup thread can be seen to
// finish after its caller has gone.
struct Gate {
    std::mutex mutex;
    std::condition_variable changed;
    bool entered = false;
    bool open = false;
    bool finished = false;
};

fv::net::testing::Lookup gated(const std::shared_ptr<Gate>& gate) {
    return
        [gate](const std::string&) -> std::expected<std::vector<fv::net::IpAddress>, std::string> {
            std::unique_lock lock(gate->mutex);
            gate->entered = true;
            gate->changed.notify_all();
            gate->changed.wait(lock, [&] { return gate->open; });
            gate->finished = true;
            gate->changed.notify_all();
            return std::vector<fv::net::IpAddress>{*fv::net::parse_address("203.0.113.9")};
        };
}

} // namespace

TEST_CASE("resolve_host returns once its token is cancelled while the lookup hangs") {
    auto gate = std::make_shared<Gate>();
    fv::net::testing::ScopedLookup replaced(gated(gate));
    fv::CancelSource source;
    auto lookup = std::async(std::launch::async, [token = source.token()] {
        return fv::net::resolve_host("peer.example.com", token);
    });
    {
        std::unique_lock lock(gate->mutex);
        REQUIRE(gate->changed.wait_for(lock, 5s, [&] { return gate->entered; }));
    }
    const auto cancelledAt = std::chrono::steady_clock::now();
    source.cancel();
    const bool returned = lookup.wait_for(2s) == std::future_status::ready;
    // Let the lookup thread finish either way, so a failure does not hang the test.
    {
        std::lock_guard lock(gate->mutex);
        gate->open = true;
    }
    gate->changed.notify_all();
    REQUIRE(returned);
    CHECK(std::chrono::steady_clock::now() - cancelledAt < 2s);
    const auto found = lookup.get();
    REQUIRE_FALSE(found.has_value());
    CHECK(found.error() == "cancelled");
    // The abandoned lookup still ends on its own, touching only what it owns.
    std::unique_lock lock(gate->mutex);
    CHECK(gate->changed.wait_for(lock, 5s, [&] { return gate->finished; }));
}

TEST_CASE("resolve_host gives the lookup's answer when nothing cancels it") {
    auto gate = std::make_shared<Gate>();
    gate->open = true;
    fv::net::testing::ScopedLookup replaced(gated(gate));
    const auto found = fv::net::resolve_host("peer.example.com", fv::CancelToken());
    REQUIRE(found.has_value());
    REQUIRE(found->size() == 1);
    CHECK(found->front().to_string() == "203.0.113.9");
}
