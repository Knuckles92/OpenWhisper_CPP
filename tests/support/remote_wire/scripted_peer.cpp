#include "remote_wire/scripted_peer.h"

#include <condition_variable>
#include <deque>
#include <map>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <utility>

#include "base/identity.h"
#include "net/http_server.h"
#include "net/tls.h"
#include "net/ws_client.h"

namespace fv::testing::remote_wire {

namespace {

// OpenWhisper's host: max(MAX_REQUEST_BYTES, MAX_REPLY_BYTES), max_queue=2 (host.py:456-469).
constexpr std::size_t kMaxMessageBytes = 8u << 20;
// websockets pauses its reader when more than max_queue=2 messages wait, so the third unreleased
// one stops reading; WsHandlers pauses at n unreleased (open-items: M9.6). It resumes once one
// is released, where websockets waits for the queue to empty; ScriptedPeer releases at once
// unless told to stop reading, so the difference never shows here.
constexpr std::size_t kMaxQueuedMessages = 3;

std::string where(const Transcript& transcript, int conn) {
    return transcript.scenario + " conn " + std::to_string(conn);
}

} // namespace

struct ScriptedPeer::State {
    Transcript transcript;
    Options options;
    std::string fingerprint;
    std::unique_ptr<net::HttpServer> server;
    std::uint16_t port = 0;

    mutable std::recursive_mutex mutex;
    mutable std::condition_variable_any changed;
    Json bindings = Json::object();
    std::vector<int> plan; // recorded connections, in order
    std::size_t next = 0;  // the next one to play
    std::size_t accepted = 0;
    std::size_t finished = 0;
    std::vector<std::string> mismatches;
    std::vector<WireFrame> received;

    struct Session {
        int conn = -1;
        std::vector<std::size_t> frames;
        std::size_t at = 0;
        std::size_t delivered = 0;
        bool done = false;
        std::shared_ptr<net::WsConnection> socket;
    };
    std::map<const net::WsConnection*, Session> sessions;

    // The next recorded connection, or nullopt (a mismatch) when every one was played.
    std::optional<int> take() {
        ++accepted;
        if (next >= plan.size()) {
            mismatches.push_back(transcript.scenario + ": connection " + std::to_string(accepted) +
                                 " was not recorded (" + std::to_string(plan.size()) + " were)");
            return std::nullopt;
        }
        return plan[next++];
    }

    void finish(Session& session) {
        if (!session.done) {
            session.done = true;
            ++finished;
            changed.notify_all();
        }
    }

    const WireFrame& frame(const Session& session) const {
        return transcript.frames[session.frames[session.at]];
    }

    // Sends every host frame due before the next client frame.
    void pump(Session& session) {
        // close() may report on_close on this thread, which ends the session under us.
        while (session.socket && session.at < session.frames.size() &&
               frame(session).dir == "s2c") {
            const std::size_t index = session.frames[session.at];
            const WireFrame& recorded = transcript.frames[index];
            ++session.at;
            if (recorded.kind == "close") {
                if (!recorded.echo) {
                    session.socket->close(static_cast<std::uint16_t>(recorded.code),
                                          recorded.reason);
                }
                continue;
            }
            WireFrame out;
            try {
                out = render(transcript, index, bindings);
            } catch (const std::exception& error) {
                // A frame that cannot be played is a fault of the transcript, reported as such
                // rather than as the connection the server would close for a throwing handler.
                mismatches.push_back(where(transcript, session.conn) + ": frame " +
                                     std::to_string(index) + " cannot be played: " + error.what());
                session.socket->close(1011, "transcript frame cannot be played");
                break;
            }
            if (out.kind == "text") {
                session.socket->send_text(std::make_shared<const std::string>(out.text));
            } else if (out.kind == "binary") {
                session.socket->send_binary(wire_bytes(out));
            } else {
                mismatches.push_back(where(transcript, session.conn) + ": frame " +
                                     std::to_string(index) +
                                     " is an HTTP answer inside a "
                                     "WebSocket connection");
            }
        }
        changed.notify_all();
    }

    void arrived(const std::shared_ptr<net::WsConnection>& socket, WireFrame actual) {
        std::lock_guard lock(mutex);
        auto it = sessions.find(socket.get());
        if (it == sessions.end()) {
            return;
        }
        Session& session = it->second;
        actual.dir = "c2s";
        actual.conn = session.conn;
        received.push_back(actual);
        if (session.at >= session.frames.size()) {
            mismatches.push_back(where(transcript, session.conn) + ": a " + actual.kind +
                                 " frame after the last recorded one");
        } else if (const auto& recorded = frame(session); recorded.kind == "close") {
            mismatches.push_back(where(transcript, session.conn) + ": a " + actual.kind +
                                 " frame where the client closed (" +
                                 std::to_string(recorded.code) + ")");
        } else {
            if (auto diff = compare(transcript, session.frames[session.at], actual, bindings,
                                    kRecordedProduct);
                !diff.empty()) {
                mismatches.push_back(std::move(diff));
            }
            ++session.at;
            pump(session);
        }
        ++session.delivered;
        if (options.stop_reading_after == 0 || session.delivered <= options.stop_reading_after) {
            socket->release_message();
        }
    }

    void closed(const net::WsConnection* socket, std::uint16_t code, std::string_view reason) {
        std::lock_guard lock(mutex);
        auto it = sessions.find(socket);
        if (it == sessions.end()) {
            return;
        }
        Session& session = it->second;
        if (session.done) {
            return;
        }
        WireFrame actual = close_frame("c2s", code, std::string(reason));
        actual.conn = session.conn;
        received.push_back(actual);
        // The server reports the code it closed with when it closed first: the client's echo
        // carries nothing more to compare.
        while (session.at < session.frames.size()) {
            const std::size_t index = session.frames[session.at];
            const WireFrame& recorded = transcript.frames[index];
            if (recorded.kind != "close") {
                mismatches.push_back(where(transcript, session.conn) + ": closed " +
                                     std::to_string(code) + " \"" + std::string(reason) +
                                     "\" with frame " + std::to_string(index) + " (" +
                                     recorded.dir + " " + recorded.kind + ") still to come");
                break;
            }
            if (recorded.dir == "c2s" && !recorded.echo) {
                if (auto diff = compare(transcript, index, actual, bindings, kRecordedProduct);
                    !diff.empty()) {
                    mismatches.push_back(std::move(diff));
                }
            }
            ++session.at;
        }
        session.socket.reset();
        finish(session);
    }

    void opened(std::shared_ptr<net::WsConnection> socket) {
        std::lock_guard lock(mutex);
        const auto conn = take();
        if (!conn) {
            socket->close(1011, "not recorded");
            return;
        }
        // A connection may take the address of one that ended: start its session afresh.
        Session& session = sessions[socket.get()];
        session = Session{};
        session.conn = *conn;
        session.frames = frames_of(transcript, *conn);
        session.socket = std::move(socket);
        if (!session.frames.empty() && frame(session).kind == "http") {
            mismatches.push_back(where(transcript, *conn) +
                                 ": the client upgraded where the record has a refused upgrade");
            session.socket->close(1011, "expected a refused upgrade");
            return;
        }
        pump(session);
    }

    void refused(const net::HttpRequest& request,
                 const std::shared_ptr<net::Responder>& responder) {
        std::lock_guard lock(mutex);
        const auto conn = take();
        if (!conn) {
            responder->send(404, {}, "");
            return;
        }
        const auto indexes = frames_of(transcript, *conn);
        const WireFrame* answer = nullptr;
        for (std::size_t index : indexes) {
            const WireFrame& recorded = transcript.frames[index];
            if (recorded.kind != "http") {
                break;
            }
            if (recorded.dir == "c2s") {
                WireFrame actual = http_frame("c2s", 0, "");
                actual.text = request.path;
                actual.conn = *conn;
                received.push_back(actual);
                if (auto diff = compare(transcript, index, actual, bindings); !diff.empty()) {
                    mismatches.push_back(std::move(diff));
                }
            } else {
                answer = &recorded;
            }
        }
        ++finished;
        changed.notify_all();
        if (answer == nullptr) {
            mismatches.push_back(where(transcript, *conn) + ": the client asked for " +
                                 request.path + " where the record has a WebSocket connection");
            responder->send(404, {}, "");
            return;
        }
        responder->send(answer->status, {{"content-type", "text/plain; charset=utf-8"}},
                        answer->body);
    }
};

ScriptedPeer::ScriptedPeer(net::Reactor& reactor, net::TlsIdentity identity,
                           const Transcript& transcript)
    : ScriptedPeer(reactor, std::move(identity), transcript, Options{}) {}

ScriptedPeer::ScriptedPeer(net::Reactor& reactor, net::TlsIdentity identity,
                           const Transcript& transcript, Options options)
    : state_(std::make_shared<State>()) {
    auto& state = *state_;
    state.transcript = transcript;
    state.options = std::move(options);
    state.fingerprint = net::certificate_sha256_from_pem(identity.cert_pem);
    state.plan = remote_wire::connections(transcript);
    // Every certificate a host frame names is this peer's.
    for (const auto& frame : transcript.frames) {
        if (frame.dir == "s2c") {
            for (const auto& name : placeholders_in(frame.text)) {
                if (name.starts_with("<fingerprint#")) {
                    state.bindings[name] = state.fingerprint;
                }
            }
        }
    }

    state.server = net::HttpServer::create(reactor);
    std::weak_ptr<State> weak = state_;
    net::WsHandlers handlers;
    handlers.permessage_deflate = false;
    handlers.max_message_bytes = kMaxMessageBytes;
    handlers.open_timeout = std::chrono::milliseconds(10'000);
    handlers.ping_interval = state.options.ping_interval;
    handlers.ping_timeout = state.options.ping_timeout;
    handlers.max_queued_messages = kMaxQueuedMessages;
    handlers.on_open = [weak](std::shared_ptr<net::WsConnection> socket, const net::HttpRequest&) {
        if (auto self = weak.lock()) {
            self->opened(std::move(socket));
        }
    };
    handlers.on_text = [weak](const std::shared_ptr<net::WsConnection>& socket,
                              std::string_view text) {
        if (auto self = weak.lock()) {
            self->arrived(socket, text_frame("c2s", std::string(text)));
        }
    };
    handlers.on_binary = [weak](const std::shared_ptr<net::WsConnection>& socket,
                                std::span<const std::byte> data) {
        if (auto self = weak.lock()) {
            self->arrived(socket, binary_frame("c2s", data));
        }
    };
    handlers.on_close = [weak](const std::shared_ptr<net::WsConnection>& socket, std::uint16_t code,
                               std::string_view reason) {
        if (auto self = weak.lock()) {
            self->closed(socket.get(), code, reason);
        }
    };
    state.server->websocket(state.options.path, std::move(handlers));
    state.server->set_websocket_fallback(
        [weak](const net::HttpRequest& request, std::shared_ptr<net::Responder> responder) {
            if (auto self = weak.lock()) {
                self->refused(request, responder);
            } else {
                responder->send(404, {}, "");
            }
        });
    auto endpoint = state.server->listen(
        net::Listen{.address = "127.0.0.1", .port = 0, .tls = std::move(identity)});
    if (!endpoint) {
        throw std::runtime_error("ScriptedPeer could not listen: " + endpoint.error());
    }
    state.port = endpoint->port;
}

ScriptedPeer::~ScriptedPeer() {
    stop();
}

void ScriptedPeer::stop() {
    std::unique_ptr<net::HttpServer> server;
    {
        std::lock_guard lock(state_->mutex);
        server = std::move(state_->server);
    }
    if (server) {
        server->stop(std::chrono::milliseconds(0));
    }
}

std::uint16_t ScriptedPeer::port() const {
    return state_->port;
}

std::string ScriptedPeer::fingerprint() const {
    return state_->fingerprint;
}

std::vector<std::string> ScriptedPeer::mismatches() const {
    std::lock_guard lock(state_->mutex);
    return state_->mismatches;
}

std::vector<WireFrame> ScriptedPeer::received() const {
    std::lock_guard lock(state_->mutex);
    return state_->received;
}

std::size_t ScriptedPeer::connections() const {
    std::lock_guard lock(state_->mutex);
    return state_->accepted;
}

bool ScriptedPeer::wait_finished(std::chrono::milliseconds timeout) const {
    std::unique_lock lock(state_->mutex);
    return state_->changed.wait_for(lock, timeout,
                                    [&] { return state_->finished >= state_->plan.size(); });
}

Json ScriptedPeer::bindings() const {
    std::lock_guard lock(state_->mutex);
    return state_->bindings;
}

namespace {

// What one ws_connect connection reported, in order.
struct Events {
    struct Event {
        enum class Kind { open, message, close, failed } kind;
        WireFrame frame;
        std::string detail;
    };
    std::mutex mutex;
    std::condition_variable changed;
    std::deque<Event> queue;

    void push(Event event) {
        {
            std::lock_guard lock(mutex);
            queue.push_back(std::move(event));
        }
        changed.notify_all();
    }
    std::optional<Event> pop(std::chrono::milliseconds timeout) {
        std::unique_lock lock(mutex);
        if (!changed.wait_for(lock, timeout, [&] { return !queue.empty(); })) {
            return std::nullopt;
        }
        Event event = std::move(queue.front());
        queue.pop_front();
        return event;
    }
};

std::string failure_text(const net::WsConnectFailure& failure) {
    return "the connection did not open (" + failure.detail + ", HTTP " +
           std::to_string(failure.http_status) + ")";
}

} // namespace

std::vector<std::string> play_client(net::Reactor& reactor, std::uint16_t port,
                                     const Transcript& transcript, int conn, Json& bindings,
                                     const ReplayOptions& options) {
    const std::string product =
        options.product.empty() ? std::string(identity().product) : options.product;
    const auto indexes = frames_of(transcript, conn);
    std::vector<std::string> out;
    if (indexes.empty()) {
        out.push_back(where(transcript, conn) + " has no frames");
        return out;
    }
    std::string path(kEnginePath);
    const WireFrame& first = transcript.frames[indexes.front()];
    const bool refused = first.kind == "http";
    if (refused && first.dir == "c2s") {
        path = first.text;
    }

    auto events = std::make_shared<Events>();
    using Kind = Events::Event::Kind;
    net::WsClientOptions client;
    client.url = "wss://127.0.0.1:" + std::to_string(port) + path;
    client.connect_timeout = options.timeout;
    client.on_open = [events] {
        events->push({Kind::open, {}, {}});
    };
    client.on_text = [events](std::string_view text) {
        events->push({Kind::message, text_frame("s2c", std::string(text)), {}});
    };
    client.on_binary = [events](std::span<const std::byte> data) {
        events->push({Kind::message, binary_frame("s2c", data), {}});
    };
    client.on_close = [events](std::uint16_t code, std::string_view reason) {
        events->push({Kind::close, close_frame("s2c", code, std::string(reason)), {}});
    };
    client.on_connect_failed = [events](const net::WsConnectFailure& failure) {
        Events::Event event{Kind::failed, http_frame("s2c", failure.http_status, ""),
                            failure_text(failure)};
        events->push(std::move(event));
    };
    auto socket = net::ws_connect(reactor, std::move(client));

    const auto opened = events->pop(options.timeout);
    if (!opened) {
        out.push_back(where(transcript, conn) + ": the connection neither opened nor failed");
        return out;
    }
    if (refused) {
        for (std::size_t index : indexes) {
            const WireFrame& recorded = transcript.frames[index];
            if (recorded.dir != "s2c") {
                continue;
            }
            if (opened->kind != Kind::failed) {
                out.push_back(where(transcript, conn) + ": the upgrade was accepted");
            } else if (auto diff = compare(transcript, index, opened->frame, bindings, product);
                       !diff.empty()) {
                out.push_back(std::move(diff));
            }
        }
        return out;
    }
    if (opened->kind != Kind::open) {
        out.push_back(where(transcript, conn) + ": " + opened->detail);
        return out;
    }

    bool closed = false;
    for (std::size_t index : indexes) {
        const WireFrame& recorded = transcript.frames[index];
        if (recorded.dir == "c2s") {
            if (recorded.kind == "close") {
                if (!recorded.echo && !closed) {
                    socket->close(static_cast<std::uint16_t>(recorded.code), recorded.reason);
                }
                continue;
            }
            if (closed) {
                out.push_back(where(transcript, conn) + ": closed before frame " +
                              std::to_string(index) + " could be sent");
                break;
            }
            WireFrame frame = render(transcript, index, bindings);
            if (frame.kind == "text") {
                socket->send_text(std::make_shared<const std::string>(frame.text));
            } else {
                socket->send_binary(wire_bytes(frame));
            }
            continue;
        }
        if (closed) {
            continue; // the close came; an echo of ours is not reported again
        }
        const auto event = events->pop(options.timeout);
        if (!event) {
            out.push_back(transcript.scenario + " frame " + std::to_string(index) + " (" +
                          recorded.kind + "): nothing arrived within " +
                          std::to_string(options.timeout.count()) + " ms");
            break;
        }
        if (event->kind == Kind::close) {
            closed = true;
        }
        if (auto diff = compare(transcript, index, event->frame, bindings, product);
            !diff.empty()) {
            out.push_back(std::move(diff));
            if (event->kind == Kind::close && recorded.kind != "close") {
                break;
            }
        }
    }
    if (!closed) {
        socket->close(1000, "");
    }
    return out;
}

} // namespace fv::testing::remote_wire
