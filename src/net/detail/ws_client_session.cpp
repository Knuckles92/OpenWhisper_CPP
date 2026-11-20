#include "net/detail/ws_client_session.h"

#include <algorithm>
#include <atomic>
#include <charconv>
#include <chrono>
#include <deque>
#include <functional>
#include <mutex>
#include <random>
#include <utility>
#include <vector>

// First: it sets the Windows version Asio is built for.
#include "net/detail/asio.h"

#include <boost/asio/ip/address.hpp>
#include <boost/beast/websocket.hpp>
#include <boost/beast/websocket/ssl.hpp>

#include <openssl/ssl.h>

#include "base/hash.h"
#include "net/detail/reactor_impl.h"
#include "net/detail/tls_context.h"
#include "net/tls.h"

namespace fv::net::detail {

namespace {

namespace websocket = boost::beast::websocket;
using Kind = WsConnectFailure::Kind;

// Close codes (RFC 6455 7.4.1) as websockets reports them in close_code.
constexpr std::uint16_t kNoStatus = 1005;
constexpr std::uint16_t kAbnormal = 1006;

std::string ascii_upper(std::string text) {
    for (char& c : text) {
        if (c >= 'a' && c <= 'z') {
            c = static_cast<char>(c - 'a' + 'A');
        }
    }
    return text;
}

Kind connect_kind(const boost::system::error_code& error) {
    if (error == asio::error::connection_refused) {
        return Kind::Refused;
    }
    if (error == asio::error::timed_out || error == beast::error::timeout) {
        return Kind::Timeout;
    }
    return Kind::Unreachable;
}

std::string random_ping_payload() {
    thread_local std::mt19937 generator{std::random_device{}()};
    std::string payload(4, '\0');
    const auto bits = generator();
    for (int i = 0; i < 4; ++i) {
        payload[static_cast<std::size_t>(i)] = static_cast<char>((bits >> (8 * i)) & 0xff);
    }
    return payload;
}

// The callbacks of one connection, ping completions included. A callback runs under the lock,
// so a handle destroyed on another thread waits for it, and none starts after; one destroyed
// from inside a callback does not wait (the lock is recursive) and the running callback is a
// copy.
class Callbacks {
public:
    explicit Callbacks(WsClientOptions& options)
        : on_open_(std::move(options.on_open)), on_text_(std::move(options.on_text)),
          on_binary_(std::move(options.on_binary)), on_close_(std::move(options.on_close)),
          on_connect_failed_(std::move(options.on_connect_failed)) {}

    void open() {
        std::lock_guard lock(mutex_);
        if (auto call = on_open_) {
            call();
        }
    }
    void text(std::string_view text) {
        std::lock_guard lock(mutex_);
        if (auto call = on_text_) {
            call(text);
        }
    }
    void binary(std::span<const std::byte> data) {
        std::lock_guard lock(mutex_);
        if (auto call = on_binary_) {
            call(data);
        }
    }
    void closed(std::uint16_t code, std::string_view reason) {
        std::lock_guard lock(mutex_);
        if (auto call = on_close_) {
            call(code, reason);
        }
    }
    void failed(const WsConnectFailure& failure) {
        std::lock_guard lock(mutex_);
        if (on_connect_failed_) {
            auto call = on_connect_failed_;
            call(failure);
        } else if (auto call = on_close_) {
            call(kAbnormal, failure.detail);
        }
    }
    // A ping's done, unless the handle is gone (its owner may be too).
    void ping_done(const std::function<void(std::optional<std::chrono::nanoseconds>)>& done,
                   std::optional<std::chrono::nanoseconds> round_trip) {
        std::lock_guard lock(mutex_);
        if (!detached_ && done) {
            done(round_trip);
        }
    }
    void clear() {
        std::lock_guard lock(mutex_);
        detached_ = true;
        on_open_ = nullptr;
        on_text_ = nullptr;
        on_binary_ = nullptr;
        on_close_ = nullptr;
        on_connect_failed_ = nullptr;
    }

private:
    std::recursive_mutex mutex_;
    bool detached_ = false;
    std::function<void()> on_open_;
    std::function<void(std::string_view)> on_text_;
    std::function<void(std::span<const std::byte>)> on_binary_;
    std::function<void(std::uint16_t, std::string_view)> on_close_;
    std::function<void(const WsConnectFailure&)> on_connect_failed_;
};

// What the handle reaches of a connection, whatever its stream.
// Affinity: thread-safe.
class ClientCore : public WsClientConnection {
public:
    virtual void connect() = 0;
    // The handle is gone: no callback runs from now on, and an open connection closes 1000.
    virtual void detach() = 0;
};

// One client connection over PlainStream (ws://) or TlsStream (wss://). Every member runs on
// the connection's strand; the public entry points post there.
// Affinity: thread-safe (entry points), owned strand (the rest).
template <class Stream>
class ClientSession final : public ClientCore,
                            public std::enable_shared_from_this<ClientSession<Stream>> {
public:
    ClientSession(std::shared_ptr<ReactorImpl> reactor, WsClientOptions options, WsUrl url,
                  std::shared_ptr<ssl::context> tls)
        : reactor_(std::move(reactor)), strand_(asio::make_strand(reactor_->io())),
          options_(std::move(options)), callbacks_(options_), url_(std::move(url)),
          tls_(std::move(tls)), resolver_(strand_), deadline_(strand_), ws_(make_stream()) {}

    void connect() override {
        post([this] { begin(); });
    }

    void detach() override {
        callbacks_.clear();
        close(1000, {});
    }

    using WsConnection::send_text; // the timed send, WsConnection's default
    void send_text(std::shared_ptr<const std::string> text) override {
        if (!text) {
            text = std::make_shared<const std::string>();
        }
        Outgoing item;
        item.size = text->size();
        item.text = std::move(text);
        queue(std::move(item));
    }

    void send_binary(std::vector<std::byte> data) override {
        Outgoing item;
        item.size = data.size();
        item.binary = std::move(data);
        item.is_text = false;
        queue(std::move(item));
    }

    void close(std::uint16_t code, std::string reason) override {
        close_called_ = true;
        post([this, code, reason = std::move(reason)]() mutable {
            if (state_ == State::Opening) {
                fail(Kind::Cancelled, "closed before it opened");
                return;
            }
            begin_close(code, std::move(reason));
        });
    }

    std::size_t queued_bytes() const override { return queued_bytes_.load(); }
    bool is_open() const override { return open_.load() && !close_called_.load(); }
    Peer local_endpoint() const override {
        std::lock_guard lock(facts_mutex_);
        return local_;
    }
    Peer remote_endpoint() const override {
        std::lock_guard lock(facts_mutex_);
        return remote_;
    }
    std::optional<std::string> peer_certificate_sha256() const override {
        std::lock_guard lock(facts_mutex_);
        return fingerprint_;
    }
    std::optional<std::uint16_t> close_code() const override {
        std::lock_guard lock(facts_mutex_);
        return close_code_;
    }

    void ping(std::chrono::milliseconds timeout,
              std::function<void(std::optional<std::chrono::nanoseconds>)> done) override {
        if (!post([this, timeout, done]() mutable { start_ping(timeout, std::move(done)); })) {
            if (done) {
                done(std::nullopt);
            }
        }
    }

private:
    enum class State { Opening, Open, Closing, Closed };

    struct Outgoing {
        std::shared_ptr<const std::string> text;
        std::vector<std::byte> binary;
        bool is_text = true;
        std::size_t size = 0;
    };

    struct PingWaiter {
        std::string payload;
        std::function<void(std::optional<std::chrono::nanoseconds>)> done;
        std::shared_ptr<asio::steady_timer> timer;
        std::chrono::steady_clock::time_point sent_at;
        bool sent = false;
    };

    websocket::stream<Stream> make_stream() {
        if constexpr (kIsTls<Stream>) {
            return websocket::stream<Stream>(strand_, *tls_);
        } else {
            return websocket::stream<Stream>(strand_);
        }
    }

    beast::tcp_stream& lowest() { return beast::get_lowest_layer(ws_); }

    // False when the reactor no longer runs anything.
    bool post(std::function<void()> work) {
        if (reactor_->stopped()) {
            return false;
        }
        asio::post(strand_, [self = this->shared_from_this(), work = std::move(work)] { work(); });
        return true;
    }

    void queue(Outgoing item) {
        queued_bytes_ += item.size;
        const std::size_t size = item.size;
        if (!post([this, item = std::move(item)]() mutable { enqueue(std::move(item)); })) {
            queued_bytes_ -= size;
        }
    }

    void begin() {
        if (state_ != State::Opening) {
            return;
        }
        deadline_.expires_after(options_.connect_timeout);
        deadline_.async_wait([self = this->shared_from_this()](const boost::system::error_code& e) {
            if (!e) {
                self->fail(Kind::Timeout, "timed out during opening handshake");
            }
        });
        resolver_.async_resolve(
            url_.host, std::to_string(url_.port),
            [self = this->shared_from_this()](const boost::system::error_code& error,
                                              const tcp::resolver::results_type& results) {
                if (self->state_ != State::Opening) {
                    return;
                }
                if (error) {
                    self->fail(Kind::Dns, error.message());
                    return;
                }
                self->connect_to(results);
            });
    }

    void connect_to(const tcp::resolver::results_type& results) {
        lowest().async_connect(
            results, [self = this->shared_from_this()](const boost::system::error_code& error,
                                                       const tcp::endpoint&) {
                if (self->state_ != State::Opening) {
                    return;
                }
                if (error) {
                    self->fail(connect_kind(error), error.message());
                    return;
                }
                self->connected();
            });
    }

    void connected() {
        boost::system::error_code ignored;
        lowest().socket().set_option(tcp::no_delay(true), ignored);
        {
            std::lock_guard lock(facts_mutex_);
            boost::system::error_code error;
            if (const auto local = lowest().socket().local_endpoint(error); !error) {
                local_ = {local.address().to_string(), local.port()};
            }
            if (const auto remote = lowest().socket().remote_endpoint(error); !error) {
                remote_ = {remote.address().to_string(), remote.port()};
            }
        }
        if constexpr (kIsTls<Stream>) {
            SSL* native = ws_.next_layer().native_handle();
            // tls.py's client: SNI for a name, never for an address.
            if (!url_.host_is_ip && SSL_set_tlsext_host_name(native, url_.host.c_str()) != 1) {
                fail(Kind::Tls, take_openssl_error("SNI was refused"));
                return;
            }
            ws_.next_layer().async_handshake(
                ssl::stream_base::client,
                [self = this->shared_from_this()](const boost::system::error_code& error) {
                    if (self->state_ != State::Opening) {
                        return;
                    }
                    if (error) {
                        self->fail(Kind::Tls, error.message());
                        return;
                    }
                    if (!self->check_certificate()) {
                        return;
                    }
                    self->upgrade();
                });
        } else {
            upgrade();
        }
    }

    // The certificate's fingerprint, then the pin. False when the connection failed.
    bool check_certificate() {
        if constexpr (kIsTls<Stream>) {
            const auto der = peer_certificate_der(ws_.next_layer().native_handle());
            if (der.empty()) {
                fail(Kind::Tls, "the host presented no certificate");
                return false;
            }
            const std::string fingerprint = certificate_sha256_from_der(der);
            {
                std::lock_guard lock(facts_mutex_);
                fingerprint_ = fingerprint;
            }
            if (options_.pinned_sha256 && ascii_upper(*options_.pinned_sha256) != fingerprint) {
                fail(Kind::Tls, "certificate mismatch");
                return false;
            }
        }
        return true;
    }

    void upgrade() {
        // websockets' client sends each message as one frame and offers no extension.
        ws_.auto_fragment(false);
        ws_.read_message_max(options_.max_message_bytes);
        ws_.set_option(websocket::stream_base::decorator(
            [headers = options_.headers](websocket::request_type& request) {
                request.erase(http::field::user_agent);
                for (const auto& [name, value] : headers) {
                    request.set(name, value);
                }
            }));
        ws_.control_callback([this](websocket::frame_type kind, boost::beast::string_view data) {
            on_control(kind, std::string_view(data.data(), data.size()));
        });
        ws_.async_handshake(
            response_, url_.host_header(), url_.target,
            [self = this->shared_from_this()](const boost::system::error_code& error) {
                self->on_handshake(error);
            });
    }

    void on_handshake(const boost::system::error_code& error) {
        if (state_ != State::Opening) {
            return;
        }
        if (error) {
            const int status = response_.result_int();
            if (status != 0 && status != 101) {
                WsConnectFailure failure{Kind::HttpStatus, status,
                                         "HTTP " + std::to_string(status)};
                fail(std::move(failure));
            } else {
                fail(Kind::Handshake, error.message());
            }
            return;
        }
        deadline_.cancel();
        // The closing handshake waits close_timeout for the peer's close frame.
        websocket::stream_base::timeout timeouts;
        timeouts.handshake_timeout = options_.close_timeout;
        timeouts.idle_timeout = websocket::stream_base::none();
        timeouts.keep_alive_pings = false;
        ws_.set_option(timeouts);
        state_ = State::Open;
        open_ = true;
        callbacks_.open();
        if (state_ == State::Closed) {
            return;
        }
        read_next();
        if (!out_.empty() && !writing_) {
            write_next();
        }
    }

    void fail(Kind kind, std::string detail) { fail(WsConnectFailure{kind, 0, std::move(detail)}); }

    void fail(WsConnectFailure failure) {
        if (state_ != State::Opening) {
            return;
        }
        state_ = State::Closed;
        deadline_.cancel();
        resolver_.cancel();
        boost::system::error_code ignored;
        lowest().socket().close(ignored);
        {
            std::lock_guard lock(facts_mutex_);
            close_code_ = kAbnormal;
        }
        drop_queued();
        callbacks_.failed(failure);
    }

    // The peer's close frame was read, and only the end of the connection after it failed: a
    // Python peer drops TLS without close_notify (SSLSocket.shutdown), which OpenSSL reports as
    // a truncated stream.
    bool close_frame_arrived(const boost::system::error_code& error) const {
        return ws_.reason().code != websocket::close_code::none &&
               error.category() !=
                   websocket::make_error_code(websocket::error::closed).category() &&
               error != beast::error::timeout;
    }

    void read_next() {
        ws_.async_read(buffer_,
                       [self = this->shared_from_this()](const boost::system::error_code& error,
                                                         std::size_t) { self->on_read(error); });
    }

    void on_read(const boost::system::error_code& error) {
        if (state_ == State::Closed) {
            return;
        }
        if (error) {
            if (error == websocket::error::closed || close_frame_arrived(error)) {
                const auto& reason = ws_.reason();
                const auto code = static_cast<std::uint16_t>(reason.code);
                finish(code == 0 ? kNoStatus : code,
                       std::string(reason.reason.data(), reason.reason.size()));
            } else {
                // Lost, or failed by this side (a message over max_message_bytes sends 1009, a
                // protocol error 1002): websockets ignores the peer's answer then, so its
                // close_code reads 1006.
                finish(kAbnormal, {});
            }
            return;
        }
        const auto data = buffer_.data();
        if (state_ == State::Open) {
            if (ws_.got_text()) {
                callbacks_.text(
                    std::string_view(static_cast<const char*>(data.data()), data.size()));
            } else {
                callbacks_.binary(std::span<const std::byte>(
                    static_cast<const std::byte*>(data.data()), data.size()));
            }
        }
        buffer_.consume(buffer_.size());
        if (state_ != State::Closed) {
            read_next();
        }
    }

    void enqueue(Outgoing item) {
        if (state_ == State::Closing || state_ == State::Closed) {
            queued_bytes_ -= item.size;
            return;
        }
        out_.push_back(std::move(item));
        if (state_ == State::Open && !writing_) {
            write_next();
        }
    }

    void write_next() {
        if (out_.empty()) {
            return;
        }
        writing_ = true;
        Outgoing& item = out_.front();
        ws_.text(item.is_text);
        auto buffer = item.is_text ? asio::buffer(*item.text) : asio::buffer(item.binary);
        ws_.async_write(
            buffer, [self = this->shared_from_this()](const boost::system::error_code& error,
                                                      std::size_t) { self->on_written(error); });
    }

    void on_written(const boost::system::error_code& error) {
        writing_ = false;
        if (state_ == State::Closed) {
            out_.clear(); // finish counted the frame this write read and kept it only for it
            return;
        }
        if (!out_.empty()) {
            queued_bytes_ -= out_.front().size;
            out_.pop_front();
        }
        if (error) {
            finish(kAbnormal, {});
            return;
        }
        if (!out_.empty()) {
            write_next();
        } else if (state_ == State::Closing) {
            send_close();
        }
    }

    void drop_queued() {
        const std::size_t keep = writing_ ? 1 : 0;
        while (out_.size() > keep) {
            queued_bytes_ -= out_.back().size;
            out_.pop_back();
        }
    }

    void begin_close(std::uint16_t code, std::string reason) {
        if (state_ != State::Open) {
            return;
        }
        state_ = State::Closing;
        open_ = false;
        close_reason_sent_ = {code, std::move(reason)};
        if (!writing_ && out_.empty()) {
            send_close();
            return;
        }
        // websockets bounds the whole close, the frames still queued before the close frame
        // included, by close_timeout: a peer that stopped reading would otherwise hold the write
        // and the connection open for good. Beast's own handshake timeout starts only with the
        // close frame.
        deadline_.expires_after(options_.close_timeout);
        deadline_.async_wait(
            [self = this->shared_from_this()](const boost::system::error_code& error) {
                if (!error && self->state_ == State::Closing && !self->close_sent_) {
                    self->finish(kAbnormal, {});
                }
            });
    }

    void send_close() {
        if (close_sent_ || state_ != State::Closing) {
            return;
        }
        if (control_busy_) {
            close_waiting_ = true;
            return;
        }
        close_sent_ = true;
        deadline_.cancel(); // begin_close's wait for the queued frames
        control_busy_ = true;
        websocket::close_reason reason(static_cast<websocket::close_code>(close_reason_sent_.first),
                                       std::string_view(close_reason_sent_.second)
                                           .substr(0, websocket::reason_string::static_capacity));
        ws_.async_close(reason,
                        [self = this->shared_from_this()](const boost::system::error_code& error) {
                            self->control_busy_ = false;
                            if (self->state_ == State::Closed) {
                                return;
                            }
                            if (error && !self->close_frame_arrived(error)) {
                                self->finish(kAbnormal, {});
                                return;
                            }
                            const auto& got = self->ws_.reason();
                            const auto code = static_cast<std::uint16_t>(got.code);
                            self->finish(code == 0 ? kNoStatus : code,
                                         std::string(got.reason.data(), got.reason.size()));
                        });
    }

    void start_ping(std::chrono::milliseconds timeout,
                    std::function<void(std::optional<std::chrono::nanoseconds>)> done) {
        if (state_ != State::Open) {
            callbacks_.ping_done(done, std::nullopt);
            return;
        }
        PingWaiter waiter;
        waiter.payload = random_ping_payload();
        while (std::ranges::any_of(
            pings_, [&](const PingWaiter& other) { return other.payload == waiter.payload; })) {
            waiter.payload = random_ping_payload();
        }
        waiter.done = std::move(done);
        waiter.timer = std::make_shared<asio::steady_timer>(strand_, timeout);
        waiter.timer->async_wait([self = this->shared_from_this(), payload = waiter.payload](
                                     const boost::system::error_code& error) {
            if (!error) {
                self->ping_expired(payload);
            }
        });
        pings_.push_back(std::move(waiter));
        send_pings();
    }

    // Hands the pings not yet sent to Beast, one control frame at a time.
    void send_pings() {
        if (control_busy_ || state_ != State::Open) {
            return;
        }
        const auto next = std::ranges::find(pings_, false, &PingWaiter::sent);
        if (next == pings_.end()) {
            return;
        }
        next->sent = true;
        next->sent_at = std::chrono::steady_clock::now();
        control_busy_ = true;
        websocket::ping_data payload(next->payload.data(), next->payload.size());
        ws_.async_ping(payload,
                       [self = this->shared_from_this()](const boost::system::error_code&) {
                           self->control_busy_ = false;
                           if (self->close_waiting_) {
                               self->close_waiting_ = false;
                               self->send_close();
                           } else {
                               self->send_pings();
                           }
                       });
    }

    void ping_expired(const std::string& payload) {
        const auto found = std::ranges::find(pings_, payload, &PingWaiter::payload);
        if (found == pings_.end()) {
            return;
        }
        auto done = std::move(found->done);
        pings_.erase(found);
        callbacks_.ping_done(done, std::nullopt);
    }

    void on_control(websocket::frame_type kind, std::string_view data) {
        if (kind != websocket::frame_type::pong) {
            return;
        }
        const auto found = std::ranges::find(pings_, data, &PingWaiter::payload);
        if (found == pings_.end() || !found->sent) {
            return; // unsolicited, or for a ping that already timed out
        }
        const auto now = std::chrono::steady_clock::now();
        // websockets: a pong acknowledges its ping and every ping sent before it.
        std::vector<PingWaiter> answered(std::make_move_iterator(pings_.begin()),
                                         std::make_move_iterator(found + 1));
        pings_.erase(pings_.begin(), found + 1);
        for (auto& waiter : answered) {
            waiter.timer->cancel();
            callbacks_.ping_done(waiter.done, std::chrono::duration_cast<std::chrono::nanoseconds>(
                                                  now - waiter.sent_at));
        }
    }

    void finish(std::uint16_t code, std::string reason) {
        if (state_ == State::Closed) {
            return;
        }
        const bool we_closed = close_sent_ || state_ == State::Closing;
        state_ = State::Closed;
        open_ = false;
        deadline_.cancel();
        boost::system::error_code ignored;
        lowest().socket().close(ignored);
        // Each queued frame's size, not all of queued_bytes_: a send that raced the close has
        // counted its frame and still posts the enqueue that takes it off again.
        for (const auto& item : out_) {
            queued_bytes_ -= item.size;
        }
        // The frame being written stays until on_written: the write still reads its bytes after
        // the socket is closed (in a Debug build asio checks the string's iterators).
        out_.erase(out_.begin() + (writing_ && !out_.empty() ? 1 : 0), out_.end());
        auto pings = std::move(pings_);
        pings_.clear();
        for (auto& waiter : pings) {
            waiter.timer->cancel();
            callbacks_.ping_done(waiter.done, std::nullopt);
        }
        {
            std::lock_guard lock(facts_mutex_);
            close_code_ = code;
        }
        if (reason.empty() && we_closed && code != kAbnormal) {
            reason = close_reason_sent_.second;
        }
        callbacks_.closed(code, reason);
    }

    std::shared_ptr<ReactorImpl> reactor_;
    asio::strand<asio::io_context::executor_type> strand_;
    WsClientOptions options_;
    Callbacks callbacks_;
    WsUrl url_;
    std::shared_ptr<ssl::context> tls_;
    tcp::resolver resolver_;
    asio::steady_timer deadline_;
    websocket::stream<Stream> ws_;
    websocket::response_type response_;
    beast::flat_buffer buffer_;

    State state_ = State::Opening;
    std::atomic<bool> open_{false};
    std::atomic<bool> close_called_{false};
    std::deque<Outgoing> out_;
    bool writing_ = false;
    std::atomic<std::size_t> queued_bytes_{0};

    std::pair<std::uint16_t, std::string> close_reason_sent_{1000, {}};
    bool close_sent_ = false;
    bool control_busy_ = false;
    bool close_waiting_ = false;
    std::deque<PingWaiter> pings_;

    mutable std::mutex facts_mutex_;
    Peer local_;
    Peer remote_;
    std::optional<std::string> fingerprint_;
    std::optional<std::uint16_t> close_code_;
};

// What ws_connect returns: dropping it detaches the callbacks and closes the connection.
class ClientHandle final : public WsClientConnection {
public:
    explicit ClientHandle(std::shared_ptr<ClientCore> core) : core_(std::move(core)) {}
    ~ClientHandle() override { core_->detach(); }
    ClientHandle(const ClientHandle&) = delete;
    ClientHandle& operator=(const ClientHandle&) = delete;

    using WsConnection::send_text; // the timed send, WsConnection's default
    void send_text(std::shared_ptr<const std::string> text) override {
        core_->send_text(std::move(text));
    }
    void send_binary(std::vector<std::byte> data) override { core_->send_binary(std::move(data)); }
    void close(std::uint16_t code, std::string reason) override {
        core_->close(code, std::move(reason));
    }
    std::size_t queued_bytes() const override { return core_->queued_bytes(); }
    Peer local_endpoint() const override { return core_->local_endpoint(); }
    Peer remote_endpoint() const override { return core_->remote_endpoint(); }
    bool is_open() const override { return core_->is_open(); }
    std::optional<std::string> peer_certificate_sha256() const override {
        return core_->peer_certificate_sha256();
    }
    void ping(std::chrono::milliseconds timeout,
              std::function<void(std::optional<std::chrono::nanoseconds>)> done) override {
        core_->ping(timeout, std::move(done));
    }
    std::optional<std::uint16_t> close_code() const override { return core_->close_code(); }

private:
    std::shared_ptr<ClientCore> core_;
};

// A connection that cannot start (a malformed URL): it reports the failure on the reactor, as
// any other would, and is closed.
class FailedClient final : public ClientCore, public std::enable_shared_from_this<FailedClient> {
public:
    FailedClient(std::shared_ptr<ReactorImpl> reactor, WsClientOptions options,
                 WsConnectFailure failure)
        : reactor_(std::move(reactor)), callbacks_(options), failure_(std::move(failure)) {}

    void connect() override {
        if (reactor_->stopped()) {
            return;
        }
        asio::post(reactor_->io(),
                   [self = shared_from_this()] { self->callbacks_.failed(self->failure_); });
    }
    void detach() override { callbacks_.clear(); }
    using WsConnection::send_text;
    void send_text(std::shared_ptr<const std::string>) override {}
    void send_binary(std::vector<std::byte>) override {}
    void close(std::uint16_t, std::string) override {}
    std::size_t queued_bytes() const override { return 0; }
    bool is_open() const override { return false; }
    std::optional<std::string> peer_certificate_sha256() const override { return std::nullopt; }
    void ping(std::chrono::milliseconds,
              std::function<void(std::optional<std::chrono::nanoseconds>)> done) override {
        if (done) {
            done(std::nullopt);
        }
    }
    std::optional<std::uint16_t> close_code() const override { return kAbnormal; }

private:
    std::shared_ptr<ReactorImpl> reactor_;
    Callbacks callbacks_;
    WsConnectFailure failure_;
};

bool is_ip_literal(const std::string& host) {
    boost::system::error_code error;
    asio::ip::make_address(host, error);
    return !error;
}

} // namespace

std::string WsUrl::host_header() const {
    const bool v6 = host.find(':') != std::string::npos;
    std::string out = v6 ? "[" + host + "]" : host;
    if (port != (secure ? 443 : 80)) {
        out += ":" + std::to_string(port);
    }
    return out;
}

std::optional<WsUrl> parse_ws_url(std::string_view url) {
    WsUrl out;
    if (url.starts_with("wss://")) {
        out.secure = true;
        url.remove_prefix(6);
    } else if (url.starts_with("ws://")) {
        url.remove_prefix(5);
    } else {
        return std::nullopt;
    }
    const auto path_at = url.find_first_of("/?");
    std::string_view authority = url.substr(0, path_at);
    std::string_view rest = path_at == std::string_view::npos ? "" : url.substr(path_at);
    if (authority.find('@') != std::string_view::npos) {
        return std::nullopt;
    }
    std::string_view port_text;
    if (authority.starts_with("[")) {
        const auto close = authority.find(']');
        if (close == std::string_view::npos) {
            return std::nullopt;
        }
        out.host = std::string(authority.substr(1, close - 1));
        const auto after = authority.substr(close + 1);
        if (!after.empty()) {
            if (!after.starts_with(":")) {
                return std::nullopt;
            }
            port_text = after.substr(1);
        }
    } else {
        const auto colon = authority.find(':');
        out.host = std::string(authority.substr(0, colon));
        if (colon != std::string_view::npos) {
            port_text = authority.substr(colon + 1);
        }
    }
    if (out.host.empty()) {
        return std::nullopt;
    }
    out.port = out.secure ? 443 : 80;
    if (!port_text.empty()) {
        unsigned value = 0;
        const auto [end, error] =
            std::from_chars(port_text.data(), port_text.data() + port_text.size(), value);
        if (error != std::errc() || end != port_text.data() + port_text.size() || value == 0 ||
            value > 65535) {
            return std::nullopt;
        }
        out.port = static_cast<std::uint16_t>(value);
    }
    out.target =
        rest.empty() || rest.starts_with("?") ? "/" + std::string(rest) : std::string(rest);
    out.host_is_ip = is_ip_literal(out.host);
    return out;
}

std::unique_ptr<WsClientConnection> start_ws_client(const std::shared_ptr<ReactorImpl>& reactor,
                                                    WsClientOptions options) {
    std::shared_ptr<ClientCore> core;
    auto url = parse_ws_url(options.url);
    if (!url) {
        core = std::make_shared<FailedClient>(
            reactor, std::move(options),
            WsConnectFailure{Kind::Handshake, 0, "not a ws:// or wss:// URL"});
    } else if (url->secure) {
        core = std::make_shared<ClientSession<TlsStream>>(reactor, std::move(options),
                                                          std::move(*url), make_client_context());
    } else {
        core = std::make_shared<ClientSession<PlainStream>>(reactor, std::move(options),
                                                            std::move(*url), nullptr);
    }
    core->connect();
    return std::make_unique<ClientHandle>(std::move(core));
}

} // namespace fv::net::detail

namespace fv::net {

std::unique_ptr<WsClientConnection> ws_connect(Reactor& reactor, WsClientOptions options) {
    return detail::start_ws_client(detail::ReactorAccess::impl(reactor), std::move(options));
}

} // namespace fv::net
