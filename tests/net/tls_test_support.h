#pragma once

// Helpers for the TLS and WebSocket client tests: a TLS server on 127.0.0.1, the events one
// ws_connect connection reports, and a raw OpenSSL client for what ws_connect does not show
// (protocol versions, session tickets). Everything here talks to 127.0.0.1 only.

#include <boost/asio/connect.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/ssl.hpp>
#include <boost/asio/write.hpp>

#ifdef near
#undef near
#endif
#ifdef far
#undef far
#endif

#include <openssl/ssl.h>

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

#include "net/http_server.h"
#include "net/reactor.h"
#include "net/tls.h"
#include "net/ws_client.h"

namespace fv::net::testing {

// One identity for the whole test binary: generating a key per test adds nothing.
inline const TlsIdentity& test_identity() {
    static const TlsIdentity identity = generate_self_signed_p256("OpenWhisper_CPP host test");
    return identity;
}

inline Endpoint listen_tls(HttpServer& server, TlsIdentity identity = test_identity()) {
    auto endpoint =
        server.listen(Listen{.address = "127.0.0.1", .port = 0, .tls = std::move(identity)});
    if (!endpoint) {
        throw std::runtime_error(endpoint.error());
    }
    return *endpoint;
}

inline std::string wss_url(std::uint16_t port, std::string_view path = "/ws") {
    return "wss://127.0.0.1:" + std::to_string(port) + std::string(path);
}

// What one ws_connect connection reported, in order, with a way to wait for it.
struct ClientEvents {
    std::mutex mutex;
    std::condition_variable changed;
    bool opened = false;
    std::vector<std::string> messages; // "text:..", "binary:<n>"
    std::optional<std::pair<std::uint16_t, std::string>> closed;
    std::optional<WsConnectFailure> failure;

    void note(const std::function<void()>& change) {
        {
            std::lock_guard lock(mutex);
            change();
        }
        changed.notify_all();
    }
    bool wait(const std::function<bool()>& done, std::chrono::milliseconds timeout) {
        std::unique_lock lock(mutex);
        return changed.wait_for(lock, timeout, done);
    }
    bool wait_open(std::chrono::milliseconds timeout = std::chrono::seconds(10)) {
        return wait([&] { return opened || failure.has_value() || closed.has_value(); }, timeout) &&
               opened;
    }
    bool wait_failure(std::chrono::milliseconds timeout = std::chrono::seconds(15)) {
        return wait([&] { return failure.has_value() || opened; }, timeout) && failure;
    }
    bool wait_closed(std::chrono::milliseconds timeout = std::chrono::seconds(10)) {
        return wait([&] { return closed.has_value(); }, timeout);
    }
    bool wait_messages(std::size_t count,
                       std::chrono::milliseconds timeout = std::chrono::seconds(10)) {
        return wait([&] { return messages.size() >= count; }, timeout);
    }
    std::vector<std::string> snapshot() {
        std::lock_guard lock(mutex);
        return messages;
    }
};

inline WsClientOptions recording_options(const std::shared_ptr<ClientEvents>& events,
                                         std::string url) {
    WsClientOptions options;
    options.url = std::move(url);
    options.on_open = [events] {
        events->note([&] { events->opened = true; });
    };
    options.on_text = [events](std::string_view text) {
        events->note([&] { events->messages.push_back("text:" + std::string(text)); });
    };
    options.on_binary = [events](std::span<const std::byte> data) {
        events->note([&] { events->messages.push_back("binary:" + std::to_string(data.size())); });
    };
    options.on_close = [events](std::uint16_t code, std::string_view reason) {
        events->note([&] { events->closed.emplace(code, std::string(reason)); });
    };
    options.on_connect_failed = [events](const WsConnectFailure& failure) {
        events->note([&] { events->failure = failure; });
    };
    return options;
}

// What one TLS handshake with the server showed a raw OpenSSL client.
struct TlsProbe {
    bool connected = false;
    std::string error;
    int version = 0; // TLS1_2_VERSION, TLS1_3_VERSION
    int tickets = 0; // session tickets received
};

namespace detail_support {
inline int& ticket_count() {
    thread_local int count = 0;
    return count;
}
inline int count_ticket(SSL*, SSL_SESSION*) {
    ++ticket_count();
    return 0; // OpenSSL keeps ownership of the session
}
} // namespace detail_support

// A TLS client limited to [min_version, max_version] (0: OpenSSL's default bound). After the
// handshake it sends one HTTP request and reads the answer, so that tickets the server sends
// after the handshake are read too.
inline TlsProbe tls_probe(std::uint16_t port, int min_version, int max_version) {
    namespace asio = boost::asio;
    TlsProbe probe;
    asio::io_context io;
    asio::ssl::context context(asio::ssl::context::tls_client);
    SSL_CTX* native = context.native_handle();
    SSL_CTX_set_min_proto_version(native, min_version);
    SSL_CTX_set_max_proto_version(native, max_version);
    if (max_version != 0 && max_version < TLS1_2_VERSION) {
        // OpenSSL 3 refuses TLS 1.1 at its default security level.
        SSL_CTX_set_security_level(native, 0);
        SSL_CTX_set_cipher_list(native, "DEFAULT:@SECLEVEL=0");
    }
    SSL_CTX_set_session_cache_mode(native, SSL_SESS_CACHE_CLIENT);
    SSL_CTX_sess_set_new_cb(native, &detail_support::count_ticket);
    context.set_verify_mode(asio::ssl::verify_none);
    detail_support::ticket_count() = 0;

    asio::ssl::stream<asio::ip::tcp::socket> stream(io, context);
    boost::system::error_code error;
    stream.next_layer().connect({asio::ip::make_address("127.0.0.1"), port}, error);
    if (!error) {
        stream.handshake(asio::ssl::stream_base::client, error);
    }
    if (error) {
        probe.error = error.message();
        return probe;
    }
    probe.connected = true;
    probe.version = SSL_version(stream.native_handle());
    const std::string request = "GET /nothing HTTP/1.1\r\nHost: 127.0.0.1\r\n"
                                "Connection: close\r\n\r\n";
    asio::write(stream, asio::buffer(request), error);
    char chunk[4096];
    while (!error) {
        stream.read_some(asio::buffer(chunk), error);
    }
    probe.tickets = detail_support::ticket_count();
    boost::system::error_code ignored;
    stream.next_layer().close(ignored);
    return probe;
}

} // namespace fv::net::testing
