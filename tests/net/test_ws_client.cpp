// The TLS WebSocket of the remote engine on both ends: HttpServer::listen with TLS, as tls.py's
// server_context and websockets' serve(...) host it, and ws_connect, as client.py's _open opens
// it. Every server binds 127.0.0.1 on port 0.

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <atomic>
#include <cctype>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "net/detail/ws_client_session.h"
#include "net/http_server.h"
#include "net/lan_address.h"
#include "net/reactor.h"
#include "net/testing/ws_test_client.h"
#include "net/tls.h"
#include "net/ws_client.h"
#include "net_test_support.h"
#include "tls_test_support.h"

using namespace fv;
using namespace fv::net;
using namespace std::chrono_literals;
namespace t = fv::net::testing;
using Kind = WsConnectFailure::Kind;

namespace {

// What the server side saw.
struct ServerSeen {
    std::mutex mutex;
    std::condition_variable changed;
    std::vector<std::shared_ptr<WsConnection>> opened;
    std::vector<HttpRequest> requests;
    std::vector<std::string> events;
    std::vector<std::pair<std::uint16_t, std::string>> closes;

    void add(std::string event) {
        {
            std::lock_guard lock(mutex);
            events.push_back(std::move(event));
        }
        changed.notify_all();
    }
    bool wait(const std::function<bool()>& done, std::chrono::milliseconds timeout = 10s) {
        std::unique_lock lock(mutex);
        return changed.wait_for(lock, timeout, done);
    }
    std::shared_ptr<WsConnection> connection(std::size_t index) {
        std::lock_guard lock(mutex);
        return opened.at(index);
    }
    std::vector<std::string> snapshot() {
        std::lock_guard lock(mutex);
        return events;
    }
    // Called inside wait(), which holds the lock.
    std::size_t count(std::string_view prefix) const {
        std::size_t n = 0;
        for (const auto& event : events) {
            n += event.starts_with(prefix) ? 1 : 0;
        }
        return n;
    }
};

// Echoes each message back ("echo:<text>" or the same bytes) unless `echo` is off; releases
// each message at once unless `release` is off.
WsHandlers server_handlers(const std::shared_ptr<ServerSeen>& seen, bool echo = true,
                           bool release = true) {
    WsHandlers handlers;
    handlers.permessage_deflate = false;
    handlers.on_open = [seen](std::shared_ptr<WsConnection> connection,
                              const HttpRequest& request) {
        {
            std::lock_guard lock(seen->mutex);
            seen->opened.push_back(std::move(connection));
            seen->requests.push_back(request);
        }
        seen->add("open");
    };
    handlers.on_text = [seen, echo, release](const std::shared_ptr<WsConnection>& connection,
                                             std::string_view text) {
        seen->add("text:" + std::string(text));
        if (echo) {
            connection->send_text(std::make_shared<const std::string>("echo:" + std::string(text)));
        }
        if (release) {
            connection->release_message();
        }
    };
    handlers.on_binary = [seen, echo, release](const std::shared_ptr<WsConnection>& connection,
                                               std::span<const std::byte> data) {
        seen->add("binary:" + std::to_string(data.size()));
        if (echo) {
            connection->send_binary(std::vector<std::byte>(data.begin(), data.end()));
        }
        if (release) {
            connection->release_message();
        }
    };
    handlers.on_close = [seen](const std::shared_ptr<WsConnection>&, std::uint16_t code,
                               std::string_view reason) {
        {
            std::lock_guard lock(seen->mutex);
            seen->closes.emplace_back(code, std::string(reason));
        }
        seen->add("close:" + std::to_string(code));
    };
    return handlers;
}

struct TlsServer {
    Reactor reactor;
    std::unique_ptr<HttpServer> server = HttpServer::create(reactor);
    std::shared_ptr<ServerSeen> seen = std::make_shared<ServerSeen>();
    Endpoint endpoint;

    explicit TlsServer(WsHandlers handlers) {
        server->websocket("/ws", std::move(handlers));
        endpoint = t::listen_tls(*server);
    }
    TlsServer() : TlsServer(WsHandlers{}) {}
    ~TlsServer() { server->stop(0ms); }
};

std::vector<std::byte> bytes_of(std::string_view text) {
    std::vector<std::byte> out;
    for (const char c : text) {
        out.push_back(static_cast<std::byte>(c));
    }
    return out;
}

} // namespace

TEST_CASE("a TLS WebSocket server and ws_connect exchange text and binary frames", "[platform]") {
    auto seen = std::make_shared<ServerSeen>();
    TlsServer f(server_handlers(seen));
    auto events = std::make_shared<t::ClientEvents>();
    auto options = t::recording_options(events, t::wss_url(f.endpoint.port, "/ws?token=abc"));
    options.headers = {{"X-Probe", "1"}};
    auto client = ws_connect(f.reactor, std::move(options));
    REQUIRE(events->wait_open());
    CHECK(client->is_open());
    REQUIRE(seen->wait([&] { return !seen->opened.empty(); }));

    client->send_text(std::make_shared<const std::string>("hello"));
    client->send_binary(bytes_of("abc"));
    REQUIRE(events->wait_messages(2));
    CHECK(events->snapshot() == std::vector<std::string>{"text:echo:hello", "binary:3"});
    CHECK(seen->snapshot() == std::vector<std::string>{"open", "text:hello", "binary:3"});

    std::lock_guard lock(seen->mutex);
    const HttpRequest& request = seen->requests.at(0);
    CHECK(request.tls);
    CHECK(request.path == "/ws");
    REQUIRE(request.query.size() == 1);
    CHECK(request.query[0] == std::pair<std::string, std::string>{"token", "abc"});
    CHECK(header(request.headers, "x-probe") == "1");
    // websockets' client offers no extension (compression=None).
    CHECK_FALSE(header(request.headers, "sec-websocket-extensions"));

    const auto server_side = seen->opened.at(0);
    CHECK(server_side->remote_endpoint().address == "127.0.0.1");
    CHECK(server_side->local_endpoint().port == f.endpoint.port);
    CHECK(server_side->remote_endpoint().port == client->local_endpoint().port);
    CHECK(client->remote_endpoint().port == f.endpoint.port);
    CHECK(server_side->is_open());
}

TEST_CASE("the client sees the certificate the server serves", "[platform]") {
    TlsServer f;
    auto events = std::make_shared<t::ClientEvents>();
    auto client = ws_connect(f.reactor, t::recording_options(events, t::wss_url(f.endpoint.port)));
    REQUIRE(events->wait_open());
    const auto expected = certificate_sha256_from_pem(t::test_identity().cert_pem);
    CHECK(client->peer_certificate_sha256() == expected);
    CHECK(expected.size() == 64);
}

TEST_CASE("a pinned certificate that matches opens, one that differs fails as Tls", "[platform]") {
    TlsServer f;
    const auto fingerprint = certificate_sha256_from_pem(t::test_identity().cert_pem);

    SECTION("matching, in lower case") {
        auto events = std::make_shared<t::ClientEvents>();
        auto options = t::recording_options(events, t::wss_url(f.endpoint.port));
        std::string lower = fingerprint;
        for (char& c : lower) {
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        }
        options.pinned_sha256 = lower;
        auto client = ws_connect(f.reactor, std::move(options));
        CHECK(events->wait_open());
    }
    SECTION("different") {
        auto events = std::make_shared<t::ClientEvents>();
        auto options = t::recording_options(events, t::wss_url(f.endpoint.port));
        options.pinned_sha256 = std::string(64, 'A');
        auto client = ws_connect(f.reactor, std::move(options));
        REQUIRE(events->wait_failure());
        CHECK(events->failure->kind == Kind::Tls);
        CHECK(events->failure->detail == "certificate mismatch");
        CHECK_FALSE(events->opened);
        CHECK_FALSE(events->closed); // on_connect_failed instead of on_close
        CHECK(client->close_code() == std::optional<std::uint16_t>(1006));
        CHECK_FALSE(client->is_open());
    }
}

TEST_CASE("the server negotiates TLS 1.3, or TLS 1.2 with a client that stops there",
          "[platform]") {
    TlsServer f;
    const auto modern = t::tls_probe(f.endpoint.port, 0, 0);
    REQUIRE(modern.connected);
    CHECK(modern.version == TLS1_3_VERSION);
    const auto older = t::tls_probe(f.endpoint.port, TLS1_2_VERSION, TLS1_2_VERSION);
    REQUIRE(older.connected);
    CHECK(older.version == TLS1_2_VERSION);
}

TEST_CASE("a TLS 1.1 client is refused", "[platform]") {
    TlsServer f;
    const auto probe = t::tls_probe(f.endpoint.port, TLS1_1_VERSION, TLS1_1_VERSION);
    CHECK_FALSE(probe.connected);
    // The server's alert, not a client that could not offer TLS 1.1 at all.
    INFO(probe.error);
    CHECK(probe.error.find("protocol version") != std::string::npos);
}

TEST_CASE("disable_tickets sends no TLS 1.3 session ticket; OpenSSL otherwise sends them",
          "[platform]") {
    // tls.py: num_tickets = 0, because a websockets client stalled on them on Windows.
    Reactor reactor;
    auto quiet = HttpServer::create(reactor);
    auto chatty = HttpServer::create(reactor);
    TlsIdentity without = t::test_identity();
    without.disable_tickets = true;
    TlsIdentity with = t::test_identity();
    with.disable_tickets = false;
    const auto quiet_end = t::listen_tls(*quiet, without);
    const auto chatty_end = t::listen_tls(*chatty, with);

    const auto none = t::tls_probe(quiet_end.port, 0, 0);
    REQUIRE(none.connected);
    CHECK(none.version == TLS1_3_VERSION);
    CHECK(none.tickets == 0);

    const auto some = t::tls_probe(chatty_end.port, 0, 0);
    REQUIRE(some.connected);
    CHECK(some.tickets > 0);
    quiet->stop(0ms);
    chatty->stop(0ms);
}

TEST_CASE("generate_self_signed_p256 marks its identity to send no tickets", "[platform]") {
    CHECK(t::test_identity().disable_tickets);
}

TEST_CASE("a certificate and key OpenSSL cannot use fail listen", "[platform]") {
    Reactor reactor;
    auto server = HttpServer::create(reactor);
    TlsIdentity broken{.cert_pem = "not a certificate", .key_pem = t::test_identity().key_pem};
    const auto result =
        server->listen(Listen{.address = "127.0.0.1", .port = 0, .tls = std::move(broken)});
    REQUIRE_FALSE(result);
    CHECK(result.error().starts_with("Could not listen on 127.0.0.1:0: the TLS certificate"));

    TlsIdentity other_key = t::test_identity();
    other_key.key_pem = generate_self_signed_p256("x").key_pem;
    const auto mismatch =
        server->listen(Listen{.address = "127.0.0.1", .port = 0, .tls = std::move(other_key)});
    REQUIRE_FALSE(mismatch);
    server->stop(0ms);
}

TEST_CASE("a plain HTTP request over TLS is answered by the routes", "[platform]") {
    Reactor reactor;
    auto server = HttpServer::create(reactor);
    std::atomic<bool> saw_tls{false};
    server->route("GET", "/nothing", [&](const HttpRequest& request, std::shared_ptr<Responder> r) {
        saw_tls = request.tls;
        r->send(200, {{"content-type", "text/plain"}}, "ok");
    });
    const auto endpoint = t::listen_tls(*server);
    const auto probe = t::tls_probe(endpoint.port, 0, 0);
    CHECK(probe.connected);
    CHECK(saw_tls);
    server->stop(0ms);
}

TEST_CASE("connect failures map to kinds", "[platform]") {
    Reactor reactor;

    SECTION("a closed port is Refused") {
        std::uint16_t port = 0;
        {
            // A port that was just free: bound, then released.
            boost::asio::io_context io;
            boost::asio::ip::tcp::acceptor acceptor(
                io, {boost::asio::ip::make_address("127.0.0.1"), 0});
            port = acceptor.local_endpoint().port();
        }
        auto events = std::make_shared<t::ClientEvents>();
        auto client = ws_connect(reactor, t::recording_options(events, t::wss_url(port)));
        REQUIRE(events->wait_failure());
        CHECK(events->failure->kind == Kind::Refused);
    }
    SECTION("a name that cannot resolve is Dns") {
        // A label over 63 characters is refused by the resolver itself, before any query.
        const std::string name = std::string(70, 'a') + ".invalid";
        auto events = std::make_shared<t::ClientEvents>();
        auto client =
            ws_connect(reactor, t::recording_options(events, "wss://" + name + ":47821/x"));
        REQUIRE(events->wait_failure());
        CHECK(events->failure->kind == Kind::Dns);
    }
    SECTION("an HTTP answer other than 101 is HttpStatus with the status") {
        // The remote host's answer to another path (host.py _check_path).
        auto server = HttpServer::create(reactor);
        server->websocket("/openwhisper/v1/engine", WsHandlers{});
        server->set_websocket_fallback([](const HttpRequest&, std::shared_ptr<Responder> r) {
            r->send(404, {{"content-type", "text/plain"}}, "Not here\n");
        });
        const auto endpoint = t::listen_tls(*server);
        auto events = std::make_shared<t::ClientEvents>();
        auto client =
            ws_connect(reactor, t::recording_options(events, t::wss_url(endpoint.port, "/x")));
        REQUIRE(events->wait_failure());
        CHECK(events->failure->kind == Kind::HttpStatus);
        CHECK(events->failure->http_status == 404);
        server->stop(0ms);
    }
    SECTION("a server that never answers is Timeout within connect_timeout") {
        boost::asio::io_context io;
        // Listening, never accepting: the TCP connect completes from the backlog, and nothing
        // answers the TLS handshake.
        boost::asio::ip::tcp::acceptor silent(io, {boost::asio::ip::make_address("127.0.0.1"), 0});
        auto events = std::make_shared<t::ClientEvents>();
        auto options = t::recording_options(events, t::wss_url(silent.local_endpoint().port()));
        options.connect_timeout = 300ms;
        const auto started = std::chrono::steady_clock::now();
        auto client = ws_connect(reactor, std::move(options));
        REQUIRE(events->wait_failure(5s));
        CHECK(events->failure->kind == Kind::Timeout);
        CHECK(std::chrono::steady_clock::now() - started < 3s);
    }
    SECTION("a plain server on wss:// is Tls") {
        auto server = HttpServer::create(reactor);
        server->websocket("/ws", WsHandlers{});
        const auto endpoint = t::listen_loopback(*server);
        auto events = std::make_shared<t::ClientEvents>();
        auto client = ws_connect(reactor, t::recording_options(events, t::wss_url(endpoint.port)));
        REQUIRE(events->wait_failure());
        CHECK(events->failure->kind == Kind::Tls);
        server->stop(0ms);
    }
    SECTION("a URL that is not ws:// or wss:// is Handshake") {
        auto events = std::make_shared<t::ClientEvents>();
        auto client = ws_connect(reactor, t::recording_options(events, "https://127.0.0.1/x"));
        REQUIRE(events->wait_failure());
        CHECK(events->failure->kind == Kind::Handshake);
    }
    SECTION("close() before the connection opened is Cancelled") {
        boost::asio::io_context io;
        boost::asio::ip::tcp::acceptor silent(io, {boost::asio::ip::make_address("127.0.0.1"), 0});
        auto events = std::make_shared<t::ClientEvents>();
        auto client = ws_connect(
            reactor, t::recording_options(events, t::wss_url(silent.local_endpoint().port())));
        client->close(1000, {});
        REQUIRE(events->wait_failure());
        CHECK(events->failure->kind == Kind::Cancelled);
    }
}

TEST_CASE("without on_connect_failed a failed connection closes 1006 with the detail",
          "[platform]") {
    Reactor reactor;
    auto events = std::make_shared<t::ClientEvents>();
    auto options = t::recording_options(events, "ftp://127.0.0.1/");
    options.on_connect_failed = nullptr;
    auto client = ws_connect(reactor, std::move(options));
    REQUIRE(events->wait_closed());
    CHECK(events->closed->first == 1006);
    CHECK(events->closed->second == "not a ws:// or wss:// URL");
}

TEST_CASE("ws:// connects without TLS", "[platform]") {
    Reactor reactor;
    auto server = HttpServer::create(reactor);
    auto seen = std::make_shared<ServerSeen>();
    server->websocket("/ws", server_handlers(seen));
    const auto endpoint = t::listen_loopback(*server);
    auto events = std::make_shared<t::ClientEvents>();
    auto client = ws_connect(
        reactor,
        t::recording_options(events, "ws://127.0.0.1:" + std::to_string(endpoint.port) + "/ws"));
    REQUIRE(events->wait_open());
    CHECK_FALSE(client->peer_certificate_sha256());
    client->send_text(std::make_shared<const std::string>("plain"));
    REQUIRE(events->wait_messages(1));
    CHECK(events->snapshot().at(0) == "text:echo:plain");
    std::lock_guard lock(seen->mutex);
    CHECK_FALSE(seen->requests.at(0).tls);
    server->stop(0ms);
}

TEST_CASE("server keepalive pings reach a TLS client that reads", "[platform]") {
    auto seen = std::make_shared<ServerSeen>();
    auto handlers = server_handlers(seen);
    handlers.ping_interval = 200ms; // test values; the remote host's are 20 s
    handlers.ping_timeout = 200ms;
    TlsServer f(std::move(handlers));
    t::WsTestClient client;
    t::WsTestClient::Options options;
    options.tls = true;
    REQUIRE(client.connect(f.endpoint.port, "/ws", options));
    CHECK(client.peer_certificate_sha256() ==
          certificate_sha256_from_pem(t::test_identity().cert_pem));
    // Reading for a while answers each ping.
    CHECK_FALSE(client.wait_closed(900ms));
    CHECK(client.pings_received() >= 2);
    client.send_text("still here");
    const auto echoed = client.receive(5s);
    REQUIRE(echoed);
    CHECK(echoed->data == "echo:still here");
}

TEST_CASE("a client that stops answering pings is closed 1011 after the timeout", "[platform]") {
    auto seen = std::make_shared<ServerSeen>();
    auto handlers = server_handlers(seen);
    handlers.ping_interval = 200ms;
    handlers.ping_timeout = 200ms;
    TlsServer f(std::move(handlers));
    t::WsTestClient client;
    t::WsTestClient::Options options;
    options.tls = true;
    REQUIRE(client.connect(f.endpoint.port, "/ws", options));
    // The client reads nothing now, so the first ping goes unanswered.
    REQUIRE(seen->wait([&] { return !seen->closes.empty(); }, 5s));
    {
        std::lock_guard lock(seen->mutex);
        CHECK(seen->closes.at(0) ==
              std::pair<std::uint16_t, std::string>{1011, "keepalive ping timeout"});
    }
    CHECK(client.wait_closed(5s));
}

TEST_CASE("ping() times the round trip and gives nullopt when no pong comes", "[platform]") {
    auto seen = std::make_shared<ServerSeen>();
    // One message delivered and never released stops the server reading, so pings sent after
    // it go unanswered.
    auto handlers = server_handlers(seen, false, false);
    handlers.max_queued_messages = 1;
    TlsServer f(std::move(handlers));
    auto events = std::make_shared<t::ClientEvents>();
    auto client = ws_connect(f.reactor, t::recording_options(events, t::wss_url(f.endpoint.port)));
    REQUIRE(events->wait_open());

    std::promise<std::optional<std::chrono::nanoseconds>> answered;
    client->ping(5s, [&](std::optional<std::chrono::nanoseconds> rtt) { answered.set_value(rtt); });
    const auto rtt = answered.get_future().get();
    REQUIRE(rtt);
    CHECK(*rtt < 1s);
    CHECK(rtt->count() > 0);

    client->send_text(std::make_shared<const std::string>("hold"));
    REQUIRE(seen->wait([&] { return seen->count("text:") == 1; }));
    std::promise<std::optional<std::chrono::nanoseconds>> unanswered;
    const auto started = std::chrono::steady_clock::now();
    client->ping(300ms,
                 [&](std::optional<std::chrono::nanoseconds> rtt) { unanswered.set_value(rtt); });
    CHECK_FALSE(unanswered.get_future().get());
    CHECK(std::chrono::steady_clock::now() - started >= 300ms);
    CHECK(client->is_open());
}

TEST_CASE("ping() on a closed connection gives nullopt at once", "[platform]") {
    TlsServer f;
    auto events = std::make_shared<t::ClientEvents>();
    auto client = ws_connect(f.reactor, t::recording_options(events, t::wss_url(f.endpoint.port)));
    REQUIRE(events->wait_open());
    client->close(1000, {});
    REQUIRE(events->wait_closed());
    std::promise<std::optional<std::chrono::nanoseconds>> answered;
    client->ping(5s, [&](std::optional<std::chrono::nanoseconds> rtt) { answered.set_value(rtt); });
    auto future = answered.get_future();
    REQUIRE(future.wait_for(2s) == std::future_status::ready);
    CHECK_FALSE(future.get());
}

TEST_CASE("max_queued_messages = 2 stops reading after two unreleased messages", "[platform]") {
    auto seen = std::make_shared<ServerSeen>();
    auto handlers = server_handlers(seen, false, false);
    handlers.max_queued_messages = 2; // host.py MAX_QUEUED_FRAMES_PER_CLIENT
    TlsServer f(std::move(handlers));
    auto events = std::make_shared<t::ClientEvents>();
    auto client = ws_connect(f.reactor, t::recording_options(events, t::wss_url(f.endpoint.port)));
    REQUIRE(events->wait_open());
    for (const char* text : {"one", "two", "three"}) {
        client->send_text(std::make_shared<const std::string>(text));
    }
    REQUIRE(seen->wait([&] { return seen->count("text:") == 2; }));
    // The third frame waits in the socket, not dropped.
    CHECK_FALSE(seen->wait([&] { return seen->count("text:") == 3; }, 400ms));

    seen->connection(0)->release_message();
    REQUIRE(seen->wait([&] { return seen->count("text:") == 3; }));
    CHECK(seen->snapshot() ==
          std::vector<std::string>{"open", "text:one", "text:two", "text:three"});
}

TEST_CASE("a message over the client's max_message_bytes closes 1009", "[platform]") {
    auto seen = std::make_shared<ServerSeen>();
    TlsServer f(server_handlers(seen, false));
    auto events = std::make_shared<t::ClientEvents>();
    auto options = t::recording_options(events, t::wss_url(f.endpoint.port));
    options.max_message_bytes = 1024;
    auto client = ws_connect(f.reactor, std::move(options));
    REQUIRE(events->wait_open());
    REQUIRE(seen->wait([&] { return !seen->opened.empty(); }));
    seen->connection(0)->send_text(std::make_shared<const std::string>(1024, 'x')); // fits
    REQUIRE(events->wait_messages(1));
    seen->connection(0)->send_text(std::make_shared<const std::string>(1025, 'x'));
    // The client fails the connection with 1009, which the server hears.
    REQUIRE(seen->wait([&] { return !seen->closes.empty(); }));
    {
        std::lock_guard lock(seen->mutex);
        CHECK(seen->closes.at(0).first == 1009);
    }
    // websockets' client ignores the server's answer after failing: its close_code is 1006.
    REQUIRE(events->wait_closed());
    CHECK(events->closed->first == 1006);
    CHECK(client->close_code() == std::optional<std::uint16_t>(1006));
    CHECK(events->snapshot().size() == 1);
}

TEST_CASE("a server's close with 4409 reaches the client's close_code", "[platform]") {
    auto seen = std::make_shared<ServerSeen>();
    TlsServer f(server_handlers(seen));
    auto events = std::make_shared<t::ClientEvents>();
    auto client = ws_connect(f.reactor, t::recording_options(events, t::wss_url(f.endpoint.port)));
    REQUIRE(events->wait_open());
    CHECK_FALSE(client->close_code());
    REQUIRE(seen->wait([&] { return !seen->opened.empty(); }));
    seen->connection(0)->close(4409, "engine changed");
    REQUIRE(events->wait_closed());
    CHECK(*events->closed == std::pair<std::uint16_t, std::string>{4409, "engine changed"});
    CHECK(client->close_code() == std::optional<std::uint16_t>(4409));
    CHECK_FALSE(client->is_open());
    REQUIRE(seen->wait([&] { return !seen->closes.empty(); }));
    std::lock_guard lock(seen->mutex);
    CHECK(seen->closes.at(0).first == 4409);
}

TEST_CASE("the client's close reaches the server and closes the client", "[platform]") {
    auto seen = std::make_shared<ServerSeen>();
    TlsServer f(server_handlers(seen));
    auto events = std::make_shared<t::ClientEvents>();
    auto client = ws_connect(f.reactor, t::recording_options(events, t::wss_url(f.endpoint.port)));
    REQUIRE(events->wait_open());
    client->send_text(std::make_shared<const std::string>("last"));
    client->close(1000, "bye");
    CHECK_FALSE(client->is_open());
    REQUIRE(events->wait_closed());
    CHECK(events->closed->first == 1000);
    CHECK(client->close_code() == std::optional<std::uint16_t>(1000));
    REQUIRE(seen->wait([&] { return !seen->closes.empty(); }));
    // Frames sent before close() go out first.
    CHECK(seen->snapshot() == std::vector<std::string>{"open", "text:last", "close:1000"});
    std::lock_guard lock(seen->mutex);
    CHECK(seen->closes.at(0).second == "bye");
}

TEST_CASE("a server that stops closes the client with its 1012", "[platform]") {
    auto seen = std::make_shared<ServerSeen>();
    TlsServer f(server_handlers(seen));
    auto events = std::make_shared<t::ClientEvents>();
    auto client = ws_connect(f.reactor, t::recording_options(events, t::wss_url(f.endpoint.port)));
    REQUIRE(events->wait_open());
    // Closes 1012 politely, then forces what is still open once the grace ends. With no grace
    // the forced close is queued right behind the close frame and, on a busy machine, ends the
    // TLS stream before the frame is out (the client then sees 1006). stop returns as soon as
    // the handshake is done, so the grace costs nothing when the machine is idle.
    f.server->stop(10s);
    REQUIRE(events->wait_closed());
    CHECK(events->closed->first == 1012);
}

TEST_CASE("a send that races the close leaves the client's queued_bytes at 0", "[platform]") {
    Reactor reactor;
    auto server = HttpServer::create(reactor);
    auto seen = std::make_shared<ServerSeen>();
    // One message delivered and never released stops the server reading: the client's frames
    // wait behind a write that does not complete.
    auto handlers = server_handlers(seen, false, false);
    handlers.max_queued_messages = 1;
    server->websocket("/ws", std::move(handlers));
    const auto endpoint = t::listen_loopback(*server);
    auto events = std::make_shared<t::ClientEvents>();
    auto client = ws_connect(
        reactor,
        t::recording_options(events, "ws://127.0.0.1:" + std::to_string(endpoint.port) + "/ws"));
    REQUIRE(events->wait_open());

    // The last frame's string is freed inside finish, after the connection closed and before
    // finish counts the frames off. Its deleter sends then, as another thread's send_text can:
    // the frame is counted at once and its enqueue runs on the strand after finish.
    struct Race {
        std::mutex mutex;
        WsClientConnection* client = nullptr; // null once the test's handle is going
        bool released = false;
        bool released_open = false;
        std::promise<void> drained;
    };
    auto race = std::make_shared<Race>();
    auto drained = race->drained.get_future();
    race->client = client.get();
    struct Detach {
        std::shared_ptr<Race> race;
        ~Detach() {
            std::lock_guard lock(race->mutex);
            race->client = nullptr;
        }
    } detach{race};

    const auto megabyte = std::make_shared<const std::string>(std::size_t{1} << 20, 'm');
    for (int i = 0; i < 64; ++i) {
        client->send_text(megabyte);
    }
    client->send_text(std::shared_ptr<const std::string>(
        new std::string("last"), [race](const std::string* text) {
            delete text;
            std::lock_guard lock(race->mutex);
            if (race->client == nullptr || race->released) {
                return;
            }
            race->released = true;
            race->released_open = race->client->is_open();
            race->client->send_text(std::make_shared<const std::string>("late"));
            // Posted after that send's enqueue: it answers once the enqueue has run.
            race->client->ping(10s, [race](std::optional<std::chrono::nanoseconds>) {
                race->drained.set_value();
            });
        }));

    // The server's force close drops the connection while the client's write is still pending.
    server->stop(0ms);
    REQUIRE(events->wait_closed());
    REQUIRE(drained.wait_for(10s) == std::future_status::ready);
    {
        std::lock_guard lock(race->mutex);
        CHECK_FALSE(race->released_open);
    }
    CHECK(client->queued_bytes() == 0);
}

TEST_CASE("a connection dropped without a close frame closes the client 1006", "[platform]") {
    WsHandlers handlers;
    handlers.on_text = [](const std::shared_ptr<WsConnection>&, std::string_view) {
        throw std::runtime_error("handler failed"); // the server drops the socket
    };
    TlsServer f(std::move(handlers));
    auto events = std::make_shared<t::ClientEvents>();
    auto client = ws_connect(f.reactor, t::recording_options(events, t::wss_url(f.endpoint.port)));
    REQUIRE(events->wait_open());
    t::ScopedCapture capture; // the handler's failure is logged
    client->send_text(std::make_shared<const std::string>("boom"));
    REQUIRE(events->wait_closed());
    CHECK(events->closed->first == 1006);
    CHECK(client->close_code() == std::optional<std::uint16_t>(1006));
}

TEST_CASE("dropping the handle closes the connection and silences its callbacks", "[platform]") {
    auto seen = std::make_shared<ServerSeen>();
    TlsServer f(server_handlers(seen));
    auto events = std::make_shared<t::ClientEvents>();
    auto client = ws_connect(f.reactor, t::recording_options(events, t::wss_url(f.endpoint.port)));
    REQUIRE(events->wait_open());
    client.reset();
    REQUIRE(seen->wait([&] { return !seen->closes.empty(); }));
    {
        std::lock_guard lock(seen->mutex);
        CHECK(seen->closes.at(0).first == 1000);
    }
    CHECK_FALSE(events->wait_closed(300ms));
}

TEST_CASE("dropping the handle silences a ping still waiting for its pong", "[platform]") {
    // The remote client's heartbeat captures its owner, which is gone once it drops the handle.
    auto seen = std::make_shared<ServerSeen>();
    auto handlers = server_handlers(seen, false, false);
    handlers.max_queued_messages = 1; // one unreleased message: the server reads no pong
    TlsServer f(std::move(handlers));
    auto events = std::make_shared<t::ClientEvents>();
    auto client = ws_connect(f.reactor, t::recording_options(events, t::wss_url(f.endpoint.port)));
    REQUIRE(events->wait_open());
    client->send_text(std::make_shared<const std::string>("hold"));
    REQUIRE(seen->wait([&] { return seen->count("text:") == 1; }));
    auto calls = std::make_shared<std::atomic<int>>(0);
    client->ping(100ms, [calls](std::optional<std::chrono::nanoseconds>) { ++*calls; });
    client.reset();
    // Past the ping's timeout and the client's close_timeout of 2 s, which ends the connection.
    std::this_thread::sleep_for(2500ms);
    CHECK(calls->load() == 0);
    CHECK_FALSE(events->wait_closed(1ms));
}

TEST_CASE("open_timeout drops a TLS connection that never upgrades", "[platform]") {
    WsHandlers handlers;
    handlers.open_timeout = 300ms;
    TlsServer f(std::move(handlers));
    t::RawClient raw(f.endpoint.port);
    const auto started = std::chrono::steady_clock::now();
    // Nothing is sent: no TLS handshake, no request. The server gives up after open_timeout.
    CHECK(raw.closed_by_server());
    CHECK(std::chrono::steady_clock::now() - started < 3s);
}

TEST_CASE("a route without permessage-deflate does not negotiate it when offered", "[platform]") {
    Reactor reactor;
    auto server = HttpServer::create(reactor);
    WsHandlers handlers;
    handlers.permessage_deflate = false;
    server->websocket("/plain", handlers);
    server->websocket("/deflate", WsHandlers{});
    const auto endpoint = t::listen_loopback(*server);
    const auto upgrade = [&](std::string_view path) {
        t::RawClient raw(endpoint.port);
        raw.send("GET " + std::string(path) +
                 " HTTP/1.1\r\nHost: 127.0.0.1\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
                 "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\nSec-WebSocket-Version: 13\r\n"
                 "Sec-WebSocket-Extensions: permessage-deflate; client_max_window_bits\r\n\r\n");
        return t::RawClient::lower(raw.read_until("\r\n\r\n"));
    };
    const auto plain = upgrade("/plain");
    CHECK(plain.starts_with("http/1.1 101"));
    CHECK(plain.find("sec-websocket-extensions") == std::string::npos);
    const auto deflate = upgrade("/deflate");
    CHECK(deflate.find("sec-websocket-extensions: permessage-deflate") != std::string::npos);
    server->stop(0ms);
}

TEST_CASE("an upgrade to another path goes to the WebSocket fallback, plain requests do not",
          "[platform]") {
    Reactor reactor;
    auto server = HttpServer::create(reactor);
    server->websocket("/engine", WsHandlers{});
    server->set_websocket_fallback([](const HttpRequest&, std::shared_ptr<Responder> r) {
        r->send(404, {{"content-type", "text/plain"}}, "Not an endpoint\n");
    });
    const auto endpoint = t::listen_loopback(*server);
    t::RawClient raw(endpoint.port);
    raw.send("GET /x HTTP/1.1\r\nHost: 127.0.0.1\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
             "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\nSec-WebSocket-Version: 13\r\n\r\n");
    const auto answer = raw.read_response();
    CHECK(answer.starts_with("HTTP/1.1 404 Not Found\r\n"));
    CHECK(answer.ends_with("\r\n\r\nNot an endpoint\n"));
    CHECK(t::RawClient::lower(answer).find("connection: close") != std::string::npos);
    CHECK(raw.closed_by_server());

    t::RawClient plain(endpoint.port);
    plain.send("GET /x HTTP/1.1\r\nHost: 127.0.0.1\r\n\r\n");
    CHECK(plain.read_response().find(R"({"detail":"Not Found"})") != std::string::npos);
    server->stop(0ms);
}

TEST_CASE("parse_ws_url reads what the remote client builds", "[platform]") {
    const auto v4 = net::detail::parse_ws_url("wss://192.168.1.5:47821/openwhisper/v1/engine");
    REQUIRE(v4);
    CHECK(v4->secure);
    CHECK(v4->host == "192.168.1.5");
    CHECK(v4->port == 47821);
    CHECK(v4->target == "/openwhisper/v1/engine");
    CHECK(v4->host_is_ip);
    CHECK(v4->host_header() == "192.168.1.5:47821");

    const auto v6 = net::detail::parse_ws_url("wss://[fd7a:115c:a1e0::1]:47821/p?x=1");
    REQUIRE(v6);
    CHECK(v6->host == "fd7a:115c:a1e0::1");
    CHECK(v6->target == "/p?x=1");
    CHECK(v6->host_header() == "[fd7a:115c:a1e0::1]:47821");

    const auto named = net::detail::parse_ws_url("ws://desk");
    REQUIRE(named);
    CHECK(named->port == 80);
    CHECK(named->target == "/");
    CHECK_FALSE(named->host_is_ip);
    CHECK(named->host_header() == "desk");
    CHECK(net::detail::parse_ws_url("wss://desk:443/")->host_header() == "desk");

    CHECK_FALSE(net::detail::parse_ws_url("http://desk/"));
    CHECK_FALSE(net::detail::parse_ws_url("wss://:47821/"));
    CHECK_FALSE(net::detail::parse_ws_url("wss://desk:0/"));
    CHECK_FALSE(net::detail::parse_ws_url("wss://desk:70000/"));
    CHECK_FALSE(net::detail::parse_ws_url("wss://[::1/"));
}

TEST_CASE("a TLS server on 0.0.0.0 answers ws_connect on this computer's LAN address",
          "[interactive]") {
    // Serving on every interface opens the port to the network (the remote host's default bind):
    // written for the owner, never run by a worker.
    Reactor reactor;
    auto server = HttpServer::create(reactor);
    auto seen = std::make_shared<ServerSeen>();
    server->websocket("/ws", server_handlers(seen));
    const auto endpoint =
        server->listen(Listen{.address = "0.0.0.0", .port = 0, .tls = t::test_identity()});
    REQUIRE(endpoint);
    const auto lan = discover_lan_ipv4();
    REQUIRE(lan);
    auto events = std::make_shared<t::ClientEvents>();
    auto client = ws_connect(
        reactor, t::recording_options(events, "wss://" + *lan + ":" +
                                                  std::to_string(endpoint->port) + "/ws"));
    REQUIRE(events->wait_open());
    client->send_text(std::make_shared<const std::string>("hi"));
    REQUIRE(events->wait_messages(1));
    CHECK(events->snapshot().at(0) == "text:echo:hi");
    server->stop(0ms);
}
