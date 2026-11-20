#include "net/detail/ws_session.h"

#include <algorithm>
#include <exception>
#include <mutex>
#include <random>
#include <utility>

#include "net/detail/server_log.h"
#include "net/detail/wire.h"

namespace fv::net::detail {

namespace {

// uvicorn's ws_ping_interval and ws_ping_timeout, unless a test shortened them.
std::mutex keepalive_mutex;
WsKeepalive keepalive_setting;

// uvicorn's close_timeout: how long a closing handshake waits for the peer's close frame.
constexpr std::chrono::seconds kCloseTimeout{10};
// After a keepalive timeout the peer is not answering; uvicorn drops it at once.
constexpr std::chrono::seconds kFailWait{1};

// Close codes (RFC 6455 7.4.1).
constexpr std::uint16_t kServiceRestart = 1012; // uvicorn's shutdown
constexpr std::uint16_t kNoStatus = 1005;       // connection lost after the handshake (uvicorn)
constexpr std::uint16_t kProtocol = 1002;
constexpr std::uint16_t kBadPayload = 1007; // text that is not UTF-8
constexpr std::uint16_t kTooBig = 1009;
constexpr std::uint16_t kServerError = 1011;

// The code Beast sent when it failed the connection on a read error, or nullopt for a lost
// connection.
std::optional<std::uint16_t> failure_code(const boost::system::error_code& error) {
    if (error == websocket::error::message_too_big || error == websocket::error::buffer_overflow) {
        return kTooBig;
    }
    if (error == websocket::error::bad_frame_payload) {
        return kBadPayload;
    }
    if (error.category() == websocket::make_error_code(websocket::error::closed).category()) {
        return kProtocol;
    }
    return std::nullopt;
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

} // namespace

WsKeepalive ws_keepalive() {
    std::lock_guard lock(keepalive_mutex);
    return keepalive_setting;
}

void set_ws_keepalive(WsKeepalive keepalive) {
    std::lock_guard lock(keepalive_mutex);
    keepalive_setting = keepalive;
}

template <class Stream>
WsSession<Stream>::WsSession(std::uint64_t id, std::shared_ptr<ServerImpl> server, Stream stream,
                             http::request<http::string_body> upgrade,
                             std::shared_ptr<const HttpRequest> request, WsHandlers handlers,
                             Peer local, Peer remote,
                             std::optional<std::chrono::steady_clock::time_point> open_by)
    : id_(id), server_(std::move(server)), ws_(std::move(stream)), upgrade_(std::move(upgrade)),
      request_(std::move(request)), handlers_(std::move(handlers)), keepalive_(ws_keepalive()),
      local_(std::move(local)), remote_(std::move(remote)), open_by_(open_by),
      close_timer_(ws_.get_executor()), ping_timer_(ws_.get_executor()),
      pong_timer_(ws_.get_executor()) {
    if (handlers_.ping_interval.count() > 0) {
        keepalive_.interval = handlers_.ping_interval;
    }
    if (handlers_.ping_timeout.count() > 0) {
        keepalive_.timeout = handlers_.ping_timeout;
    }
}

template <class Stream> void WsSession<Stream>::set_close_timeout() {
    // The closing handshake waits this long for the peer's close frame; keepalive is ours.
    websocket::stream_base::timeout timeouts;
    timeouts.handshake_timeout = kCloseTimeout;
    timeouts.idle_timeout = websocket::stream_base::none();
    timeouts.keep_alive_pings = false;
    ws_.set_option(timeouts);
}

template <class Stream> void WsSession<Stream>::release_message() {
    post([this] {
        if (unreleased_ > 0) {
            --unreleased_;
        }
        if (read_paused_ && unreleased_ < handlers_.max_queued_messages &&
            state_ != State::Closed) {
            read_paused_ = false;
            read_next();
        }
    });
}

template <class Stream> WsSession<Stream>::~WsSession() {
    server_->ws_closed(id_);
}

template <class Stream> bool WsSession<Stream>::post(std::function<void()> work) {
    if (server_->reactor()->stopped()) {
        return false;
    }
    asio::post(ws_.get_executor(),
               [self = this->shared_from_this(), work = std::move(work)] { work(); });
    return true;
}

template <class Stream> void WsSession<Stream>::start() {
    post([this] {
        if (server_->stopping()) {
            finish(kNoStatus, {});
            return;
        }
        beast::get_lowest_layer(ws_).expires_never();
        if (open_by_) {
            const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(
                *open_by_ - std::chrono::steady_clock::now());
            if (left.count() <= 0) {
                finish(kNoStatus, {}); // websockets drops a late handshake without an answer
                return;
            }
            // Beast's handshake timeout bounds the 101; the close handshake gets it back after.
            websocket::stream_base::timeout timeouts;
            timeouts.handshake_timeout = left;
            timeouts.idle_timeout = websocket::stream_base::none();
            timeouts.keep_alive_pings = false;
            ws_.set_option(timeouts);
        } else {
            set_close_timeout();
        }
        ws_.set_option(websocket::stream_base::decorator([](websocket::response_type& response) {
            response.erase(http::field::server);
            response.set(http::field::date, http_date(std::chrono::system_clock::now()));
        }));
        if (handlers_.permessage_deflate) {
            // uvicorn offers permessage-deflate with 12-bit windows and memLevel 5.
            websocket::permessage_deflate deflate;
            deflate.server_enable = true;
            deflate.server_max_window_bits = 12;
            deflate.client_max_window_bits = 12;
            deflate.memLevel = 5;
            ws_.set_option(deflate);
        }
        // websockets sends each message as one frame.
        ws_.auto_fragment(false);
        ws_.read_message_max(handlers_.max_message_bytes);
        ws_.control_callback([this](websocket::frame_type kind, boost::beast::string_view payload) {
            on_control(kind, payload);
        });
        ws_.async_accept(upgrade_,
                         [self = this->shared_from_this()](const boost::system::error_code& error) {
                             self->on_accept(error);
                         });
    });
}

template <class Stream> void WsSession<Stream>::on_accept(const boost::system::error_code& error) {
    if (state_ != State::Accepting) {
        return;
    }
    if (error) {
        // Beast has answered a request that was no handshake (400 or 426); nothing was opened.
        finish(kNoStatus, {});
        return;
    }
    if (open_by_) {
        set_close_timeout();
    }
    state_ = State::Open;
    opened_ = true;
    open_ = true;
    if (handlers_.on_open) {
        try {
            handlers_.on_open(this->shared_from_this(), *request_);
        } catch (const std::exception& failure) {
            handler_failed(&failure);
            return;
        } catch (...) {
            handler_failed(nullptr);
            return;
        }
    }
    if (state_ == State::Closed) {
        return;
    }
    read_next();
    if (state_ == State::Open) {
        schedule_ping(keepalive_.interval);
    }
}

template <class Stream> void WsSession<Stream>::read_next() {
    ws_.async_read(read_buffer_,
                   [self = this->shared_from_this()](const boost::system::error_code& error,
                                                     std::size_t) { self->on_read(error); });
}

template <class Stream> void WsSession<Stream>::on_read(const boost::system::error_code& error) {
    if (state_ == State::Closed) {
        return;
    }
    // A Python client drops TLS without close_notify after the closing handshake (SSLSocket's
    // shutdown), which OpenSSL reports as a truncated stream once the close frame was read.
    const bool after_close_frame = error && ws_.reason().code != websocket::close_code::none &&
                                   !failure_code(error) && error != beast::error::timeout;
    if (error) {
        if (error == websocket::error::closed || after_close_frame) {
            // The closing handshake is done: ours when we began it, else the peer's.
            if (close_sent_ || state_ == State::Closing) {
                finish(close_code_, close_reason_);
            } else {
                // A close frame without a status reads as 1005, as websockets reports it.
                const auto& reason = ws_.reason();
                const auto code = static_cast<std::uint16_t>(reason.code);
                finish(code == 0 ? kNoStatus : code,
                       std::string(reason.reason.data(), reason.reason.size()));
            }
        } else if (const auto code = failure_code(error)) {
            finish(*code, {});
        } else {
            finish(state_ == State::Closing ? close_code_ : kNoStatus,
                   state_ == State::Closing ? close_reason_ : std::string());
        }
        return;
    }
    const auto data = read_buffer_.data();
    // After close() the application no longer receives; reads go on until the peer's close.
    if (state_ == State::Open) {
        try {
            if (ws_.got_text()) {
                if (handlers_.on_text) {
                    ++unreleased_;
                    handlers_.on_text(
                        this->shared_from_this(),
                        std::string_view(static_cast<const char*>(data.data()), data.size()));
                }
            } else if (handlers_.on_binary) {
                ++unreleased_;
                handlers_.on_binary(this->shared_from_this(),
                                    std::span<const std::byte>(
                                        static_cast<const std::byte*>(data.data()), data.size()));
            }
        } catch (const std::exception& failure) {
            read_buffer_.consume(read_buffer_.size());
            handler_failed(&failure);
            return;
        } catch (...) {
            read_buffer_.consume(read_buffer_.size());
            handler_failed(nullptr);
            return;
        }
    }
    read_buffer_.consume(read_buffer_.size());
    if (state_ == State::Closed) {
        return;
    }
    if (handlers_.max_queued_messages > 0 && unreleased_ >= handlers_.max_queued_messages) {
        // Nothing reads the socket until a release: the peer's frames wait in TCP's buffers.
        read_paused_ = true;
        return;
    }
    read_next();
}

template <class Stream> void WsSession<Stream>::send_text(std::shared_ptr<const std::string> text) {
    send_text(std::move(text), std::chrono::milliseconds(0), nullptr);
}

template <class Stream>
void WsSession<Stream>::send_text(std::shared_ptr<const std::string> text,
                                  std::chrono::milliseconds timeout,
                                  std::function<void(bool timed_out)> done) {
    if (!text) {
        text = std::make_shared<const std::string>();
    }
    const std::size_t size = text->size();
    // The posted work owns `done`; this one is called only when nothing was posted.
    auto refused = std::make_shared<std::function<void(bool)>>(std::move(done));
    Outgoing item;
    item.size = size;
    item.text = std::move(text);
    item.is_text = true;
    queued_bytes_ += size; // counted at once, so a burst shows before the strand runs
    if (!post([this, item = std::move(item), refused, timeout]() mutable {
            item.done = std::move(*refused);
            enqueue(std::move(item), timeout);
        })) {
        // Nothing runs on a stopped reactor (it may have stopped since the frame was counted):
        // the frame is dropped now, not left counted.
        queued_bytes_ -= size;
        if (*refused) {
            (*refused)(false);
        }
    }
}

template <class Stream> void WsSession<Stream>::send_binary(std::vector<std::byte> data) {
    const std::size_t size = data.size();
    Outgoing item;
    item.size = size;
    item.binary = std::move(data);
    item.is_text = false;
    queued_bytes_ += size;
    if (!post([this, item = std::move(item)]() mutable {
            enqueue(std::move(item), std::chrono::milliseconds(0));
        })) {
        queued_bytes_ -= size;
    }
}

template <class Stream>
void WsSession<Stream>::enqueue(Outgoing item, std::chrono::milliseconds timeout) {
    if (state_ != State::Open) {
        // Closed, closing or not open yet: uvicorn refuses a send then; the frame is dropped.
        queued_bytes_ -= item.size;
        if (item.done) {
            item.done(false);
        }
        return;
    }
    item.number = next_number_++;
    if (timeout.count() > 0 && item.done) {
        item.timer = std::make_shared<asio::steady_timer>(ws_.get_executor(), timeout);
        item.timer->async_wait([self = this->shared_from_this(),
                                number = item.number](const boost::system::error_code& error) {
            if (!error) {
                self->expire(number);
            }
        });
    }
    out_.push_back(std::move(item));
    if (!writing_) {
        write_next();
    }
}

template <class Stream> void WsSession<Stream>::expire(std::uint64_t number) {
    const auto it = std::ranges::find(out_, number, &Outgoing::number);
    if (it == out_.end()) {
        return; // already written
    }
    auto done = std::exchange(it->done, nullptr);
    if (!(writing_ && it == out_.begin())) {
        // Not started: dropped, as a cancelled send never reaches the transport.
        queued_bytes_ -= it->size;
        out_.erase(it);
    }
    if (done) {
        done(true);
    }
}

template <class Stream> void WsSession<Stream>::write_next() {
    if (out_.empty() || state_ == State::Closed) {
        return;
    }
    writing_ = true;
    Outgoing& item = out_.front();
    ws_.text(item.is_text);
    auto buffer = item.is_text ? asio::buffer(*item.text) : asio::buffer(item.binary);
    ws_.async_write(buffer,
                    [self = this->shared_from_this()](const boost::system::error_code& error,
                                                      std::size_t) { self->on_written(error); });
}

template <class Stream> void WsSession<Stream>::on_written(const boost::system::error_code& error) {
    writing_ = false;
    if (state_ == State::Closed) {
        // finish counted and completed every frame and kept only this one, for the write: it
        // goes now, so a later drop_queued or expire cannot subtract its size again.
        out_.clear();
        return;
    }
    if (!out_.empty()) {
        Outgoing item = std::move(out_.front());
        out_.pop_front();
        queued_bytes_ -= item.size;
        if (item.timer) {
            item.timer->cancel();
        }
        if (error) {
            if (item.done) {
                item.done(false);
            }
            finish(state_ == State::Closing ? close_code_ : kNoStatus,
                   state_ == State::Closing ? close_reason_ : std::string());
            return;
        }
        if (item.done) {
            item.done(false);
        }
    }
    if (state_ == State::Closed) {
        return;
    }
    if (!out_.empty()) {
        write_next();
    } else if (state_ == State::Closing && !close_sent_) {
        send_close();
    }
}

template <class Stream> void WsSession<Stream>::drop_queued() {
    // The frame being written stays: cutting it short would break the stream.
    const std::size_t keep = writing_ ? 1 : 0;
    std::vector<std::function<void(bool)>> dropped;
    while (out_.size() > keep) {
        Outgoing item = std::move(out_.back());
        out_.pop_back();
        queued_bytes_ -= item.size;
        if (item.timer) {
            item.timer->cancel();
        }
        if (item.done) {
            dropped.push_back(std::move(item.done));
        }
    }
    for (auto it = dropped.rbegin(); it != dropped.rend(); ++it) {
        (*it)(false);
    }
}

template <class Stream> void WsSession<Stream>::close(std::uint16_t code, std::string reason) {
    post([this, code, reason = std::move(reason)]() mutable {
        if (state_ == State::Accepting) {
            return;
        }
        begin_close(code, std::move(reason), kCloseTimeout);
    });
}

template <class Stream>
void WsSession<Stream>::begin_close(std::uint16_t code, std::string reason,
                                    std::chrono::milliseconds wait) {
    if (state_ != State::Open) {
        return;
    }
    state_ = State::Closing;
    open_ = false;
    close_code_ = code;
    close_reason_ = std::move(reason);
    ping_timer_.cancel();
    pong_timer_.cancel();
    // A peer that never answers, or never reads the frames before the close, is cut off.
    close_timer_.expires_after(wait);
    close_timer_.async_wait(
        [self = this->shared_from_this()](const boost::system::error_code& error) {
            if (!error) {
                self->finish(self->close_code_, self->close_reason_);
            }
        });
    if (!writing_ && out_.empty()) {
        send_close();
    }
}

template <class Stream> void WsSession<Stream>::send_close() {
    if (close_sent_ || state_ != State::Closing) {
        return;
    }
    if (control_busy_) {
        close_waiting_ = true;
        return;
    }
    close_sent_ = true;
    control_busy_ = true;
    // A close frame carries at most 123 bytes of reason.
    websocket::close_reason reason(
        static_cast<websocket::close_code>(close_code_),
        std::string_view(close_reason_).substr(0, websocket::reason_string::static_capacity));
    ws_.async_close(reason, [self = this->shared_from_this()](const boost::system::error_code&) {
        self->control_busy_ = false;
        self->finish(self->close_code_, self->close_reason_);
    });
}

template <class Stream>
void WsSession<Stream>::schedule_ping(std::chrono::steady_clock::duration delay) {
    ping_timer_.expires_after(std::max(delay, std::chrono::steady_clock::duration::zero()));
    ping_timer_.async_wait(
        [self = this->shared_from_this()](const boost::system::error_code& error) {
            if (!error) {
                self->send_ping();
            }
        });
}

template <class Stream> void WsSession<Stream>::send_ping() {
    if (state_ != State::Open || control_busy_) {
        return;
    }
    pending_ping_ = random_ping_payload();
    ping_sent_at_ = std::chrono::steady_clock::now();
    control_busy_ = true;
    websocket::ping_data payload(pending_ping_->data(), pending_ping_->size());
    ws_.async_ping(payload, [self = this->shared_from_this()](const boost::system::error_code&) {
        self->control_busy_ = false;
        if (self->close_waiting_) {
            self->close_waiting_ = false;
            self->send_close();
        }
    });
    pong_timer_.expires_after(keepalive_.timeout);
    pong_timer_.async_wait(
        [self = this->shared_from_this()](const boost::system::error_code& error) {
            if (error || self->state_ != State::Open || !self->pending_ping_) {
                return;
            }
            self->pending_ping_.reset();
            self->drop_queued();
            self->begin_close(kServerError, "keepalive ping timeout", kFailWait);
        });
}

template <class Stream>
void WsSession<Stream>::on_control(websocket::frame_type kind, boost::beast::string_view payload) {
    if (kind != websocket::frame_type::pong || !pending_ping_ ||
        std::string_view(payload.data(), payload.size()) != *pending_ping_) {
        return; // unsolicited or stale
    }
    last_rtt_ = std::chrono::steady_clock::now() - ping_sent_at_;
    pending_ping_.reset();
    pong_timer_.cancel();
    if (state_ == State::Open) {
        schedule_ping(keepalive_.interval - last_rtt_);
    }
}

template <class Stream> void WsSession<Stream>::server_stopping() {
    post([this] {
        if (state_ == State::Accepting) {
            finish(kNoStatus, {});
            return;
        }
        drop_queued();
        begin_close(kServiceRestart, {}, kCloseTimeout);
    });
}

template <class Stream> void WsSession<Stream>::force_close() {
    post([this] {
        if (state_ == State::Closing) {
            finish(close_code_, close_reason_);
        } else {
            finish(kNoStatus, {});
        }
    });
}

template <class Stream> void WsSession<Stream>::handler_failed(const std::exception* error) {
    log_handler_exception(error);
    finish(state_ == State::Closing ? close_code_ : kNoStatus,
           state_ == State::Closing ? close_reason_ : std::string());
}

template <class Stream> void WsSession<Stream>::finish(std::uint16_t code, std::string reason) {
    if (state_ == State::Closed) {
        return;
    }
    state_ = State::Closed;
    open_ = false;
    boost::system::error_code ignored;
    close_timer_.cancel();
    ping_timer_.cancel();
    pong_timer_.cancel();
    beast::get_lowest_layer(ws_).socket().close(ignored);
    std::vector<std::function<void(bool)>> dropped;
    for (auto& item : out_) {
        queued_bytes_ -= item.size;
        if (item.timer) {
            item.timer->cancel();
        }
        if (item.done) {
            // Emptied, not moved from: the kept frame's callback must never run twice.
            dropped.push_back(std::exchange(item.done, nullptr));
        }
    }
    // The frame being written stays until on_written: the write still reads its bytes after the
    // socket is closed (in a Debug build asio checks the string's iterators and fails fast).
    out_.erase(out_.begin() + (writing_ && !out_.empty() ? 1 : 0), out_.end());
    for (auto& done : dropped) {
        done(false);
    }
    if (opened_ && handlers_.on_close) {
        try {
            handlers_.on_close(this->shared_from_this(), code, reason);
        } catch (const std::exception& failure) {
            log_handler_exception(&failure);
        } catch (...) {
            log_handler_exception(nullptr);
        }
    }
    server_->ws_closed(id_);
}

template class WsSession<PlainStream>;
template class WsSession<TlsStream>;

} // namespace fv::net::detail
