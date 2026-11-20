// WebSocket routes of HttpServer, as uvicorn serves Starlette's WebSocket to meeting/web/ws.py:
// the upgrade on the HTTP port, frames both ways, close codes the browser reports, one buffer
// for many sockets, the timed send the hub's stall detection uses, the message cap, and stop().
// Every client is net::testing::WsTestClient on 127.0.0.1.

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "net/detail/server_connection.h"
#include "net/detail/ws_keepalive.h"
#include "net/http_server.h"
#include "net/lan_address.h"
#include "net/reactor.h"
#include "net/testing/ws_test_client.h"
#include "net_test_support.h"

using namespace fv;
using namespace fv::net;
using namespace std::chrono_literals;
namespace t = fv::net::testing;
using t::WsTestClient;

namespace {

// What the server side saw, in order, with a way to wait for it.
struct Seen {
    std::mutex mutex;
    std::condition_variable changed;
    std::vector<std::shared_ptr<WsConnection>> opened;
    std::vector<HttpRequest> requests;
    std::vector<std::string> events; // "open", "text:..", "binary:<n>", "close:<code>:<reason>"
    std::vector<std::pair<std::uint16_t, std::string>> closes;

    void add(std::string event) {
        std::lock_guard lock(mutex);
        events.push_back(std::move(event));
        changed.notify_all();
    }
    bool wait(const std::function<bool()>& done, std::chrono::milliseconds timeout = 10s) {
        std::unique_lock lock(mutex);
        return changed.wait_for(lock, timeout, done);
    }
    bool wait_opened(std::size_t count) {
        return wait([&] { return opened.size() >= count; });
    }
    bool wait_closes(std::size_t count, std::chrono::milliseconds timeout = 10s) {
        return wait([&] { return closes.size() >= count; }, timeout);
    }
    std::shared_ptr<WsConnection> connection(std::size_t index) {
        std::lock_guard lock(mutex);
        return opened.at(index);
    }
    std::vector<std::string> snapshot() {
        std::lock_guard lock(mutex);
        return events;
    }
};

WsHandlers recording_handlers(const std::shared_ptr<Seen>& seen, bool echo) {
    WsHandlers handlers;
    handlers.on_open = [seen](std::shared_ptr<WsConnection> connection,
                              const HttpRequest& request) {
        {
            std::lock_guard lock(seen->mutex);
            seen->opened.push_back(std::move(connection));
            seen->requests.push_back(request);
        }
        seen->add("open");
    };
    handlers.on_text = [seen, echo](const std::shared_ptr<WsConnection>& connection,
                                    std::string_view text) {
        seen->add("text:" + std::string(text));
        if (echo) {
            connection->send_text(std::make_shared<const std::string>("echo:" + std::string(text)));
        }
    };
    handlers.on_binary = [seen, echo](const std::shared_ptr<WsConnection>& connection,
                                      std::span<const std::byte> data) {
        seen->add("binary:" + std::to_string(data.size()));
        if (echo) {
            connection->send_binary(std::vector<std::byte>(data.begin(), data.end()));
        }
    };
    handlers.on_close = [seen](const std::shared_ptr<WsConnection>&, std::uint16_t code,
                               std::string_view reason) {
        {
            std::lock_guard lock(seen->mutex);
            seen->closes.emplace_back(code, std::string(reason));
        }
        seen->add("close:" + std::to_string(code) + ":" + std::string(reason));
    };
    return handlers;
}

struct WsFixture {
    Reactor reactor;
    std::unique_ptr<HttpServer> server = HttpServer::create(reactor);
    std::shared_ptr<Seen> seen = std::make_shared<Seen>();
    Endpoint endpoint;

    explicit WsFixture(std::size_t max_message_bytes = 1 << 20, bool echo = true) {
        WsHandlers handlers = recording_handlers(seen, echo);
        handlers.max_message_bytes = max_message_bytes;
        server->websocket("/ws", std::move(handlers));
        server->route("GET", "/api/ping", [](const HttpRequest&, std::shared_ptr<Responder> r) {
            r->send(200, {{"content-type", "text/plain"}}, "pong");
        });
        endpoint = t::listen_loopback(*server);
    }
    ~WsFixture() { server->stop(0ms); }

    std::uint16_t port() const { return endpoint.port; }
};

std::shared_ptr<const std::string> text_of(std::size_t size, char fill = 'x') {
    return std::make_shared<const std::string>(size, fill);
}

// Results of timed sends, by the order they were made.
struct SendLog {
    std::mutex mutex;
    std::condition_variable changed;
    std::vector<std::optional<bool>> results;

    std::function<void(bool)> slot() {
        std::lock_guard lock(mutex);
        const std::size_t index = results.size();
        results.emplace_back();
        return [this, index](bool timed_out) {
            std::lock_guard inner(mutex);
            results[index] = timed_out;
            changed.notify_all();
        };
    }
    bool wait(const std::function<bool(const std::vector<std::optional<bool>>&)>& done,
              std::chrono::milliseconds timeout) {
        std::unique_lock lock(mutex);
        return changed.wait_for(lock, timeout, [&] { return done(results); });
    }
    std::vector<std::optional<bool>> snapshot() {
        std::lock_guard lock(mutex);
        return results;
    }
};

// Polls `done` until it holds or `timeout` passes: for state the server updates on its own
// threads just after the client saw the result.
bool eventually(const std::function<bool()>& done, std::chrono::milliseconds timeout = 5s) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (!done()) {
        if (std::chrono::steady_clock::now() > deadline) {
            return false;
        }
        std::this_thread::sleep_for(10ms);
    }
    return true;
}

// Shortens uvicorn's 20 s keepalive for the connections a test opens, and puts it back.
struct ShortKeepalive {
    explicit ShortKeepalive(fv::net::detail::WsKeepalive keepalive) {
        fv::net::detail::set_ws_keepalive(keepalive);
    }
    ~ShortKeepalive() { fv::net::detail::set_ws_keepalive({}); }
    ShortKeepalive(const ShortKeepalive&) = delete;
    ShortKeepalive& operator=(const ShortKeepalive&) = delete;
};

bool any_timed_out(const std::vector<std::optional<bool>>& results) {
    for (const auto& result : results) {
        if (result == true) {
            return true;
        }
    }
    return false;
}

} // namespace

TEST_CASE("the upgrade is answered on the HTTP port with the query and headers visible",
          "[platform]") {
    WsFixture f;
    WsTestClient client;
    WsTestClient::Options options;
    options.headers = {{"Origin", "http://127.0.0.1:" + std::to_string(f.port())}};
    REQUIRE(client.connect(f.port(), "/ws?token=abc&name=Ann%20B&guest_id=g-1", options));
    REQUIRE(f.seen->wait_opened(1));

    const auto connection = f.seen->connection(0);
    const HttpRequest& request = connection->request();
    CHECK(request.method == "GET");
    CHECK(request.path == "/ws");
    CHECK(request.query == Query{{"token", "abc"}, {"name", "Ann B"}, {"guest_id", "g-1"}});
    CHECK(header(request.headers, "origin") == "http://127.0.0.1:" + std::to_string(f.port()));
    CHECK(request.peer.address == "127.0.0.1");
    CHECK(connection->id() != 0);
    {
        std::lock_guard lock(f.seen->mutex);
        CHECK(f.seen->requests.at(0).query == request.query);
    }

    // The same port still answers HTTP.
    const auto answer =
        t::loopback_client()->send(t::request("GET", t::url(f.port(), "/api/ping")), {});
    REQUIRE(answer);
    CHECK(answer->status == 200);
    CHECK(answer->body == "pong");
}

TEST_CASE("text and binary frames reach their handlers in order", "[platform]") {
    WsFixture f;
    WsTestClient client;
    REQUIRE(client.connect(f.port(), "/ws"));
    REQUIRE(f.seen->wait_opened(1));

    client.send_text("hello");
    const std::vector<std::byte> bytes{std::byte{1}, std::byte{0}, std::byte{255}};
    client.send_binary(bytes);
    client.send_text("caf\xc3\xa9");

    const auto first = client.receive(5s);
    REQUIRE(first);
    CHECK_FALSE(first->binary);
    CHECK(first->data == "echo:hello");
    const auto second = client.receive(5s);
    REQUIRE(second);
    CHECK(second->binary);
    CHECK(second->data == std::string("\x01\x00\xff", 3));
    const auto third = client.receive(5s);
    REQUIRE(third);
    CHECK(third->data == "echo:caf\xc3\xa9");

    CHECK(f.seen->snapshot() ==
          std::vector<std::string>{"open", "text:hello", "binary:3", "text:caf\xc3\xa9"});
}

TEST_CASE("a close with an application code reaches the client with that code", "[platform]") {
    WsFixture f;
    WsTestClient client;
    REQUIRE(client.connect(f.port(), "/ws?token=bad"));
    REQUIRE(f.seen->wait_opened(1));

    // The hub accepts first and checks afterwards, then closes 4401.
    f.seen->connection(0)->close(4401, "");

    REQUIRE(client.wait_closed(5s));
    CHECK(client.close_code() == std::uint16_t{4401});
    REQUIRE(f.seen->wait_closes(1));
    CHECK(f.seen->closes.at(0) == std::pair<std::uint16_t, std::string>{4401, ""});
}

TEST_CASE("frames queued before close go out first, and frames after it are dropped",
          "[platform]") {
    WsFixture f;
    WsTestClient client;
    REQUIRE(client.connect(f.port(), "/ws"));
    REQUIRE(f.seen->wait_opened(1));
    const auto connection = f.seen->connection(0);
    SendLog log;

    connection->send_text(std::make_shared<const std::string>("before"));
    connection->close(1000, "done");
    connection->send_text(std::make_shared<const std::string>("after"), 1s, log.slot());

    const auto message = client.receive(5s);
    REQUIRE(message);
    CHECK(message->data == "before");
    CHECK_FALSE(client.receive(5s).has_value());
    CHECK(client.close_code() == std::uint16_t{1000});
    CHECK(client.close_reason() == "done");
    REQUIRE(log.wait([](const auto& r) { return r[0].has_value(); }, 5s));
    CHECK(log.snapshot()[0] == false); // dropped, not timed out
}

TEST_CASE("the peer's close code and reason reach on_close", "[platform]") {
    WsFixture f;
    WsTestClient client;
    REQUIRE(client.connect(f.port(), "/ws"));
    REQUIRE(f.seen->wait_opened(1));

    client.close(1001, "tab closed");

    REQUIRE(f.seen->wait_closes(1));
    CHECK(f.seen->closes.at(0) == std::pair<std::uint16_t, std::string>{1001, "tab closed"});
}

TEST_CASE("a connection lost without a close frame reports 1005 once", "[platform]") {
    WsFixture f;
    {
        WsTestClient client;
        REQUIRE(client.connect(f.port(), "/ws"));
        REQUIRE(f.seen->wait_opened(1));
        client.abort();
    }
    REQUIRE(f.seen->wait_closes(1));
    std::this_thread::sleep_for(100ms);
    CHECK(f.seen->closes == std::vector<std::pair<std::uint16_t, std::string>>{{1005, ""}});
}

TEST_CASE("one buffer goes to 32 connections", "[platform]") {
    WsFixture f;
    std::vector<std::unique_ptr<WsTestClient>> clients;
    for (int i = 0; i < 32; ++i) {
        clients.push_back(std::make_unique<WsTestClient>());
        REQUIRE(clients.back()->connect(f.port(), "/ws"));
    }
    REQUIRE(f.seen->wait_opened(32));

    const auto shared = std::make_shared<const std::string>(R"({"type":"segment","text":"hi"})");
    for (std::size_t i = 0; i < 32; ++i) {
        f.seen->connection(i)->send_text(shared);
    }

    for (auto& client : clients) {
        const auto message = client->receive(5s);
        REQUIRE(message);
        CHECK(message->data == *shared);
    }
    for (std::size_t i = 0; i < 32; ++i) {
        const auto connection = f.seen->connection(i);
        CHECK(eventually([&] { return connection->queued_bytes() == 0; }));
    }
}

TEST_CASE("a send to a client that stopped reading reports timed_out after the timeout",
          "[platform]") {
    WsFixture f;
    WsTestClient client;
    WsTestClient::Options options;
    options.receive_buffer_bytes = 4096;
    REQUIRE(client.connect(f.port(), "/ws", options));
    REQUIRE(f.seen->wait_opened(1));
    const auto connection = f.seen->connection(0);

    // The client never reads again: the socket buffers fill, then the frames wait.
    const auto megabyte = text_of(1 << 20);
    SendLog log;
    const auto started = std::chrono::steady_clock::now();
    for (int i = 0; i < 64; ++i) {
        connection->send_text(megabyte, 500ms, log.slot());
    }

    REQUIRE(log.wait(any_timed_out, 10s));
    const auto waited = std::chrono::steady_clock::now() - started;
    CHECK(waited >= 450ms);
    CHECK(waited < 5s);
    // Frames that timed out before going out were dropped, so they no longer count as queued.
    REQUIRE(log.wait(
        [](const auto& r) {
            for (const auto& result : r) {
                if (!result) {
                    return false;
                }
            }
            return true;
        },
        10s));
    CHECK(connection->queued_bytes() <= std::size_t{1} << 20);
}

TEST_CASE("a frame over max_message_bytes closes 1009", "[platform]") {
    WsFixture f(1024);
    WsTestClient client;
    REQUIRE(client.connect(f.port(), "/ws"));
    REQUIRE(f.seen->wait_opened(1));

    client.send_text(std::string(1024, 'a'));
    const auto echoed = client.receive(5s);
    REQUIRE(echoed);
    client.send_text(std::string(1025, 'a'));

    REQUIRE(client.wait_closed(5s));
    CHECK(client.close_code() == std::uint16_t{1009});
    REQUIRE(f.seen->wait_closes(1));
    CHECK(f.seen->closes.at(0).first == 1009);
}

TEST_CASE("text that is not UTF-8 closes 1007", "[platform]") {
    WsFixture f;
    WsTestClient client;
    REQUIRE(client.connect(f.port(), "/ws"));
    REQUIRE(f.seen->wait_opened(1));

    client.send_text("\xff\xfe");

    REQUIRE(client.wait_closed(5s));
    CHECK(client.close_code() == std::uint16_t{1007});
    REQUIRE(f.seen->wait_closes(1));
    CHECK(f.seen->closes.at(0).first == 1007);
}

TEST_CASE("permessage-deflate is negotiated when the client offers it", "[platform]") {
    WsFixture f;
    WsTestClient client;
    WsTestClient::Options options;
    options.deflate = true;
    REQUIRE(client.connect(f.port(), "/ws", options));
    REQUIRE(f.seen->wait_opened(1));

    const std::string text(20'000, 'z');
    client.send_text(text);
    const auto message = client.receive(5s);
    REQUIRE(message);
    CHECK(message->data == "echo:" + text);
}

TEST_CASE("a WebSocket request to a path without a route is refused with 403", "[platform]") {
    WsFixture f;
    WsTestClient client;
    CHECK_FALSE(client.connect(f.port(), "/elsewhere"));
    CHECK(client.error() == "HTTP 403");
    CHECK_FALSE(client.connect(f.port(), "/api/ping"));
    CHECK(client.error() == "HTTP 403");
    CHECK(f.seen->snapshot().empty());
}

TEST_CASE("a plain GET of the WebSocket path is FastAPI's 404", "[platform]") {
    WsFixture f;
    const auto answer = t::loopback_client()->send(t::request("GET", t::url(f.port(), "/ws")), {});
    REQUIRE(answer);
    CHECK(answer->status == 404);
    CHECK(answer->body == R"({"detail":"Not Found"})");
}

TEST_CASE("a handler that throws drops its connection and the others go on", "[platform]") {
    Reactor reactor;
    auto server = HttpServer::create(reactor);
    auto seen = std::make_shared<Seen>();
    WsHandlers handlers = recording_handlers(seen, true);
    auto inner = handlers.on_text;
    handlers.on_text = [inner](const std::shared_ptr<WsConnection>& connection,
                               std::string_view text) {
        if (text == "boom") {
            throw std::runtime_error("handler failed");
        }
        inner(connection, text);
    };
    server->websocket("/ws", std::move(handlers));
    const auto endpoint = t::listen_loopback(*server);
    t::ScopedCapture capture;

    WsTestClient failing;
    WsTestClient healthy;
    REQUIRE(failing.connect(endpoint.port, "/ws"));
    REQUIRE(healthy.connect(endpoint.port, "/ws"));
    REQUIRE(seen->wait_opened(2));

    failing.send_text("boom");
    CHECK(failing.wait_closed(5s));
    healthy.send_text("still here");
    const auto message = healthy.receive(5s);
    REQUIRE(message);
    CHECK(message->data == "echo:still here");
    bool logged = false;
    for (const auto& line : capture->lines()) {
        logged = logged || (line.logger == "uvicorn.error" &&
                            line.message == "Exception in ASGI application");
    }
    CHECK(logged);
    server->stop(0ms);
}

TEST_CASE("stop closes open WebSockets with 1012 and releases the port", "[platform]") {
    Reactor reactor;
    auto server = HttpServer::create(reactor);
    auto seen = std::make_shared<Seen>();
    server->websocket("/ws", recording_handlers(seen, true));
    const auto endpoint = t::listen_loopback(*server);

    WsTestClient first;
    WsTestClient second;
    REQUIRE(first.connect(endpoint.port, "/ws"));
    REQUIRE(second.connect(endpoint.port, "/ws"));
    REQUIRE(seen->wait_opened(2));

    // The client answers the close frame only while one of its calls runs; one waits for it.
    std::thread reader([&] { first.wait_closed(10s); });
    const auto started = std::chrono::steady_clock::now();
    server->stop(1s);
    const auto took = std::chrono::steady_clock::now() - started;
    reader.join();

    CHECK(took < 3s);
    CHECK(first.close_code() == std::uint16_t{1012});
    REQUIRE(second.wait_closed(5s));
    CHECK(
        (second.close_code() == std::uint16_t{1012} || second.close_code() == std::uint16_t{1006}));
    REQUIRE(seen->wait_closes(2));
    CHECK(seen->closes.at(0).first == 1012);
    CHECK(seen->closes.at(1).first == 1012);

    auto again = HttpServer::create(reactor);
    const auto rebound = again->listen(Listen{.address = "127.0.0.1", .port = endpoint.port});
    CHECK(rebound.has_value());
    again->stop(0ms);
}

TEST_CASE("stop does not wait past its grace for a client that stopped reading", "[platform]") {
    Reactor reactor;
    auto server = HttpServer::create(reactor);
    auto seen = std::make_shared<Seen>();
    server->websocket("/ws", recording_handlers(seen, false));
    const auto endpoint = t::listen_loopback(*server);
    WsTestClient stalled;
    WsTestClient::Options options;
    options.receive_buffer_bytes = 4096;
    REQUIRE(stalled.connect(endpoint.port, "/ws", options));
    REQUIRE(seen->wait_opened(1));
    const auto megabyte = text_of(1 << 20);
    for (int i = 0; i < 32; ++i) {
        seen->connection(0)->send_text(megabyte);
    }
    std::this_thread::sleep_for(200ms);

    const auto started = std::chrono::steady_clock::now();
    server->stop(500ms);
    const auto took = std::chrono::steady_clock::now() - started;

    CHECK(took < 2500ms);
    REQUIRE(seen->wait_closes(1));
    CHECK(seen->closes.at(0).first == 1012);
}

TEST_CASE("the frame being written is still owned when on_close runs", "[platform]") {
    Reactor reactor;
    auto server = HttpServer::create(reactor);
    auto seen = std::make_shared<Seen>();
    // Larger than anything loopback can absorb (the server's send buffer, at most tcp_wmem's 4 MB
    // here, and the client's 4 KB): the first write can never complete, so it is in flight
    // whenever stop reaches the connection. With 1 MB frames a loaded machine could see a write
    // finish just as stop arrived, leaving none in flight.
    auto payload = text_of(std::size_t{16} << 20);
    const std::weak_ptr<const std::string> watched = payload;
    // 0 until on_close runs, then 1 if the payload was still alive there, else 2.
    auto alive_at_close = std::make_shared<std::atomic<int>>(0);
    WsHandlers handlers = recording_handlers(seen, false);
    auto inner = handlers.on_close;
    handlers.on_close = [inner, watched,
                         alive_at_close](const std::shared_ptr<WsConnection>& connection,
                                         std::uint16_t code, std::string_view reason) {
        alive_at_close->store(watched.expired() ? 2 : 1);
        inner(connection, code, reason);
    };
    server->websocket("/ws", std::move(handlers));
    const auto endpoint = t::listen_loopback(*server);
    WsTestClient stalled;
    WsTestClient::Options options;
    options.receive_buffer_bytes = 4096;
    REQUIRE(stalled.connect(endpoint.port, "/ws", options));
    REQUIRE(seen->wait_opened(1));

    // The client never reads: the first frame's write stays pending, the others wait behind it,
    // and only the queue holds the string.
    for (int i = 0; i < 4; ++i) {
        seen->connection(0)->send_text(payload);
    }
    payload.reset();
    // On the connection's strand after every enqueue: server_stopping drops the frames not
    // started, then force_close finishes the connection with that write still pending.
    server->stop(0ms);

    REQUIRE(seen->wait_closes(1));
    CHECK(alive_at_close->load() == 1);
}

TEST_CASE("a closed WebSocket counts no queued bytes when stop reaches it after its last write",
          "[platform]") {
    Reactor reactor;
    auto server = HttpServer::create(reactor);
    auto seen = std::make_shared<Seen>();
    server->websocket("/ws", recording_handlers(seen, false));
    const auto endpoint = t::listen_loopback(*server);
    WsTestClient stalled;
    WsTestClient::Options options;
    options.receive_buffer_bytes = 4096;
    REQUIRE(stalled.connect(endpoint.port, "/ws", options));
    REQUIRE(seen->wait_opened(1));
    std::shared_ptr<WsConnection> connection;
    {
        std::lock_guard lock(seen->mutex);
        connection = std::move(seen->opened.at(0));
        seen->opened.clear();
    }
    std::weak_ptr<const std::string> written;
    {
        const auto payload = text_of(1 << 20);
        written = payload;
        for (int i = 0; i < 64; ++i) {
            connection->send_text(payload);
        }
    }

    // finish runs while the first write is pending: it counts every frame off and keeps the one
    // being written, which the aborted write's completion then finds on a closed connection.
    server->stop(0ms);
    REQUIRE(seen->wait_closes(1));
    // Each pending handler holds the connection, the write's completion among them: once only
    // this test does, all of them have run.
    REQUIRE(eventually([&] { return connection.use_count() == 1; }, 10s));
    CHECK(written.expired());

    // stop() posts server_stopping to the connections it listed; one that closed after the
    // listing receives it closed, after its last write completed.
    auto* as_listed = dynamic_cast<fv::net::detail::ServerConnection*>(connection.get());
    REQUIRE(as_listed != nullptr);
    as_listed->server_stopping(); // holds the connection until it has run on its strand
    REQUIRE(eventually([&] { return connection.use_count() == 1; }, 10s));
    CHECK(connection->queued_bytes() == 0);
}

TEST_CASE("a 64 KB frame fans out to 100 sockets while a stalled one times out alone",
          "[platform]") {
    WsFixture f(1 << 20, false);
    constexpr std::size_t kReaders = 100;
    std::vector<std::unique_ptr<WsTestClient>> readers;
    WsTestClient stalled;
    WsTestClient::Options small;
    small.receive_buffer_bytes = 4096;
    REQUIRE(stalled.connect(f.port(), "/ws", small));
    REQUIRE(f.seen->wait_opened(1));
    const auto stalled_connection = f.seen->connection(0);
    for (std::size_t i = 0; i < kReaders; ++i) {
        readers.push_back(std::make_unique<WsTestClient>());
        REQUIRE(readers.back()->connect(f.port(), "/ws"));
    }
    REQUIRE(f.seen->wait_opened(kReaders + 1));

    // The stalled client's socket is full before the fan-out begins.
    const auto megabyte = text_of(1 << 20, 'm');
    for (int i = 0; i < 32; ++i) {
        stalled_connection->send_text(megabyte);
    }
    std::this_thread::sleep_for(200ms);

    // The hub's broadcast: one buffer, a 5 s timeout per socket.
    const auto frame = text_of(64 * 1024, 'f');
    SendLog log;
    const auto started = std::chrono::steady_clock::now();
    stalled_connection->send_text(frame, 5s, log.slot());
    for (std::size_t i = 1; i <= kReaders; ++i) {
        f.seen->connection(i)->send_text(frame, 5s, log.slot());
    }
    for (auto& reader : readers) {
        const auto message = reader->receive(10s);
        REQUIRE(message);
        CHECK(message->data == *frame);
    }
    const auto readers_done = std::chrono::steady_clock::now() - started;

    // When the stalled socket's timeout reports, every other send has long completed.
    REQUIRE(log.wait([](const auto& r) { return r[0].has_value(); }, 15s));
    const auto results = log.snapshot();
    CHECK(results[0] == true);
    std::size_t delivered = 0;
    for (std::size_t i = 1; i < results.size(); ++i) {
        delivered += results[i] == false ? 1 : 0;
    }
    CHECK(delivered == kReaders);
    CHECK(readers_done < 5s);
}

TEST_CASE("an unanswered keepalive ping closes 1011 after the ping timeout", "[platform]") {
    // uvicorn: a ping every ws_ping_interval; no pong within ws_ping_timeout fails the
    // connection with 1011 "keepalive ping timeout". Here 150 ms each instead of 20 s.
    ShortKeepalive keepalive({.interval = 150ms, .timeout = 150ms});
    WsFixture f;
    WsTestClient client;
    REQUIRE(client.connect(f.port(), "/ws"));
    REQUIRE(f.seen->wait_opened(1));
    const auto opened = std::chrono::steady_clock::now();

    // The client reads nothing between its calls, so the ping is never answered.
    REQUIRE(f.seen->wait_closes(1, 5s));
    CHECK(std::chrono::steady_clock::now() - opened >= 250ms);
    CHECK(f.seen->closes.at(0) ==
          std::pair<std::uint16_t, std::string>{1011, "keepalive ping timeout"});
    // The client reads only now, after the server cut the socket: its pong and its close echo
    // may meet a reset before the close frame is parsed, as a dead browser would.
    REQUIRE(client.wait_closed(5s));
    const auto code = client.close_code();
    REQUIRE(code);
    CHECK((*code == 1011 || *code == 1006));
    if (*code == 1011) {
        CHECK(client.close_reason() == "keepalive ping timeout");
    }
}

TEST_CASE("a client that answers the keepalive pings stays open", "[platform]") {
    ShortKeepalive keepalive({.interval = 100ms, .timeout = 300ms});
    WsFixture f;
    WsTestClient client;
    REQUIRE(client.connect(f.port(), "/ws"));
    REQUIRE(f.seen->wait_opened(1));

    // Reading answers each ping with a pong: many intervals and timeouts pass, nothing closes.
    CHECK_FALSE(client.receive(1500ms).has_value());
    CHECK_FALSE(client.close_code().has_value());
    client.send_text("still here");
    const auto echoed = client.receive(5s);
    REQUIRE(echoed);
    CHECK(echoed->data == "echo:still here");
    CHECK(f.seen->snapshot() == std::vector<std::string>{"open", "text:still here"});
}

TEST_CASE("a server on 0.0.0.0 upgrades and answers on this computer's LAN address",
          "[interactive]") {
    // Serving on every interface opens the port to the network (the dashboard's "Share on
    // local network"): written for the owner, never run by a worker.
    Reactor reactor;
    auto server = HttpServer::create(reactor);
    auto seen = std::make_shared<Seen>();
    server->websocket("/ws", recording_handlers(seen, true));
    server->route("GET", "/api/ping", [](const HttpRequest&, std::shared_ptr<Responder> r) {
        r->send(200, {{"content-type", "text/plain"}}, "pong");
    });
    const auto endpoint = server->listen(Listen{.address = "0.0.0.0", .port = 0});
    REQUIRE(endpoint);

    WsTestClient client;
    REQUIRE(client.connect(endpoint->port, "/ws?token=t"));
    client.send_text("hi");
    const auto echoed = client.receive(5s);
    REQUIRE(echoed);
    CHECK(echoed->data == "echo:hi");

    const auto lan = discover_lan_ipv4();
    REQUIRE(lan);
    const auto answer = t::loopback_client()->send(
        t::request("GET", "http://" + *lan + ":" + std::to_string(endpoint->port) + "/api/ping"),
        {});
    REQUIRE(answer);
    CHECK(answer->status == 200);
    CHECK(answer->body == "pong");
    server->stop(0ms);
}
