// Closing a WebSocket whose peer stopped reading, and sending on one whose reactor has stopped.
// Every server binds 127.0.0.1 on port 0.

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "net/http_server.h"
#include "net/reactor.h"
#include "net/testing/ws_test_client.h"
#include "net/ws_client.h"
#include "net_test_support.h"
#include "tls_test_support.h"

using namespace fv;
using namespace fv::net;
using namespace std::chrono_literals;
namespace t = fv::net::testing;

namespace {

// The server's side: its connections and their close codes.
struct Seen {
    std::mutex mutex;
    std::condition_variable changed;
    std::vector<std::shared_ptr<WsConnection>> opened;
    std::vector<std::uint16_t> closes;
    int texts = 0;
    // Set to read on: every message is then released as it arrives.
    std::atomic<bool> draining{false};

    template <class Done> bool wait(Done done, std::chrono::milliseconds timeout) {
        std::unique_lock lock(mutex);
        return changed.wait_for(lock, timeout, done);
    }
};

// Delivers each message and never releases it: with max_queued_messages = 1 the server stops
// reading after the first, so the client's later frames wait behind a write that never ends.
WsHandlers holding_handlers(const std::shared_ptr<Seen>& seen) {
    WsHandlers handlers;
    handlers.permessage_deflate = false;
    handlers.max_queued_messages = 1;
    handlers.on_open = [seen](std::shared_ptr<WsConnection> connection, const HttpRequest&) {
        {
            std::lock_guard lock(seen->mutex);
            seen->opened.push_back(std::move(connection));
        }
        seen->changed.notify_all();
    };
    handlers.on_text = [seen](const std::shared_ptr<WsConnection>& connection, std::string_view) {
        {
            std::lock_guard lock(seen->mutex);
            ++seen->texts;
        }
        seen->changed.notify_all();
        if (seen->draining.load()) {
            connection->release_message();
        }
    };
    handlers.on_close = [seen](const std::shared_ptr<WsConnection>&, std::uint16_t code,
                               std::string_view) {
        {
            std::lock_guard lock(seen->mutex);
            seen->closes.push_back(code);
        }
        seen->changed.notify_all();
    };
    return handlers;
}

// A client connected to a server that has stopped reading, with 64 MB queued behind the write
// in flight; its close_timeout is 500 ms.
struct StalledPeer {
    Reactor reactor;
    std::shared_ptr<HttpServer> server = HttpServer::create(reactor);
    std::shared_ptr<Seen> seen = std::make_shared<Seen>();
    std::shared_ptr<t::ClientEvents> events = std::make_shared<t::ClientEvents>();
    std::unique_ptr<WsClientConnection> client;

    StalledPeer() {
        server->websocket("/ws", holding_handlers(seen));
        const auto endpoint = t::listen_loopback(*server);
        auto options =
            t::recording_options(events, "ws://127.0.0.1:" + std::to_string(endpoint.port) + "/ws");
        options.close_timeout = 500ms;
        client = ws_connect(reactor, std::move(options));
        REQUIRE(events->wait_open());
        client->send_text(std::make_shared<const std::string>("hold"));
        REQUIRE(seen->wait([this] { return seen->texts == 1; }, 10s));
        const auto megabyte = std::make_shared<const std::string>(std::size_t{1} << 20, 'm');
        for (int i = 0; i < 64; ++i) {
            client->send_text(megabyte);
        }
        // Long enough for the first megabyte's write to fill both sockets' buffers and stall.
        std::this_thread::sleep_for(300ms);
        REQUIRE(client->queued_bytes() > 0);
    }
    ~StalledPeer() { server->stop(0ms); }

    // Lets the server read on until the connection ends, and gives the code it saw: 1005 (no
    // close frame) when the client dropped the connection, 1000 when the client's frames all went
    // out after all and its close frame followed them.
    std::optional<std::uint16_t> drain() {
        std::shared_ptr<WsConnection> connection;
        {
            std::lock_guard lock(seen->mutex);
            connection = seen->opened.at(0);
        }
        seen->draining = true;
        connection->release_message();
        if (!seen->wait([this] { return !seen->closes.empty(); }, 10s)) {
            return std::nullopt;
        }
        std::lock_guard lock(seen->mutex);
        return seen->closes.at(0);
    }
};

} // namespace

// websockets' close() waits close_timeout for the frames before the close frame too, then fails
// the connection (1006); without that bound the stalled write held the connection open.
TEST_CASE("sweep B4: closing a client whose peer stopped reading ends after close_timeout",
          "[platform]") {
    StalledPeer f;
    const auto started = std::chrono::steady_clock::now();
    f.client->close(1000, {});

    REQUIRE(f.events->wait_closed(5s));
    CHECK(std::chrono::steady_clock::now() - started < 4s);
    CHECK(f.events->closed->first == 1006);
    CHECK(f.client->close_code() == std::optional<std::uint16_t>(1006));
    CHECK(f.client->queued_bytes() == 0);
    // The server finds the connection dropped, not closed after the frames, once it reads again.
    CHECK(f.drain() == std::optional<std::uint16_t>(1005));
}

// Dropping the handle closes the same way: the server is not left holding the connection.
TEST_CASE("sweep B4: a dropped client whose peer stopped reading still ends its connection",
          "[platform]") {
    StalledPeer f;
    f.client.reset();
    std::this_thread::sleep_for(1500ms); // past the 500 ms close_timeout

    CHECK(f.drain() == std::optional<std::uint16_t>(1005));
    CHECK_FALSE(f.events->wait_closed(1ms)); // its callbacks went with the handle
}

// A send after the reactor stopped is dropped at once: its size is not left counted and a
// timed send's done runs with false.
TEST_CASE("sweep B6: a server send on a stopped reactor counts nothing and reports done",
          "[platform]") {
    Reactor reactor;
    auto server = HttpServer::create(reactor);
    auto seen = std::make_shared<Seen>();
    server->websocket("/ws", holding_handlers(seen));
    const auto endpoint = t::listen_loopback(*server);
    t::WsTestClient client;
    REQUIRE(client.connect(endpoint.port, "/ws"));
    REQUIRE(seen->wait([&seen] { return !seen->opened.empty(); }, 10s));
    std::shared_ptr<WsConnection> connection;
    {
        std::lock_guard lock(seen->mutex);
        connection = seen->opened.at(0);
    }
    server->stop(0ms);
    reactor.stop(0ms);

    std::optional<bool> result;
    connection->send_text(std::make_shared<const std::string>(1000, 'x'), 1s,
                          [&result](bool timed_out) { result = timed_out; });
    connection->send_binary(std::vector<std::byte>(2000, std::byte{1}));

    CHECK(result == std::optional<bool>(false));
    CHECK(connection->queued_bytes() == 0);
}
