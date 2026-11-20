// HttpServer::listen on loopback: exclusive binding, several listeners, and what a failure
// leaves behind (agent_mcp/runtime.py _bind_sockets).

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <future>
#include <string>
#include <thread>

#include "net/http_server.h"
#include "net/reactor.h"
#include "net_test_support.h"

using namespace fv;
using namespace fv::net;
using namespace std::chrono_literals;
namespace t = fv::net::testing;

namespace {

void answer_ok(HttpServer& server) {
    server.route("GET", "/",
                 [](const HttpRequest&, std::shared_ptr<Responder> r) { r->send(200, {}, "ok"); });
}

bool serves(std::uint16_t port) {
    const auto response = t::loopback_client()->send(t::request("GET", t::url(port, "/")), {});
    return response && response->status == 200 && response->body == "ok";
}

} // namespace

TEST_CASE("listen on port 0 reports the bound port", "[platform]") {
    Reactor reactor;
    auto server = HttpServer::create(reactor);
    answer_ok(*server);
    const auto endpoint = server->listen(Listen{.address = "127.0.0.1", .port = 0});
    REQUIRE(endpoint);
    CHECK(endpoint->address == "127.0.0.1");
    CHECK(endpoint->port != 0);
    CHECK(serves(endpoint->port));
    server->stop(0ms);
}

TEST_CASE("a second exclusive listen on a bound port fails with an error string", "[platform]") {
    Reactor reactor;
    auto first = HttpServer::create(reactor);
    auto second = HttpServer::create(reactor);
    const auto endpoint = t::listen_loopback(*first);

    const auto taken = second->listen(Listen{.address = "127.0.0.1", .port = endpoint.port});
    REQUIRE_FALSE(taken);
    CHECK(taken.error().starts_with(
        "Could not listen on 127.0.0.1:" + std::to_string(endpoint.port) + ": "));
    CHECK(taken.error().size() > std::string("Could not listen on 127.0.0.1:1: ").size());
    first->stop(0ms);
    second->stop(0ms);
}

TEST_CASE("one server serves two listeners", "[platform]") {
    Reactor reactor;
    auto server = HttpServer::create(reactor);
    answer_ok(*server);
    const auto a = t::listen_loopback(*server);
    const auto b = t::listen_loopback(*server);
    CHECK(a.port != b.port);
    CHECK(serves(a.port));
    CHECK(serves(b.port));
    server->stop(0ms);
}

// The server's half of
// tests/test_agent_mcp.py::test_tailscale_bind_failure_releases_loopback_socket: TEST-NET-1 is on
// no interface, so the second bind fails as it does when the Tailscale address vanishes between
// status and listen, and the loopback listener must not stay behind.
TEST_CASE("a failed listen releases the listeners bound before it", "[platform]") {
    Reactor reactor;
    auto server = HttpServer::create(reactor);
    const auto loopback = t::listen_loopback(*server);
    const auto tailnet = server->listen(Listen{.address = "192.0.2.1", .port = loopback.port});
    REQUIRE_FALSE(tailnet);
    CHECK(tailnet.error().starts_with("Could not listen on 192.0.2.1:"));

    auto again = HttpServer::create(reactor);
    CHECK(again->listen(Listen{.address = "127.0.0.1", .port = loopback.port}).has_value());

    // And a port another server holds fails the same way.
    auto taken = HttpServer::create(reactor);
    REQUIRE_FALSE(taken->listen(Listen{.address = "127.0.0.1", .port = loopback.port}));
    again->stop(0ms);
    server->stop(0ms);
    taken->stop(0ms);
}

TEST_CASE("stop releases the port for another server", "[platform]") {
    Reactor reactor;
    auto server = HttpServer::create(reactor);
    answer_ok(*server);
    const auto endpoint = t::listen_loopback(*server);
    REQUIRE(serves(endpoint.port));
    server->stop(1s);

    auto next = HttpServer::create(reactor);
    answer_ok(*next);
    REQUIRE(next->listen(Listen{.address = "127.0.0.1", .port = endpoint.port}).has_value());
    CHECK(serves(endpoint.port));
    next->stop(0ms);
}

TEST_CASE("listen refuses what is not an IP address, and a stopped server", "[platform]") {
    Reactor reactor;
    auto server = HttpServer::create(reactor);
    const auto named = server->listen(Listen{.address = "localhost", .port = 0});
    REQUIRE_FALSE(named);
    CHECK(named.error().starts_with("Could not listen on localhost:0: "));
    server->stop(0ms);
    CHECK_FALSE(server->listen(Listen{.address = "127.0.0.1", .port = 0}).has_value());
}

TEST_CASE("a non-exclusive listener serves too", "[platform]") {
    Reactor reactor;
    auto server = HttpServer::create(reactor);
    answer_ok(*server);
    const auto endpoint =
        server->listen(Listen{.address = "127.0.0.1", .port = 0, .exclusive = false});
    REQUIRE(endpoint);
    CHECK(serves(endpoint->port));
    server->stop(0ms);
}

TEST_CASE("the reactor runs posted tasks and timers on its own threads", "[platform]") {
    Reactor reactor;
    std::promise<bool> ran;
    reactor.executor().post([&] { ran.set_value(reactor.executor().is_current()); });
    CHECK(ran.get_future().get());
    CHECK_FALSE(reactor.executor().is_current());

    std::promise<void> fired;
    auto timer = reactor.executor().post_after(20ms, [&] { fired.set_value(); });
    CHECK(fired.get_future().wait_for(5s) == std::future_status::ready);

    std::atomic<bool> cancelled_ran{false};
    auto cancelled = reactor.executor().post_after(50ms, [&] { cancelled_ran = true; });
    cancelled.cancel();
    std::this_thread::sleep_for(150ms);
    CHECK_FALSE(cancelled_ran);
    reactor.stop(1s);
    reactor.executor().post([&] { cancelled_ran = true; }); // dropped after stop
    std::this_thread::sleep_for(50ms);
    CHECK_FALSE(cancelled_ran);
}

TEST_CASE("a timer handle may outlive its reactor", "[platform]") {
    std::atomic<bool> ran{false};
    TimerHandle pending;
    TimerHandle kept;
    {
        Reactor reactor;
        pending = reactor.executor().post_after(1h, [&] { ran = true; });
        kept = reactor.executor().post_after(1h, [&] { ran = true; });
        CHECK(pending.active());
    }
    // The reactor, its io_context and the wait are gone: cancelling and dropping the handles
    // touch none of them.
    pending.cancel();
    CHECK_FALSE(pending.active());
    kept = TimerHandle();
    CHECK_FALSE(ran);
}
