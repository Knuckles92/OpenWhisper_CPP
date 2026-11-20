#include "net/detail/session.h"

#include <chrono>
#include <cstdio>
#include <exception>
#include <limits>
#include <stdexcept>
#include <utility>

#include "base/utf8.h"
#include "net/detail/server_log.h"
#include "net/detail/wire.h"
#include "net/detail/ws_session.h"

namespace fv::net::detail {

namespace {

// uvicorn's timeout_keep_alive: an idle keep-alive connection closes after it.
constexpr std::chrono::seconds kKeepAlive{5};
// FileResponse.chunk_size: the file is read this much at a time, as the socket takes it.
constexpr std::size_t kFileChunk = 64 * 1024;
// After our FIN, how long the rest of what the client sends is read and dropped, so that
// closing with unread data does not reset the connection before the client reads the answer.
constexpr std::chrono::seconds kDrain{2};
// The request line and headers together; beyond it the request is refused with 400.
constexpr std::uint32_t kHeaderLimit = 64 * 1024;
// asyncio's ssl_handshake_timeout, for a TLS server without a WebSocket open_timeout.
constexpr std::chrono::seconds kTlsHandshake{60};

constexpr std::string_view kInvalidRequest = "Invalid HTTP request received.";
constexpr std::string_view kInternalError = "Internal Server Error";

bool is_peer_reset(const boost::system::error_code& error) {
    return error == asio::error::connection_reset || error == asio::error::connection_aborted;
}

// Errors that end a connection without anything to answer or log, as asyncio's transports
// treat OSError (logged only in debug mode).
bool is_disconnect(const boost::system::error_code& error) {
    return error == http::error::end_of_stream || error == http::error::partial_message ||
           error == asio::error::eof || error == asio::error::operation_aborted ||
           error == boost::beast::error::timeout || error == asio::error::broken_pipe ||
           error == asio::error::not_connected || error == asio::error::shut_down ||
           error == asio::error::bad_descriptor || is_peer_reset(error);
}

Headers internal_error_headers() {
    return {{"content-type", "text/plain; charset=utf-8"},
            {"content-length", std::to_string(kInternalError.size())},
            {"connection", "close"}};
}

std::string hex_size(std::size_t size) {
    char text[2 * sizeof(std::size_t) + 1];
    std::snprintf(text, sizeof(text), "%zx", size);
    return text;
}

class ResponderImpl final : public Responder {
public:
    ResponderImpl(std::shared_ptr<HttpConnection> session, std::uint64_t request,
                  std::shared_ptr<const HttpRequest> http_request)
        : session_(std::move(session)), request_(request), http_request_(std::move(http_request)) {}

    ~ResponderImpl() override {
        if (!used_.exchange(true)) {
            log_no_response();
            session_->respond(request_, 500, internal_error_headers(), std::string(kInternalError));
        }
    }

    void send(int status, Headers headers, std::string body) override {
        if (used_.exchange(true)) {
            log_second_response();
            return;
        }
        session_->respond(request_, status, std::move(headers), std::move(body));
    }

    void send_file(std::filesystem::path path, std::string content_type,
                   std::optional<ByteRange> range) override {
        send_file(std::move(path), std::move(content_type), range, FileOptions{});
    }

    // Planned on the caller's thread (the stat and the open, as Starlette runs os.stat in a
    // worker thread); the session streams the body.
    void send_file(std::filesystem::path path, std::string content_type,
                   std::optional<ByteRange> range, FileOptions options) override {
        if (used_.exchange(true)) {
            log_second_response();
            return;
        }
        const auto facts = stat_file(path);
        std::ifstream file;
        if (facts) {
            file.open(path, std::ios::binary);
        }
        if (!facts || !file) {
            FilePlan missing = not_found_plan();
            session_->respond(request_, missing.status, std::move(missing.headers),
                              std::move(missing.body));
            return;
        }
        if (content_type.empty()) {
            // guess_type(filename or path): the download name decides when there is one.
            content_type = content_type_for(options.filename && !options.filename->empty()
                                                ? path_from_utf8(*options.filename)
                                                : path);
        }
        FilePlan plan = plan_file_response(*facts, content_type, range, options,
                                           http_request_->headers, random_boundary());
        std::shared_ptr<FileBody> body;
        if (!plan.pieces.empty()) {
            body = std::make_shared<FileBody>(std::move(file), std::move(plan.pieces));
        }
        session_->respond_file(request_, plan.status, std::move(plan.headers), std::move(plan.body),
                               std::move(body));
    }

    std::shared_ptr<EventStream> start_sse(Headers headers) override {
        auto stream = std::make_shared<SseStream>(session_, request_);
        if (used_.exchange(true)) {
            log_second_response();
            stream->mark_gone();
            return stream;
        }
        session_->start_sse(request_, std::move(headers), stream);
        return stream;
    }

    // After the handler threw: 500 unless it had answered.
    void fail() {
        if (!used_.exchange(true)) {
            session_->respond(request_, 500, internal_error_headers(), std::string(kInternalError));
        }
    }

private:
    std::shared_ptr<HttpConnection> session_;
    std::uint64_t request_;
    std::shared_ptr<const HttpRequest> http_request_;
    std::atomic<bool> used_{false};
};

} // namespace

SseStream::~SseStream() {
    // Dropped without close(): end it, as a Starlette response ends with its generator. A
    // stream that never went out (a second answer) or whose client left has nothing to end.
    if (!closed_ && !gone_) {
        session_->sse_end(request_);
    }
}

bool SseStream::write(std::string frame) {
    if (gone_ || closed_ || session_->reactor_stopped()) {
        return false;
    }
    session_->sse_write(request_, std::move(frame));
    return true;
}

void SseStream::close() {
    if (closed_.exchange(true)) {
        return;
    }
    session_->sse_end(request_);
}

std::size_t SseStream::queued_frames() const {
    return session_->queued_frames();
}

template <class Stream> bool Session<Stream>::reactor_stopped() const {
    return server_->reactor()->stopped();
}

template <class Stream>
Session<Stream>::Session(std::uint64_t id, std::shared_ptr<ServerImpl> server, Stream stream,
                         std::chrono::steady_clock::time_point accepted)
    : id_(id), server_(std::move(server)), stream_(std::move(stream)),
      drain_timer_(stream_.get_executor()), request_began_(accepted) {
    boost::system::error_code error;
    const auto remote = lowest().socket().remote_endpoint(error);
    if (!error) {
        peer_ = {remote.address().to_string(), remote.port()};
    }
    const auto local = lowest().socket().local_endpoint(error);
    if (!error) {
        local_ = {local.address().to_string(), local.port()};
    }
}

template <class Stream> Session<Stream>::~Session() {
    server_->session_closed(id_);
}

template <class Stream> void Session<Stream>::post(std::function<void()> work) {
    if (server_->reactor()->stopped()) {
        return;
    }
    asio::post(stream_.get_executor(),
               [self = this->shared_from_this(), work = std::move(work)] { work(); });
}

template <class Stream> void Session<Stream>::start() {
    post([this] {
        boost::system::error_code ignored;
        // Frames and small answers go out at once (asyncio sets it on every connection).
        lowest().socket().set_option(tcp::no_delay(true), ignored);
        if constexpr (kIsTls<Stream>) {
            handshake();
        } else {
            read_header();
        }
    });
}

template <class Stream> void Session<Stream>::handshake() {
    if constexpr (kIsTls<Stream>) {
        const auto open = server_->open_deadline();
        lowest().expires_at(request_began_ +
                            (open ? *open : std::chrono::milliseconds(kTlsHandshake)));
        stream_.async_handshake(
            ssl::stream_base::server,
            [self = this->shared_from_this()](const boost::system::error_code& error) {
                if (self->state_ != State::Reading) {
                    return;
                }
                if (error) {
                    // A client that is not speaking TLS, or gave up: asyncio
                    // logs a failed handshake in debug mode only.
                    self->close_now();
                    return;
                }
                self->read_header();
            });
    }
}

template <class Stream> void Session<Stream>::server_stopping() {
    post([this] {
        stopping_ = true;
        if (state_ == State::Reading && !reading_body_) {
            close_now(); // idle between requests, or nothing complete yet
        } else if (state_ == State::Reading || state_ == State::Handling) {
            close_after_ = true;
        }
    });
}

template <class Stream> void Session<Stream>::force_close() {
    post([this] { close_now(); });
}

template <class Stream> void Session<Stream>::read_header() {
    state_ = State::Reading;
    reading_body_ = false;
    parser_.emplace();
    parser_->header_limit(kHeaderLimit);
    // Checked against the route's cap once the route is known.
    parser_->body_limit((std::numeric_limits<std::uint64_t>::max)());
    if (!first_request_) {
        request_began_ = std::chrono::steady_clock::now();
        lowest().expires_after(kKeepAlive);
    } else if (const auto open = server_->open_deadline(); open && kIsTls<Stream>) {
        // websockets' open_timeout covers the first request of a TLS connection too.
        lowest().expires_at(request_began_ + *open);
    } else {
        lowest().expires_never();
    }
    http::async_read_header(
        stream_, buffer_, *parser_,
        [self = this->shared_from_this()](const boost::system::error_code& error, std::size_t) {
            self->on_header(error);
        });
}

template <class Stream> void Session<Stream>::on_header(const boost::system::error_code& error) {
    lowest().expires_never();
    if (state_ != State::Reading) {
        return;
    }
    if (error) {
        if (is_disconnect(error)) {
            close_now();
        } else {
            log_invalid_request();
            send_400();
        }
        return;
    }
    first_request_ = false;
    const auto& message = parser_->get();
    const auto text = [](const auto& view) {
        return std::string(view.data(), view.size());
    };
    request_ = std::make_shared<HttpRequest>();
    request_->method = text(message.method_string());
    const std::string target = text(message.target());
    const auto question = target.find('?');
    // uvicorn: the path before "?" unquoted; the query string as it came.
    request_->path = unquote(std::string_view(target).substr(0, question));
    raw_query_ = question == std::string::npos ? std::string() : target.substr(question + 1);
    request_->query = parse_query(raw_query_);
    for (const auto& field : message) {
        request_->headers.emplace_back(text(field.name_string()), text(field.value()));
    }
    request_->peer = peer_;
    request_->tls = kIsTls<Stream>;

    // uvicorn keeps HTTP/1.1 connections open unless the client says "Connection: close".
    keep_alive_ = message.version() >= 11;
    expect_continue_ = false;
    for (const auto& [name, value] : request_->headers) {
        if (iequals(name, "connection") && iequals(value, "close")) {
            keep_alive_ = false;
        } else if (iequals(name, "expect") && iequals(value, "100-continue")) {
            expect_continue_ = true;
        }
    }
    head_request_ = request_->method == "HEAD";

    if (boost::beast::websocket::is_upgrade(message)) {
        upgrade();
        return;
    }

    dispatch_ = server_->dispatch_for(request_->method, request_->path);
    const std::size_t max_body =
        dispatch_.handler ? dispatch_.options.max_body : RouteOptions{}.max_body;
    if (parser_->is_done()) {
        handle();
        return;
    }
    if (const auto length = parser_->content_length(); length && *length > max_body) {
        request_->body_too_large = true;
        close_after_ = true;
        handle();
        return;
    }
    if (expect_continue_) {
        enqueue("HTTP/1.1 100 Continue\r\n\r\n");
    }
    read_body(max_body);
}

template <class Stream> void Session<Stream>::read_body(std::size_t max_body) {
    reading_body_ = true;
    parser_->body_limit(static_cast<std::uint64_t>(max_body));
    http::async_read(
        stream_, buffer_, *parser_,
        [self = this->shared_from_this()](const boost::system::error_code& error, std::size_t) {
            if (self->state_ != State::Reading) {
                return;
            }
            if (error == http::error::body_limit) {
                // The streamed body went over the cap: the handler hears of it
                // before the rest arrives, and the connection closes after.
                self->request_->body_too_large = true;
                self->close_after_ = true;
            } else if (error) {
                self->close_now();
                return;
            } else {
                self->request_->body = std::move(self->parser_->get().body());
            }
            self->handle();
        });
}

template <class Stream> void Session<Stream>::handle() {
    state_ = State::Handling;
    reading_body_ = false;
    ++request_number_;
    if (dispatch_.redirect_path) {
        // Starlette's redirect_slashes: the URL as Starlette rebuilds it from the scope.
        std::string url;
        bool has_host = false;
        for (const auto& [name, value] : request_->headers) {
            if (iequals(name, "host")) {
                url = "http://" + value + *dispatch_.redirect_path;
                has_host = true;
                break;
            }
        }
        if (!has_host) {
            url = "http://" + local_.address +
                  (local_.port == 80 ? std::string() : ":" + std::to_string(local_.port)) +
                  *dispatch_.redirect_path;
        }
        if (!raw_query_.empty()) {
            url += "?" + raw_query_;
        }
        write_response(
            307, {{"content-length", "0"}, {"location", quote(url, ":/%#?=@[]!$&'()*+,;")}}, {});
        return;
    }
    auto responder =
        std::make_shared<ResponderImpl>(this->shared_from_this(), request_number_, request_);
    if (dispatch_.handler) {
        request_->params = dispatch_.params;
        auto call = [handler = dispatch_.handler, request = request_,
                     responder = std::move(responder)] {
            try {
                handler(*request, responder);
            } catch (const std::exception& error) {
                log_handler_exception(&error);
                responder->fail();
            } catch (...) {
                log_handler_exception(nullptr);
                responder->fail();
            }
        };
        if (dispatch_.options.run_on != nullptr) {
            dispatch_.options.run_on->post(std::move(call));
        } else {
            call();
        }
        return;
    }
    if (dispatch_.fallback) {
        try {
            dispatch_.fallback(*request_, dispatch_.path_known, responder);
        } catch (const std::exception& error) {
            log_handler_exception(&error);
            responder->fail();
        } catch (...) {
            log_handler_exception(nullptr);
            responder->fail();
        }
        return;
    }
    // FastAPI's answers to HTTPException(404) and HTTPException(405, headers={"Allow": ...}).
    if (dispatch_.path_known) {
        std::string allow;
        for (const auto& method : dispatch_.allowed) {
            allow += (allow.empty() ? "" : ", ") + method;
        }
        const std::string body = R"({"detail":"Method Not Allowed"})";
        responder->send(405,
                        {{"allow", allow},
                         {"content-length", std::to_string(body.size())},
                         {"content-type", "application/json"}},
                        body);
    } else {
        const std::string body = R"({"detail":"Not Found"})";
        responder->send(
            404,
            {{"content-length", std::to_string(body.size())}, {"content-type", "application/json"}},
            body);
    }
}

template <class Stream>
void Session<Stream>::respond(std::uint64_t request, int status, Headers headers,
                              std::string body) {
    post([this, request, status, headers = std::move(headers), body = std::move(body)]() mutable {
        if (request != request_number_ || state_ != State::Handling || finish_after_flush_) {
            return; // the connection moved on or closed: the answer has nowhere to go
        }
        write_response(status, std::move(headers), std::move(body));
    });
}

template <class Stream>
void Session<Stream>::respond_file(std::uint64_t request, int status, Headers headers,
                                   std::string body, std::shared_ptr<FileBody> file) {
    post([this, request, status, headers = std::move(headers), body = std::move(body),
          file = std::move(file)]() mutable {
        if (request != request_number_ || state_ != State::Handling || finish_after_flush_) {
            return;
        }
        bool closes = false;
        std::string bytes = response_head(status, headers, false, body.size(), closes);
        const bool bodiless = status < 200 || status == 204 || status == 304;
        if (!head_request_ && !bodiless) {
            if (file) {
                file_body_ = std::move(file);
            } else {
                bytes += body;
            }
        }
        close_after_ = closes;
        finish_after_flush_ = true;
        enqueue(std::move(bytes));
    });
}

template <class Stream> void Session<Stream>::pump_file() {
    auto chunk = file_body_->next(kFileChunk);
    if (!chunk) {
        // The file shrank or failed after its length went out: the answer cannot be finished.
        file_body_.reset();
        close_now();
        return;
    }
    if (chunk->empty()) {
        file_body_.reset();
        after_flush();
        return;
    }
    enqueue(std::move(*chunk));
}

template <class Stream> void Session<Stream>::upgrade() {
    auto handlers = server_->websocket_for(request_->path);
    if (!handlers) {
        if (auto fallback = server_->websocket_fallback()) {
            // websockets' process_request: an ordinary answer, then the connection closes.
            dispatch_ = Dispatch{};
            dispatch_.handler = std::move(fallback);
            keep_alive_ = false;
            close_after_ = true;
            handle();
            return;
        }
        // Starlette closes a WebSocket no route takes before accepting it; uvicorn answers 403.
        state_ = State::Handling;
        ++request_number_;
        keep_alive_ = false;
        close_after_ = true;
        head_request_ = false;
        write_response(403,
                       {{"connection", "close"},
                        {"content-length", "0"},
                        {"content-type", "text/plain; charset=utf-8"}},
                       {});
        return;
    }
    if (stopping_ || server_->stopping()) {
        close_now();
        return;
    }
    std::optional<std::chrono::steady_clock::time_point> open_by;
    if (handlers->open_timeout.count() > 0) {
        open_by = request_began_ + handlers->open_timeout;
    }
    // The socket now belongs to the WebSocket; this session ends without closing it.
    state_ = State::Closed;
    drain_timer_.cancel();
    lowest().expires_never();
    auto session = std::make_shared<WsSession<Stream>>(
        id_, server_, std::move(stream_), parser_->release(), request_, std::move(*handlers),
        Peer{local_.address, local_.port}, peer_, open_by);
    server_->ws_opened(id_, session);
    server_->session_closed(id_);
    session->start();
}

template <class Stream>
std::string Session<Stream>::response_head(int status, const Headers& headers, bool streaming,
                                           std::size_t body_size, bool& closes) const {
    std::string head = "HTTP/1.1 " + std::to_string(status) + " ";
    head += status_phrase(status);
    head += "\r\ndate: " + http_date(std::chrono::system_clock::now()) + "\r\n";
    bool has_length = false;
    bool has_close = false;
    bool has_encoding = false;
    for (const auto& [name, value] : headers) {
        const std::string lowered = ascii_lower(name);
        if (lowered == "content-length") {
            has_length = true;
        } else if (lowered == "transfer-encoding") {
            has_encoding = true;
        } else if (lowered == "connection" && iequals(value, "close")) {
            has_close = true;
        }
        head += lowered + ": " + value + "\r\n";
    }
    closes = has_close || !keep_alive_ || close_after_ || stopping_;
    const bool bodiless = status < 200 || status == 204 || status == 304;
    if (streaming) {
        if (!has_length && !has_encoding && !head_request_ && !bodiless) {
            head += "transfer-encoding: chunked\r\n";
        }
    } else if (!has_length && !bodiless) {
        head += "content-length: " + std::to_string(body_size) + "\r\n";
    }
    if (closes && !has_close) {
        head += "connection: close\r\n";
    }
    head += "\r\n";
    return head;
}

template <class Stream>
void Session<Stream>::write_response(int status, Headers headers, std::string body) {
    bool closes = false;
    std::string bytes = response_head(status, headers, false, body.size(), closes);
    const bool bodiless = status < 200 || status == 204 || status == 304;
    if (!head_request_ && !bodiless) {
        bytes += body;
    }
    close_after_ = closes;
    finish_after_flush_ = true;
    enqueue(std::move(bytes));
}

template <class Stream> void Session<Stream>::send_400() {
    state_ = State::Handling;
    ++request_number_;
    keep_alive_ = false;
    close_after_ = true;
    head_request_ = false;
    write_response(400, {{"content-type", "text/plain; charset=utf-8"}},
                   std::string(kInvalidRequest));
}

template <class Stream>
void Session<Stream>::start_sse(std::uint64_t request, Headers headers,
                                std::shared_ptr<SseStream> stream) {
    post([this, request, headers = std::move(headers), stream = std::move(stream)]() mutable {
        if (request != request_number_ || state_ != State::Handling || finish_after_flush_) {
            stream->mark_gone();
            return;
        }
        bool closes = false;
        std::string head = response_head(200, headers, true, 0, closes);
        close_after_ = true;
        if (head_request_) {
            // HEAD gets the head alone; frames have no body to go into.
            stream->mark_gone();
            finish_after_flush_ = true;
            enqueue(std::move(head));
            return;
        }
        state_ = State::Streaming;
        sse_ = stream;
        enqueue(std::move(head));
        watch_peer();
    });
}

template <class Stream> void Session<Stream>::sse_write(std::uint64_t request, std::string frame) {
    if (frame.empty()) {
        return; // an empty chunk would end the stream; uvicorn sends nothing for it
    }
    ++queued_frames_; // counted at once, so a burst of writes shows before the strand runs
    post([this, request, frame = std::move(frame)] {
        if (request != request_number_ || state_ != State::Streaming || finish_after_flush_) {
            --queued_frames_;
            return;
        }
        enqueue(hex_size(frame.size()) + "\r\n" + frame + "\r\n", true);
    });
}

template <class Stream> void Session<Stream>::sse_end(std::uint64_t request) {
    post([this, request] {
        if (request != request_number_ || state_ != State::Streaming || finish_after_flush_) {
            return;
        }
        enqueue("0\r\n\r\n");
        finish_after_flush_ = true;
    });
}

template <class Stream> void Session<Stream>::enqueue(std::string bytes, bool frame) {
    out_.push_back({std::move(bytes), frame});
    if (!writing_) {
        write_next();
    }
}

template <class Stream> void Session<Stream>::write_next() {
    writing_ = true;
    asio::async_write(
        stream_, asio::buffer(out_.front().bytes),
        [self = this->shared_from_this()](const boost::system::error_code& error, std::size_t) {
            self->writing_ = false;
            if (error || self->state_ == State::Closed) {
                self->close_now();
                return;
            }
            if (self->out_.front().frame) {
                --self->queued_frames_;
            }
            self->out_.pop_front();
            if (!self->out_.empty()) {
                self->write_next();
            } else if (self->file_body_) {
                self->pump_file();
            } else {
                self->after_flush();
            }
        });
}

template <class Stream> void Session<Stream>::after_flush() {
    if (!finish_after_flush_) {
        return;
    }
    finish_after_flush_ = false;
    if (state_ == State::Streaming) {
        release_stream();
        close_gracefully();
        return;
    }
    if (state_ != State::Handling) {
        return;
    }
    if (close_after_) {
        close_gracefully();
    } else {
        read_header();
    }
}

template <class Stream> void Session<Stream>::watch_peer() {
    // A pending read while streaming: it completes when the client goes away, which is how
    // EventStream::write learns to return false.
    stream_.async_read_some(
        asio::buffer(scratch_),
        [self = this->shared_from_this()](const boost::system::error_code& error, std::size_t) {
            if (self->state_ == State::Closing) {
                if (error) {
                    self->close_now();
                } else {
                    self->drain();
                }
                return;
            }
            if (self->state_ != State::Streaming) {
                return;
            }
            if (error) {
                self->close_now();
                return;
            }
            self->watch_peer();
        });
}

template <class Stream> void Session<Stream>::close_gracefully() {
    if (state_ == State::Closing || state_ == State::Closed) {
        return;
    }
    const bool read_pending = state_ == State::Streaming;
    state_ = State::Closing;
    release_stream();
    boost::system::error_code error;
    lowest().socket().shutdown(tcp::socket::shutdown_send, error);
    if (error) {
        log_connection_error(ErrorSite::Closing, is_peer_reset(error), error.message());
        close_now();
        return;
    }
    drain_timer_.expires_after(kDrain);
    drain_timer_.async_wait(
        [self = this->shared_from_this()](const boost::system::error_code& error) {
            if (!error) {
                self->close_now();
            }
        });
    if (!read_pending) {
        drain();
    }
}

template <class Stream> void Session<Stream>::drain() {
    stream_.async_read_some(
        asio::buffer(scratch_),
        [self = this->shared_from_this()](const boost::system::error_code& error, std::size_t) {
            if (self->state_ != State::Closing) {
                return;
            }
            if (error) {
                self->close_now();
            } else {
                self->drain();
            }
        });
}

template <class Stream> void Session<Stream>::close_now() {
    if (state_ == State::Closed) {
        return;
    }
    state_ = State::Closed;
    release_stream();
    file_body_.reset();
    drain_timer_.cancel();
    boost::system::error_code ignored;
    lowest().socket().close(ignored);
    server_->session_closed(id_);
}

template <class Stream> void Session<Stream>::release_stream() {
    if (auto stream = sse_.lock()) {
        stream->mark_gone();
    }
    sse_.reset();
}

template class Session<PlainStream>;
template class Session<TlsStream>;

} // namespace fv::net::detail
