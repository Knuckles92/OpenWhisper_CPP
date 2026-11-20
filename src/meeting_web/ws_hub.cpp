#include "meeting_web/ws_hub.h"

#include <algorithm>
#include <deque>
#include <exception>
#include <format>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include "base/log.h"
#include "base/subscription.h"
#include "meeting_state/store.h"
#include "meeting_web/auth.h"
#include "meeting_web/detail/py_value.h"
#include "meeting_web/voice_help.h"
#include "meeting_web/ws_protocol.h"
#include "pyport/text.h"

namespace fv::meeting_web {

namespace {

constexpr log::Logger logger("meeting.web.ws");

// uvicorn's ws_max_size and websockets' max_queue, the limits Python's hub ran under.
constexpr std::size_t kMaxFrameBytes = 16 * 1024 * 1024;
constexpr std::size_t kMaxQueuedMessages = 32;
// Python's asyncio.to_thread pool is shared by every socket; four workers serve the dashboard's
// handful of sockets, each of which runs one task at a time.
constexpr int kWorkerThreads = 4;

using Text = std::shared_ptr<const std::string>;

Text share(const Json& message) {
    return std::make_shared<const std::string>(encode(message));
}

bool is_text(const Json& value, std::string_view text) {
    return value.is_string() && value.get_ref<const std::string&>() == text;
}

// Starlette's query_params.get: the last value of a repeated name.
std::optional<std::string> query_value(const net::Query& query, std::string_view name) {
    std::optional<std::string> found;
    for (const auto& [key, value] : query) {
        if (key == name) {
            found = value;
        }
    }
    return found;
}

struct Inbound {
    bool binary = false;
    std::string text;
};

// One dashboard socket (ws.py _Connection plus the receive loop's place).
struct Conn {
    enum class Phase { Handshake, Serving, Done };

    std::shared_ptr<net::WsConnection> ws;
    Phase phase = Phase::Handshake;
    std::string role;
    std::string name;
    std::optional<std::string> participant_id;
    // The joined guest, announced when it leaves; null for the host.
    Json participant;
    bool registered = false;
    bool ready = false;
    std::deque<Text> pending;
    bool overflowed = false;
    int send_timeouts = 0;
    int inflight = 0;
    // Frames received and not yet handled; `busy` while one is being handled.
    std::deque<Inbound> inbox;
    bool busy = false;
    // The connection has ended (on_close arrived).
    bool closed = false;
};

using ConnPtr = std::shared_ptr<Conn>;

// The hub whose fan-out this thread is running, holding that hub's lock. A timed send may
// complete on the calling thread (a stopped reactor drops the frame at once, and
// WsConnection's default timed send completes at once); its outcome then waits for the fan-out
// to end instead of taking the lock a second time.
thread_local const void* t_fanout_hub = nullptr;

} // namespace

namespace detail {

class HubCore : public std::enable_shared_from_this<HubCore> {
public:
    HubCore(net::Reactor& reactor, DashboardEngine& engine,
            std::shared_ptr<meeting_state::MeetingRepository> repository)
        : reactor_(reactor), engine_(engine), repository_(std::move(repository)) {}

    void attach_worker(Executor* worker) {
        std::lock_guard lock(mutex_);
        worker_ = worker;
    }

    // The hub is gone: nothing runs any more and late reactor callbacks do nothing.
    void retire() {
        std::lock_guard lock(mutex_);
        alive_ = false;
        worker_ = nullptr;
        registry_.clear();
        attached_.clear();
    }

    void startup() {
        {
            std::lock_guard lock(mutex_);
            running_ = true;
        }
        ensure_store_subscription();
    }

    void shutdown() {
        Subscription ended;
        {
            std::lock_guard lock(subscription_mutex_);
            ended = std::move(subscription_);
            subscribed_store_.reset();
        }
        ended.disconnect();
        std::lock_guard lock(mutex_);
        running_ = false;
    }

    // ---- WebSocket callbacks (reactor) ----

    void on_open(const std::shared_ptr<net::WsConnection>& ws, const net::HttpRequest& request) {
        auto conn = std::make_shared<Conn>();
        conn->ws = ws;
        std::string token = query_value(request.query, "token").value_or("");
        std::string name = py::strip(query_value(request.query, "name").value_or(""));
        auto guest_id = clean_guest_id(query_value(request.query, "guest_id"));
        {
            std::unique_lock lock(mutex_);
            if (!alive_) {
                lock.unlock();
                ws->close(kWsCloseServerError, {});
                return;
            }
            attached_[ws.get()] = conn;
        }
        const bool posted = post_work(
            [self = shared_from_this(), conn, token = std::move(token), name = std::move(name),
             guest_id = std::move(guest_id)] { self->handshake(conn, token, name, guest_id); });
        if (!posted) {
            ws->close(kWsCloseServerError, {});
        }
    }

    void on_message(const std::shared_ptr<net::WsConnection>& ws, Inbound message) {
        std::unique_lock lock(mutex_);
        const auto found = attached_.find(ws.get());
        if (found == attached_.end() || found->second->phase == Conn::Phase::Done ||
            found->second->closed) {
            lock.unlock();
            ws->release_message();
            return;
        }
        const ConnPtr conn = found->second;
        conn->inbox.push_back(std::move(message));
        pump_locked(conn);
    }

    void on_close(const std::shared_ptr<net::WsConnection>& ws) {
        std::lock_guard lock(mutex_);
        const auto found = attached_.find(ws.get());
        if (found == attached_.end()) {
            return;
        }
        const ConnPtr conn = found->second;
        attached_.erase(found);
        conn->closed = true;
        if (conn->phase == Conn::Phase::Serving) {
            pump_locked(conn);
        }
    }

    // ---- Broadcasts ----

    void broadcast(const Json& message, bool host_only) {
        {
            std::lock_guard lock(mutex_);
            if (!alive_ || !running_ || registry_.empty()) {
                return;
            }
        }
        Text text = share(message);
        std::lock_guard lock(mutex_);
        if (!alive_ || !running_) {
            return;
        }
        fanout_locked(text, host_only);
    }

    void invalidate(bool resync) {
        std::lock_guard lock(mutex_);
        if (!alive_ || !running_) {
            return;
        }
        if (resync) {
            close_all_locked(kWsCloseResync);
            return;
        }
        // Give the regenerating REST response time to deliver the new host link.
        auto timer = reactor_.executor().post_after(kRotationCloseDelay, [weak = weak_from_this()] {
            if (auto self = weak.lock()) {
                std::lock_guard inner(self->mutex_);
                if (self->alive_ && self->running_) {
                    self->close_all_locked(kWsCloseUnauthorized);
                }
            }
        });
        timer.detach();
    }

    void on_store_batch(std::int64_t seq, const std::vector<meeting_state::OpResult>& applied) {
        for (const auto& item : split_store_batch(seq, applied)) {
            broadcast(item.message, item.host_only);
        }
    }

    // ---- Providers ----

    void set_guest_url_provider(std::function<std::string()> provider) {
        std::lock_guard lock(providers_mutex_);
        guest_url_ = std::move(provider);
    }

    void set_voice_commands(std::function<Json()> provider) {
        std::lock_guard lock(providers_mutex_);
        voice_commands_ = std::move(provider);
    }

    Json voice_commands() const {
        std::function<Json()> provider;
        {
            std::lock_guard lock(providers_mutex_);
            provider = voice_commands_;
        }
        return provider ? provider() : voice_command_guide(Json::object());
    }

    std::size_t connection_count() const {
        std::lock_guard lock(mutex_);
        return registry_.size();
    }

private:
    bool post_work(Task task) {
        std::lock_guard lock(mutex_);
        if (!alive_ || worker_ == nullptr) {
            return false;
        }
        worker_->post(std::move(task));
        return true;
    }

    // broadcast_json: ready sockets get the frame now; sockets still in their hello buffer it.
    void fanout_locked(const Text& text, bool host_only) {
        if (registry_.empty()) {
            return;
        }
        std::vector<ConnPtr> live;
        for (const auto& conn : registry_) {
            if (host_only && conn->role != kRoleHost) {
                continue;
            }
            if (conn->ready) {
                live.push_back(conn);
            } else if (host_only || conn->overflowed) {
                continue;
            } else if (conn->pending.size() >= kMaxPendingMessages) {
                conn->overflowed = true;
                conn->pending.clear();
            } else {
                conn->pending.push_back(text);
            }
        }
        const void* outer = std::exchange(t_fanout_hub, this);
        for (const auto& conn : live) {
            send_guarded_locked(conn, text);
        }
        t_fanout_hub = outer;
        for (const auto& [conn, timed_out] : std::exchange(completed_inline_, {})) {
            send_done_locked(conn, timed_out);
        }
    }

    void send_guarded_locked(const ConnPtr& conn, const Text& text) {
        conn->ws->send_text(text, kBroadcastSendTimeout,
                            [weak = weak_from_this(), conn](bool timed_out) {
                                auto self = weak.lock();
                                if (!self) {
                                    return;
                                }
                                if (t_fanout_hub == self.get()) {
                                    // Completed inside send_text: this thread holds the lock.
                                    self->completed_inline_.emplace_back(conn, timed_out);
                                    return;
                                }
                                self->send_done(conn, timed_out);
                            });
    }

    void send_done(const ConnPtr& conn, bool timed_out) {
        std::lock_guard lock(mutex_);
        send_done_locked(conn, timed_out);
    }

    void send_done_locked(const ConnPtr& conn, bool timed_out) {
        if (!alive_ || !conn->registered) {
            return;
        }
        if (timed_out) {
            ++conn->send_timeouts;
            logger.warning("Dashboard socket stalled ({} consecutive timeouts)",
                           conn->send_timeouts);
            if (conn->send_timeouts >= kMaxSendTimeouts) {
                drop_locked(conn, "send timed out repeatedly");
            }
        } else if (!conn->ws->is_open()) {
            drop_locked(conn, "send failed");
        } else {
            conn->send_timeouts = 0;
        }
    }

    void drop_locked(const ConnPtr& conn, std::string_view reason) {
        unregister_locked(conn);
        logger.info("Dropping dashboard socket: {}", reason);
        conn->ws->close(kWsCloseServerError, {});
    }

    void unregister_locked(const ConnPtr& conn) {
        if (!conn->registered) {
            return;
        }
        conn->registered = false;
        std::erase(registry_, conn);
    }

    void close_all_locked(std::uint16_t code) {
        const auto sockets = std::move(registry_);
        registry_.clear();
        for (const auto& conn : sockets) {
            conn->registered = false;
        }
        for (const auto& conn : sockets) {
            conn->ws->close(code, {});
        }
    }

    // ---- The store subscription ----

    // _ensure_store_subscription: the store can be created after the server starts, so this
    // runs on every connect; a replaced store's subscription ends.
    void ensure_store_subscription() {
        const auto store = engine_.store();
        std::lock_guard lock(subscription_mutex_);
        if (!store || store == subscribed_store_) {
            return;
        }
        subscription_.disconnect();
        subscription_ = store->subscribe(
            [weak = weak_from_this()](std::int64_t seq,
                                      const std::vector<meeting_state::OpResult>& applied) {
                if (auto self = weak.lock()) {
                    self->on_store_batch(seq, applied);
                }
            });
        subscribed_store_ = store;
    }

    // ---- One connection: handshake, messages, end (worker threads) ----

    void handshake(const ConnPtr& conn, const std::string& token, const std::string& name,
                   const std::optional<std::string>& guest_id) {
        ensure_store_subscription();
        const std::string meeting_id = engine_.meeting_id();
        const auto store = engine_.store();
        std::optional<Json> meeting;
        try {
            if (!meeting_id.empty()) {
                meeting = repository_->get_meeting(meeting_id);
            }
        } catch (const std::exception& error) {
            logger.error_with(error,
                              std::format("Dashboard handshake failed (meeting {})", meeting_id));
            refuse(conn, kWsCloseServerError);
            return;
        }
        if (!meeting || !store) {
            refuse(conn, kWsCloseUnauthorized);
            return;
        }
        const auto role = resolve_stored_role(token, detail::get(*meeting, "host_token"),
                                              detail::get(*meeting, "guest_token"));
        {
            std::unique_lock lock(mutex_);
            std::optional<std::uint16_t> refusal;
            if (!role) {
                refusal = kWsCloseUnauthorized;
            } else if (*role == kRoleGuest && name.empty()) {
                refusal = kWsCloseNameRequired;
            } else if (registry_.size() >= kMaxConnections) {
                logger.warning("Refusing dashboard connection: {} already served",
                               registry_.size());
                refusal = kWsCloseTooMany;
            }
            if (refusal || conn->closed) {
                conn->phase = Conn::Phase::Done;
                lock.unlock();
                if (refusal) {
                    conn->ws->close(*refusal, {});
                }
                return;
            }
            // Registered before any further read: every patch raised while the snapshot and
            // segments are read lands in `pending` instead of falling between the snapshot and
            // the first live patch.
            conn->role = *role;
            conn->name = name;
            conn->registered = true;
            registry_.push_back(conn);
        }

        Text hello;
        try {
            identify(conn, guest_id, name);
            hello = share(build_hello(*conn, *store, *meeting, meeting_id));
        } catch (const std::exception& error) {
            logger.error_with(error,
                              std::format("Dashboard handshake failed (meeting {})", meeting_id));
            {
                std::lock_guard lock(mutex_);
                unregister_locked(conn);
                conn->phase = Conn::Phase::Done;
            }
            conn->ws->close(kWsCloseServerError, {});
            return;
        }

        std::lock_guard lock(mutex_);
        if (!conn->registered || conn->closed) {
            // Invalidated, dropped or gone while the hello was assembled.
            unregister_locked(conn);
            conn->phase = Conn::Phase::Done;
            logger.debug("Dashboard socket ended before its hello (meeting {})", meeting_id);
            return;
        }
        conn->ws->send_text(hello);
        if (conn->overflowed) {
            logger.warning("Pre-hello buffer overflowed; asking client to resync");
            unregister_locked(conn);
            conn->phase = Conn::Phase::Done;
            conn->ws->close(kWsCloseResync, {});
            return;
        }
        while (!conn->pending.empty()) {
            conn->ws->send_text(conn->pending.front());
            conn->pending.pop_front();
        }
        // Nothing can slip in between the drain and the flag: both happen under the lock the
        // fan-out takes.
        conn->ready = true;
        conn->phase = Conn::Phase::Serving;
        if (!conn->participant.is_null()) {
            fanout_locked(share(presence("joined", conn->participant)), false);
        }
        pump_locked(conn);
    }

    void refuse(const ConnPtr& conn, std::uint16_t code) {
        {
            std::lock_guard lock(mutex_);
            conn->phase = Conn::Phase::Done;
        }
        conn->ws->close(code, {});
    }

    // _identify: the host's "me" participant, or the joining guest.
    void identify(const ConnPtr& conn, const std::optional<std::string>& guest_id,
                  const std::string& name) {
        std::optional<std::string> participant_id;
        Json participant;
        if (conn->role != kRoleGuest) {
            participant_id = host_participant_id();
        } else {
            participant = join_guest(guest_id, name);
            const Json& id = detail::get(participant, "id");
            if (id.is_string()) {
                participant_id = id.get<std::string>();
            }
        }
        std::lock_guard lock(mutex_);
        conn->participant_id = std::move(participant_id);
        conn->participant = std::move(participant);
    }

    std::optional<std::string> host_participant_id() const {
        const auto store = engine_.store();
        if (!store) {
            return std::nullopt;
        }
        const Json state = store->snapshot();
        const Json& participants = detail::get(state, "participants");
        if (!participants.is_object()) {
            return std::nullopt;
        }
        for (const auto& [id, participant] : participants.items()) {
            if (is_text(detail::get(participant, "kind"), "me")) {
                return id;
            }
        }
        return std::nullopt;
    }

    // _join_guest: a guest key that rejoins with the same name reuses its participant, so a
    // reconnect storm cannot mint participants without bound; a changed name or an unknown key
    // joins anew.
    Json join_guest(const std::optional<std::string>& guest_id, const std::string& name) {
        if (guest_id) {
            std::optional<std::pair<std::string, std::string>> known;
            {
                std::lock_guard lock(guests_mutex_);
                if (const auto found = guest_participants_.find(*guest_id);
                    found != guest_participants_.end()) {
                    known = found->second;
                }
            }
            if (known && known->second == name) {
                if (auto existing = participant_snapshot(known->first)) {
                    return std::move(*existing);
                }
            }
        }
        Json participant = engine_.add_guest(name);
        const Json& id = detail::get(participant, "id");
        if (guest_id && id.is_string() && !id.get_ref<const std::string&>().empty()) {
            std::lock_guard lock(guests_mutex_);
            guest_participants_[*guest_id] = {id.get<std::string>(), name};
        }
        return participant;
    }

    // The participant as the live state has it now.
    std::optional<Json> participant_snapshot(const std::optional<std::string>& participant_id) {
        if (!participant_id || participant_id->empty()) {
            return std::nullopt;
        }
        const auto store = engine_.store();
        if (!store) {
            return std::nullopt;
        }
        try {
            const Json state = store->snapshot();
            const Json& participants = detail::get(state, "participants");
            if (!participants.is_object()) {
                return std::nullopt;
            }
            const Json& found = detail::get(participants, *participant_id);
            if (found.is_null()) {
                return std::nullopt;
            }
            return std::optional<Json>(found);
        } catch (const std::exception&) {
            logger.warning("Could not read participant from the state snapshot");
            return std::nullopt;
        }
    }

    std::string guest_url_for(const Json& meeting) {
        std::function<std::string()> provider;
        {
            std::lock_guard lock(providers_mutex_);
            provider = guest_url_;
        }
        if (provider) {
            try {
                std::string url = provider();
                if (!url.empty()) {
                    return url;
                }
            } catch (const std::exception& error) {
                logger.error_with(error, "guest_url provider raised");
            }
        }
        const auto token = meeting.find("guest_token");
        return "/m/" + (token == meeting.end() ? std::string() : detail::py_str(*token));
    }

    Json recent_agent_activity() const {
        Json records;
        try {
            records = engine_.recent_agent_activity();
        } catch (const std::exception& error) {
            logger.error_with(error, "Failed to read recent agent activity");
            return Json::array();
        }
        if (!records.is_array()) {
            return Json::array();
        }
        if (records.size() <= kHelloActivityCount) {
            return records;
        }
        Json tail = Json::array();
        for (std::size_t i = records.size() - kHelloActivityCount; i < records.size(); ++i) {
            tail.push_back(records[i]);
        }
        return tail;
    }

    Json build_hello(const Conn& conn, meeting_state::MeetingStateStore& store, const Json& meeting,
                     const std::string& meeting_id) {
        Json state = store.snapshot();
        Json segments = Json::array();
        try {
            for (auto& segment : repository_->get_last_segments(meeting_id, kHelloSegmentCount)) {
                segments.push_back(std::move(segment));
            }
        } catch (const std::exception& error) {
            logger.error_with(error, "Failed to load hello segments");
            segments = Json::array();
        }
        const std::string guest_url = guest_url_for(meeting);
        Json guide = voice_commands();
        if (conn.role != kRoleHost && state.is_object()) {
            // Same rule as the live fan-out: omitted entirely, not sent empty.
            state.erase("custom_reports");
        }
        const auto seq = state.is_object() && state.contains("seq") ? state.at("seq") : Json(0);
        Json title = detail::py_or(
            detail::py_or(detail::get(state, "title"), detail::get(meeting, "title")), Json(""));
        Json status = detail::py_or(detail::get(state, "status"), detail::get(meeting, "status"));
        Json payload = {
            {"type", "hello"},
            {"voice_commands", std::move(guide)},
            {"role", conn.role},
            {"participant_id", conn.participant_id ? Json(*conn.participant_id) : Json()},
            {"seq", seq},
            {"state", std::move(state)},
            {"segments", std::move(segments)},
            {"urls", Json{{"guest", guest_url}}},
            {"meeting", Json{{"id", detail::get(meeting, "id")},
                             {"title", std::move(title)},
                             {"started_at", detail::get(meeting, "started_at")},
                             {"status", std::move(status)}}},
        };
        if (conn.role == kRoleHost) {
            // Host-only, like the live fan-out: a guest never learns what the agent is doing.
            payload["agent_activity"] = recent_agent_activity();
        }
        return payload;
    }

    // Starts handling the next received frame, or ends the connection once it has closed.
    void pump_locked(const ConnPtr& conn) {
        if (conn->busy || conn->phase != Conn::Phase::Serving) {
            return;
        }
        if (conn->closed) {
            finish_locked(conn);
            return;
        }
        if (conn->inbox.empty()) {
            return;
        }
        conn->busy = true;
        Inbound message = std::move(conn->inbox.front());
        conn->inbox.pop_front();
        if (!alive_ || worker_ == nullptr) {
            return;
        }
        worker_->post([self = shared_from_this(), conn, message = std::move(message)] {
            try {
                self->handle_frame(*conn, message);
            } catch (const std::exception& error) {
                // The receive loop's own failure (not a handler's, which is answered with an
                // error message): the connection ends, as ws.py's _serve ends it.
                logger.error_with(error, std::format("Dashboard connection failed (meeting {})",
                                                     self->engine_.meeting_id()));
                conn->ws->close(kWsCloseNormal, {});
            }
            conn->ws->release_message();
            std::lock_guard lock(self->mutex_);
            conn->busy = false;
            self->pump_locked(conn);
        });
    }

    // The receive loop's end: unregister, announce a guest's departure, close 1000.
    void finish_locked(const ConnPtr& conn) {
        conn->phase = Conn::Phase::Done;
        unregister_locked(conn);
        for (std::size_t i = 0; i < conn->inbox.size(); ++i) {
            conn->ws->release_message();
        }
        conn->inbox.clear();
        if (!conn->participant.is_null() && alive_ && worker_ != nullptr) {
            worker_->post([self = shared_from_this(), conn] { self->announce_left(*conn); });
        }
        conn->ws->close(kWsCloseNormal, {});
    }

    void announce_left(const Conn& conn) {
        try {
            const auto current = participant_snapshot(conn.participant_id);
            broadcast(presence("left", current ? *current : conn.participant), false);
        } catch (const std::exception&) {
            logger.warning("Presence 'left' broadcast failed");
        }
    }

    void send(const Conn& conn, const Json& message) { conn.ws->send_text(share(message)); }

    void handle_frame(Conn& conn, const Inbound& frame) {
        if (frame.binary) {
            send(conn, error_message(binary_frame_error()));
            return;
        }
        auto checked = check_text_frame(frame.text);
        if (auto* error = std::get_if<FrameError>(&checked)) {
            send(conn, error_message(*error));
            return;
        }
        try {
            dispatch(conn, std::get<Json>(checked));
        } catch (const std::exception& error) {
            logger.error_with(error, "Dashboard message handling failed");
            send(conn, error_message(internal_error()));
        }
    }

    void dispatch(Conn& conn, const Json& message) {
        const Json& type = detail::get(message, "type");
        if (is_text(type, "ping")) {
            send(conn, pong_message());
        } else if (is_text(type, "action")) {
            handle_action(conn, message);
        } else if (is_text(type, "undo")) {
            handle_undo(conn, message);
        } else {
            send(conn, error_message(unknown_type_error(type)));
        }
    }

    std::optional<std::string> participant_of(const Conn& conn) {
        std::lock_guard lock(mutex_);
        return conn.participant_id;
    }

    // Claims an in-flight slot; false at the cap.
    bool begin_inflight(Conn& conn) {
        std::lock_guard lock(mutex_);
        if (conn.inflight >= kMaxInflightActions) {
            return false;
        }
        ++conn.inflight;
        return true;
    }

    void end_inflight(Conn& conn) {
        std::lock_guard lock(mutex_);
        --conn.inflight;
    }

    static Json wire_results(const std::vector<meeting_state::OpResult>& applied) {
        Json results = Json::array();
        for (const auto& result : applied) {
            results.push_back(op_result_wire(result));
        }
        return results;
    }

    void handle_action(Conn& conn, const Json& message) {
        const Json& op = detail::get(message, "op");
        if (!op.is_object()) {
            send(conn, action_result(detail::get(message, "client_action_id"),
                                     rejection("malformed_op")));
            return;
        }
        if (!begin_inflight(conn)) {
            send(conn, action_result(detail::get(message, "client_action_id"),
                                     rejection("too_many_in_flight")));
            return;
        }
        Json results;
        try {
            const std::string_view actor = conn.role == kRoleHost ? "host" : "user";
            results = wire_results(engine_.apply_client_action(actor, participant_of(conn), op));
        } catch (const std::exception& error) {
            logger.error_with(error, "apply_client_action failed");
            results = rejection("internal_error");
        }
        end_inflight(conn);
        send(conn, action_result(detail::get(message, "client_action_id"), std::move(results)));
    }

    void handle_undo(Conn& conn, const Json& message) {
        const Json& client_action_id = detail::get(message, "client_action_id");
        if (conn.role != kRoleHost) {
            send(conn, action_result(client_action_id, rejection("host_only")));
            return;
        }
        const auto seq = coerce_seq(detail::get(message, "seq"));
        if (!seq) {
            send(conn, action_result(client_action_id, rejection("invalid_seq")));
            return;
        }
        if (!begin_inflight(conn)) {
            send(conn, action_result(client_action_id, rejection("too_many_in_flight")));
            return;
        }
        Json results;
        try {
            results = wire_results(engine_.undo(*seq, participant_of(conn)));
        } catch (const std::exception& error) {
            logger.error_with(error, "undo failed");
            results = rejection("internal_error");
        }
        end_inflight(conn);
        send(conn, action_result(client_action_id, std::move(results)));
    }

    net::Reactor& reactor_;
    DashboardEngine& engine_;
    std::shared_ptr<meeting_state::MeetingRepository> repository_;

    mutable std::mutex mutex_;
    bool alive_ = true;
    bool running_ = false;
    Executor* worker_ = nullptr;
    std::vector<ConnPtr> registry_; // registration order
    std::map<const net::WsConnection*, ConnPtr> attached_;
    // Timed sends that completed inside the running fan-out (t_fanout_hub).
    std::vector<std::pair<ConnPtr, bool>> completed_inline_;

    std::mutex guests_mutex_;
    // Stable guest key -> (participant id, display name), for the hub's lifetime.
    std::map<std::string, std::pair<std::string, std::string>, std::less<>> guest_participants_;

    std::mutex subscription_mutex_;
    std::shared_ptr<meeting_state::MeetingStateStore> subscribed_store_;
    Subscription subscription_;

    mutable std::mutex providers_mutex_;
    std::function<std::string()> guest_url_;
    std::function<Json()> voice_commands_;
};

} // namespace detail

WsHub::WsHub(net::Reactor& reactor, DashboardEngine& engine,
             std::shared_ptr<meeting_state::MeetingRepository> repository)
    : pool_(std::make_unique<ThreadPool>("fv-meeting-web", kWorkerThreads)),
      core_(std::make_shared<detail::HubCore>(reactor, engine, std::move(repository))) {
    core_->attach_worker(pool_.get());
}

WsHub::~WsHub() {
    core_->shutdown();
    core_->retire();
    pool_.reset();
}

net::WsHandlers WsHub::handlers() {
    net::WsHandlers handlers;
    std::weak_ptr<detail::HubCore> weak = core_;
    handlers.on_open = [weak](std::shared_ptr<net::WsConnection> ws,
                              const net::HttpRequest& request) {
        if (auto core = weak.lock()) {
            core->on_open(ws, request);
        } else {
            ws->close(kWsCloseServerError, {});
        }
    };
    handlers.on_text = [weak](const std::shared_ptr<net::WsConnection>& ws, std::string_view text) {
        if (auto core = weak.lock()) {
            core->on_message(ws, Inbound{false, std::string(text)});
        } else {
            ws->release_message();
        }
    };
    handlers.on_binary = [weak](const std::shared_ptr<net::WsConnection>& ws,
                                std::span<const std::byte>) {
        if (auto core = weak.lock()) {
            core->on_message(ws, Inbound{true, {}});
        } else {
            ws->release_message();
        }
    };
    handlers.on_close = [weak](const std::shared_ptr<net::WsConnection>& ws, std::uint16_t,
                               std::string_view) {
        if (auto core = weak.lock()) {
            core->on_close(ws);
        }
    };
    handlers.max_message_bytes = kMaxFrameBytes;
    handlers.max_queued_messages = kMaxQueuedMessages;
    return handlers;
}

void WsHub::on_startup() {
    core_->startup();
}

void WsHub::on_shutdown() {
    core_->shutdown();
}

void WsHub::schedule_broadcast(const Json& message, bool host_only) {
    core_->broadcast(message, host_only);
}

void WsHub::schedule_invalidate_connections(bool resync) {
    core_->invalidate(resync);
}

void WsHub::on_store_batch(std::int64_t seq, const std::vector<meeting_state::OpResult>& applied) {
    core_->on_store_batch(seq, applied);
}

void WsHub::set_guest_url_provider(std::function<std::string()> provider) {
    core_->set_guest_url_provider(std::move(provider));
}

void WsHub::set_voice_commands(std::function<Json()> provider) {
    core_->set_voice_commands(std::move(provider));
}

Json WsHub::voice_commands() const {
    return core_->voice_commands();
}

std::size_t WsHub::connection_count() const {
    return core_->connection_count();
}

} // namespace fv::meeting_web
