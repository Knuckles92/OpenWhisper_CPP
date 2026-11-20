#include "net/testing/ws_test_client.h"

#include <deque>
#include <functional>
#include <utility>

// First: it sets the Windows version Asio is built for.
#include "net/detail/asio.h"

#include <boost/asio/connect.hpp>
#include <boost/asio/socket_base.hpp>
#include <boost/beast/core/buffers_to_string.hpp>
#include <boost/beast/core/flat_buffer.hpp>
#include <boost/beast/core/tcp_stream.hpp>
#include <boost/beast/websocket.hpp>
#include <boost/beast/websocket/ssl.hpp>

#include "net/detail/tls_context.h"
#include "net/tls.h"

namespace fv::net::testing {

namespace {

namespace asio = boost::asio;
namespace beast = boost::beast;
namespace websocket = boost::beast::websocket;
using tcp = boost::asio::ip::tcp;
using detail::PlainStream;
using detail::TlsStream;

constexpr std::uint16_t kAbnormal = 1006;
constexpr std::uint16_t kNoStatus = 1005;

} // namespace

struct WsTestClient::Impl {
    asio::io_context io;
    std::shared_ptr<detail::ssl::context> tls_context;
    std::optional<websocket::stream<PlainStream>> plain;
    std::optional<websocket::stream<TlsStream>> secure;
    beast::flat_buffer buffer;
    std::deque<Message> inbox;
    bool reading = false;
    bool closed = false;
    std::optional<std::uint16_t> code;
    std::string reason;
    std::string error;
    std::string fingerprint;
    int pings = 0;
    std::chrono::milliseconds timeout{10'000};

    bool connected() const { return plain || secure; }

    // Calls f with whichever stream is in use.
    template <class F> decltype(auto) with(F&& f) {
        if (secure) {
            return f(*secure);
        }
        return f(*plain);
    }

    beast::tcp_stream& lowest() {
        return with([](auto& ws) -> beast::tcp_stream& { return beast::get_lowest_layer(ws); });
    }

    void note_close(const boost::system::error_code& failure) {
        closed = true;
        if (code) {
            return;
        }
        with([&](auto& ws) {
            const auto& got = ws.reason();
            if (failure == websocket::error::closed || got.code != websocket::close_code::none) {
                const auto value = static_cast<std::uint16_t>(got.code);
                code = value == 0 ? kNoStatus : value;
                reason.assign(got.reason.data(), got.reason.size());
            } else {
                code = kAbnormal;
            }
        });
    }

    void start_read() {
        if (reading || closed || !connected()) {
            return;
        }
        reading = true;
        with([&](auto& ws) {
            ws.async_read(buffer, [this](const boost::system::error_code& failure, std::size_t) {
                reading = false;
                if (failure) {
                    note_close(failure);
                    return;
                }
                const bool text = with([](auto& stream) { return stream.got_text(); });
                inbox.push_back({!text, beast::buffers_to_string(buffer.data())});
                buffer.consume(buffer.size());
                start_read();
            });
        });
    }

    // Runs the client's handlers until `done` or the deadline; true when `done`.
    bool run_until(const std::function<bool()>& done, std::chrono::milliseconds limit) {
        const auto deadline = std::chrono::steady_clock::now() + limit;
        while (!done()) {
            const auto left = deadline - std::chrono::steady_clock::now();
            if (left <= std::chrono::steady_clock::duration::zero()) {
                return done();
            }
            if (io.stopped()) {
                io.restart();
            }
            if (io.run_one_for(left) == 0 && io.stopped() && !done()) {
                return false; // nothing left that could make it true
            }
        }
        return true;
    }

    // run_until(done), but when the limit passes first the socket is closed and the handlers
    // still run before this returns: they capture the caller's locals, which must not outlive
    // the call (a timed-out handshake or write would otherwise complete into a dead frame).
    bool finish(const std::function<bool()>& done, std::chrono::milliseconds limit) {
        if (run_until(done, limit)) {
            return true;
        }
        boost::system::error_code ignored;
        lowest().socket().close(ignored);
        run_until(done, std::chrono::milliseconds(5'000));
        return false;
    }
};

WsTestClient::WsTestClient() : impl_(std::make_unique<Impl>()) {}

WsTestClient::~WsTestClient() {
    abort();
}

bool WsTestClient::connect(std::uint16_t port, std::string_view path_and_query) {
    return connect(port, path_and_query, Options{});
}

bool WsTestClient::connect(std::uint16_t port, std::string_view path_and_query,
                           const Options& options) {
    Impl& d = *impl_;
    d.timeout = options.timeout;
    if (options.tls) {
        d.tls_context = detail::make_client_context();
        d.secure.emplace(d.io, *d.tls_context);
    } else {
        d.plain.emplace(d.io);
    }
    auto& socket = d.lowest().socket();
    boost::system::error_code failure;
    socket.open(tcp::v4(), failure);
    if (!failure && options.receive_buffer_bytes > 0) {
        socket.set_option(asio::socket_base::receive_buffer_size(options.receive_buffer_bytes),
                          failure);
    }
    if (failure) {
        d.error = failure.message();
        return false;
    }
    bool finished = false;
    d.lowest().expires_after(options.timeout);
    d.lowest().async_connect(tcp::endpoint(asio::ip::make_address_v4("127.0.0.1"), port),
                             [&](const boost::system::error_code& result) {
                                 failure = result;
                                 finished = true;
                             });
    if (!d.finish([&] { return finished; }, options.timeout) || failure) {
        d.error = failure && finished ? failure.message() : "connect timed out";
        return false;
    }
    if (d.secure) {
        finished = false;
        d.secure->next_layer().async_handshake(detail::ssl::stream_base::client,
                                               [&](const boost::system::error_code& result) {
                                                   failure = result;
                                                   finished = true;
                                               });
        if (!d.finish([&] { return finished; }, options.timeout) || failure) {
            d.error = failure && finished ? "TLS: " + failure.message() : "TLS timed out";
            return false;
        }
        const auto der = detail::peer_certificate_der(d.secure->next_layer().native_handle());
        d.fingerprint = der.empty() ? std::string() : certificate_sha256_from_der(der);
    }
    d.lowest().expires_never();
    const Headers headers = options.headers;
    websocket::response_type response;
    finished = false;
    const std::string host = "127.0.0.1:" + std::to_string(port);
    d.with([&](auto& ws) {
        if (options.deflate) {
            websocket::permessage_deflate deflate;
            deflate.client_enable = true;
            ws.set_option(deflate);
        }
        ws.auto_fragment(false);
        ws.set_option(
            websocket::stream_base::decorator([headers](websocket::request_type& request) {
                for (const auto& [name, value] : headers) {
                    request.set(name, value);
                }
            }));
        ws.control_callback([&d](websocket::frame_type kind, beast::string_view) {
            if (kind == websocket::frame_type::ping) {
                ++d.pings;
            }
        });
        ws.async_handshake(response, host, std::string(path_and_query),
                           [&](const boost::system::error_code& result) {
                               failure = result;
                               finished = true;
                           });
    });
    if (!d.finish([&] { return finished; }, options.timeout)) {
        d.error = "handshake timed out";
        return false;
    }
    if (failure) {
        d.error = response.result_int() != 0 && response.result_int() != 101
                      ? "HTTP " + std::to_string(response.result_int())
                      : failure.message();
        return false;
    }
    d.start_read();
    return true;
}

const std::string& WsTestClient::error() const {
    return impl_->error;
}

void WsTestClient::send_text(std::string_view text) {
    Impl& d = *impl_;
    if (!d.connected() || d.closed) {
        return;
    }
    bool finished = false;
    d.with([&](auto& ws) {
        ws.text(true);
        ws.async_write(asio::buffer(text.data(), text.size()),
                       [&](const boost::system::error_code&, std::size_t) { finished = true; });
    });
    d.finish([&] { return finished; }, d.timeout);
}

void WsTestClient::send_binary(std::span<const std::byte> data) {
    Impl& d = *impl_;
    if (!d.connected() || d.closed) {
        return;
    }
    bool finished = false;
    d.with([&](auto& ws) {
        ws.binary(true);
        ws.async_write(asio::buffer(data.data(), data.size()),
                       [&](const boost::system::error_code&, std::size_t) { finished = true; });
    });
    d.finish([&] { return finished; }, d.timeout);
}

std::optional<WsTestClient::Message> WsTestClient::receive(std::chrono::milliseconds timeout) {
    Impl& d = *impl_;
    if (!d.connected()) {
        return std::nullopt;
    }
    d.start_read();
    d.run_until([&] { return !d.inbox.empty() || d.closed; }, timeout);
    if (d.inbox.empty()) {
        return std::nullopt;
    }
    Message message = std::move(d.inbox.front());
    d.inbox.pop_front();
    return message;
}

bool WsTestClient::wait_closed(std::chrono::milliseconds timeout) {
    Impl& d = *impl_;
    if (!d.connected()) {
        return false;
    }
    d.start_read();
    return d.run_until([&] { return d.closed; }, timeout);
}

std::optional<std::uint16_t> WsTestClient::close_code() const {
    return impl_->code;
}

std::string WsTestClient::close_reason() const {
    return impl_->reason;
}

void WsTestClient::close(std::uint16_t code, std::string_view reason) {
    Impl& d = *impl_;
    if (!d.connected() || d.closed) {
        return;
    }
    bool finished = false;
    d.with([&](auto& ws) {
        ws.async_close(websocket::close_reason(static_cast<websocket::close_code>(code), reason),
                       [&](const boost::system::error_code&) { finished = true; });
    });
    d.finish([&] { return finished && !d.reading; }, d.timeout);
    d.closed = true;
}

void WsTestClient::abort() {
    Impl& d = *impl_;
    if (!d.connected()) {
        return;
    }
    boost::system::error_code ignored;
    d.lowest().socket().close(ignored);
    // Lets the pending read see the closed socket before its buffer goes away.
    d.run_until([&] { return !d.reading; }, std::chrono::milliseconds(1'000));
    d.closed = true;
}

int WsTestClient::pings_received() const {
    return impl_->pings;
}

std::string WsTestClient::peer_certificate_sha256() const {
    return impl_->fingerprint;
}

} // namespace fv::net::testing
