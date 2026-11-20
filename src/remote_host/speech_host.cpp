#include "remote_host/speech_host.h"

#include <algorithm>
#include <array>
#include <condition_variable>
#include <deque>
#include <exception>
#include <map>
#include <mutex>
#include <set>
#include <utility>
#include <variant>

#include "base/executor.h"
#include "base/hash.h"
#include "base/identity.h"
#include "base/ids.h"
#include "base/log.h"
#include "net/http_server.h"
#include "pyport/errors.h"
#include "pyport/json.h"
#include "pyport/number.h"
#include "pyport/text.h"
#include "remote/discovery_codec.h"
#include "remote/protocol.h"
#include "remote/wire.h"
#include "remote_host/admission.h"
#include "remote_host/detail/pyvalue.h"
#include "remote_host/discovery_responder.h"
#include "remote_host/dispatch.h"
#include "remote_host/pairing_host.h"

namespace fv::remote_host {

namespace {

constexpr log::Logger logger("services.remote_asr.host");

// How often a pairing request waiting for the owner looks at its connection and deadline.
constexpr auto kApprovalPoll = std::chrono::milliseconds(200);
// websockets' max_queue=2 pauses its reader once more than two frames wait; WsHandlers pauses at
// n unreleased messages.
constexpr std::size_t kMaxQueuedMessages = 3;
constexpr int kPoolThreads = 8;

using detail::get;
using detail::is_text;

// How the server's callbacks reach Impl without owning it. Were they to lock a weak_ptr, one
// still running as the host went away would hold the last reference, and ~Impl would run on the
// reactor thread, its pool.reset() holding the reactor until a running task (a model switch)
// ended, and destroying the host's state after its owner had moved on. ~SpeechHost closes the
// gate and waits for the callbacks inside, so Impl is destroyed by its owner.
// Affinity: thread-safe.
class HandlerGate {
public:
    // False once closed: the callback must not touch Impl.
    bool enter() {
        std::lock_guard lock(mutex_);
        if (closed_) {
            return false;
        }
        ++active_;
        return true;
    }
    void leave() {
        {
            std::lock_guard lock(mutex_);
            --active_;
        }
        idle_.notify_all();
    }
    // Refuses later callbacks and waits for those inside; never from a reactor callback.
    void close() {
        std::unique_lock lock(mutex_);
        closed_ = true;
        idle_.wait(lock, [this] { return active_ == 0; });
    }

private:
    std::mutex mutex_;
    std::condition_variable idle_;
    int active_ = 0;
    bool closed_ = false;
};

// Runs `fn` with the host's Impl while the gate is open.
template <class Impl, class Fn>
void through_gate(const std::shared_ptr<HandlerGate>& gate, Impl* impl, Fn&& fn) {
    if (!gate->enter()) {
        return;
    }
    struct Leave {
        HandlerGate& gate;
        ~Leave() { gate.leave(); }
    } leave{*gate};
    fn(*impl);
}

// bytes.fromhex: pairs of hex digits, ASCII whitespace between them ignored; nullopt where
// Python raises ValueError.
std::optional<std::vector<std::uint8_t>> from_hex(std::string_view text) {
    const auto digit = [](char c) -> int {
        if (c >= '0' && c <= '9') {
            return c - '0';
        }
        if (c >= 'a' && c <= 'f') {
            return c - 'a' + 10;
        }
        if (c >= 'A' && c <= 'F') {
            return c - 'A' + 10;
        }
        return -1;
    };
    const auto space = [](char c) {
        return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\v' || c == '\f';
    };
    std::vector<std::uint8_t> out;
    std::size_t i = 0;
    while (i < text.size()) {
        if (space(text[i])) {
            ++i;
            continue;
        }
        if (i + 1 >= text.size()) {
            return std::nullopt;
        }
        const int high = digit(text[i]);
        const int low = digit(text[i + 1]);
        if (high < 0 || low < 0) {
            return std::nullopt;
        }
        out.push_back(static_cast<std::uint8_t>(high * 16 + low));
        i += 2;
    }
    return out;
}

Json error_message(std::string_view code, std::string message) {
    Json out = Json::object();
    out["type"] = "error";
    out["code"] = std::string(code);
    out["message"] = std::move(message);
    return out;
}

double wall_seconds(const Clock& clock) {
    return std::chrono::duration<double>(clock.utc().time_since_epoch()).count();
}

std::string_view outcome_name(ApprovalOutcome outcome) {
    switch (outcome) {
    case ApprovalOutcome::allowed:
        return "allowed";
    case ApprovalOutcome::denied:
        return "denied";
    case ApprovalOutcome::stopped:
        return "stopped";
    case ApprovalOutcome::timeout:
        return "timeout";
    case ApprovalOutcome::canceled:
        return "canceled";
    }
    return "canceled";
}

// One admitted request's work slot, given back once (WorkAdmission::release): by the request
// when it is done, else when the task holding it is dropped unrun.
// Affinity: one thread at a time.
struct AdmittedSlot {
    explicit AdmittedSlot(WorkAdmission& admission) : admission(admission) {}
    AdmittedSlot(const AdmittedSlot&) = delete;
    AdmittedSlot& operator=(const AdmittedSlot&) = delete;
    ~AdmittedSlot() { release(); }
    void release() {
        if (!std::exchange(released, true)) {
            admission.release();
        }
    }
    WorkAdmission& admission;
    bool released = false;
};

} // namespace

// One paired computer's speech connection (host.py _Client).
struct HostClient {
    std::string device_id;
    std::string name;
    std::string address;
    std::shared_ptr<net::WsConnection> ws;
    bool busy = false;                        // decoding one of its requests right now
    std::vector<std::string> engine_identity; // the engine it was told about in ready
    bool in_request = false;                  // handling any of its requests
    bool reports_recording = false;
    std::string recording_id;
    double since = 0.0; // wall clock, seconds
};

// One WebSocket from accept to close. Everything but `ws` is touched on its strand only.
struct HostConnection {
    enum class Phase { Waiting, Approval, Serving, History, Done };
    struct Message {
        bool binary = false;
        std::string data;
    };

    std::shared_ptr<net::WsConnection> ws;
    std::unique_ptr<Strand> strand;
    std::string address;
    std::string local_address;
    Phase phase = Phase::Waiting;
    bool open = true;
    Reservation handshake;
    Reservation connection;
    TimerHandle timer;

    // Frames are handled one at a time, in order, even while a request waits for a work slot.
    std::deque<Message> inbox;
    bool handling = false;

    // Pairing by approval.
    std::shared_ptr<PairRequest> request;
    std::array<std::uint8_t, remote::PAIRING_NONCE_BYTES> host_nonce{};
    std::string request_name;
    bool awaiting_nonce = false;
    bool shown = false;

    // A paired computer's speech connection.
    std::string token;
    remote::PairedDevice device;
    std::string connection_id;
    std::shared_ptr<HostEngine> engine;
    std::vector<std::string> identity;
    std::set<std::string> streams;
    bool registered = false;
    WorkAdmission::Ticket ticket;
};

struct SpeechHost::Impl : std::enable_shared_from_this<Impl> {
    explicit Impl(SpeechHostDeps deps_in)
        : deps(std::move(deps_in)), activity(deps.clock),
          pairing(deps.clock, deps.host_name,
                  PairingLimits{deps.limits.handshake, deps.limits.approval,
                                deps.limits.approval_window, deps.limits.max_approval_requests},
                  [](std::string_view address) { return remote::allowed_sender(address); }),
          connection_slots(deps.limits.max_connections),
          handshake_slots(deps.limits.max_handshakes),
          responder([this] { return dispatcher.discovery_info(port_now()); }, deps.clock),
          pool(std::make_unique<ThreadPool>("fv-remote-host", kPoolThreads)),
          admission(*pool, deps.limits.max_active_jobs, deps.limits.max_queued_jobs,
                    deps.limits.job_wait),
          dispatcher(deps, activity,
                     [this](std::string kind, Json info) { emit(kind, std::move(info)); }) {}

    ~Impl() {
        admission.cancel_all();
        std::vector<std::shared_ptr<HostConnection>> all;
        {
            std::lock_guard lock(conns_mutex);
            for (auto& [key, conn] : conns) {
                all.push_back(conn);
            }
        }
        for (const auto& conn : all) {
            conn->strand->shutdown(std::chrono::milliseconds(0));
        }
        pool.reset();
        std::lock_guard lock(conns_mutex);
        conns.clear();
    }

    SpeechHostDeps deps;
    // The server's callbacks enter here (HandlerGate); it outlives Impl in their captures.
    std::shared_ptr<HandlerGate> gate = std::make_shared<HandlerGate>();
    HostActivity activity;
    Signal<std::string, Json> events;
    PairingHost pairing;
    SlotPool connection_slots;
    SlotPool handshake_slots;
    DiscoveryResponder responder;

    mutable std::mutex state_mutex;
    std::unique_ptr<net::HttpServer> server;
    std::optional<std::uint16_t> port;
    std::vector<std::pair<std::string, HostClient>> clients; // by connection id, in arrival order
    std::string update_token;
    std::chrono::steady_clock::time_point update_until{};
    std::map<std::pair<std::string, std::string>, std::chrono::steady_clock::time_point>
        reconnecting;

    std::mutex conns_mutex;
    std::map<const net::WsConnection*, std::shared_ptr<HostConnection>> conns;

    // Declared after everything its tasks use, so it is gone (and its threads joined) first.
    std::unique_ptr<ThreadPool> pool;
    WorkAdmission admission;
    Dispatcher dispatcher;

    // ---- small helpers ----

    std::optional<std::uint16_t> port_now() const {
        std::lock_guard lock(state_mutex);
        return port;
    }

    bool running() const {
        std::lock_guard lock(state_mutex);
        return server != nullptr;
    }

    void emit(const std::string& kind, Json info) {
        try {
            events.emit(kind, info);
        } catch (const std::exception&) {
            logger.warning("Remote engine host event listener raised");
        }
    }

    static void send(const std::shared_ptr<HostConnection>& conn, const Json& message) {
        conn->ws->send_text(std::make_shared<const std::string>(remote::dumps_compact(message)));
    }

    static void close(const std::shared_ptr<HostConnection>& conn, std::uint16_t code,
                      std::string reason) {
        conn->ws->close(code, std::move(reason));
    }

    void refuse_busy(const std::shared_ptr<HostConnection>& conn) {
        Json message = error_message("busy", std::string(BUSY_MESSAGE));
        message["retry_after_ms"] = 1000;
        send(conn, message);
        close(conn, remote::CLOSE_BUSY, "host busy");
    }

    bool update_active_locked(std::chrono::steady_clock::time_point now) const {
        return now < update_until;
    }

    bool update_active() const {
        std::lock_guard lock(state_mutex);
        return update_active_locked(deps.clock.steady());
    }

    bool recording_reconnecting_locked(std::chrono::steady_clock::time_point now) {
        std::erase_if(reconnecting, [now](const auto& entry) { return entry.second <= now; });
        return !reconnecting.empty();
    }

    HostClient* client_locked(const std::string& connection_id) {
        for (auto& [id, client] : clients) {
            if (id == connection_id) {
                return &client;
            }
        }
        return nullptr;
    }

    // ---- the strand -----------------------------------------------------------------------------

    // Runs `fn` on the connection's strand while the connection is still known.
    template <class Fn> void post(const std::shared_ptr<HostConnection>& conn, Fn fn) {
        std::weak_ptr<HostConnection> weak = conn;
        conn->strand->post([this, weak, fn = std::move(fn)]() mutable {
            if (auto alive = weak.lock()) {
                guarded(alive, [&] { fn(alive); });
            }
        });
    }

    template <class Fn>
    TimerHandle post_after(const std::shared_ptr<HostConnection>& conn,
                           std::chrono::milliseconds delay, Fn fn) {
        std::weak_ptr<HostConnection> weak = conn;
        return conn->strand->post_after(delay, [this, weak, fn = std::move(fn)]() mutable {
            if (auto alive = weak.lock()) {
                guarded(alive, [&] { fn(alive); });
            }
        });
    }

    // A handler that raises: a paired connection is dropped after logging (_serve_device's
    // except), any other is closed 1011 as websockets closes a handler that failed.
    template <class Fn> void guarded(const std::shared_ptr<HostConnection>& conn, Fn&& fn) {
        try {
            fn();
        } catch (const std::exception& error) {
            if (conn->phase == HostConnection::Phase::Serving) {
                logger.warning("Remote engine client {} dropped after an error: {}",
                               conn->device.name, error.what());
                close(conn, 1000, "");
                set_in_request(conn->connection_id, false);
                end_serving(conn);
            } else {
                logger.error("Remote engine connection handler failed: {}", error.what());
                close(conn, 1011, "");
                if (conn->request) {
                    finish_request(conn);
                }
                end(conn);
            }
            if (conn->handling) {
                handled(conn);
                pump(conn);
            }
        }
    }

    // ---- reactor callbacks --------------------------------------------------------------------

    // Measures a reactor callback for deps.reactor_timing.
    template <class Fn> void on_reactor(Fn&& fn) {
        if (!deps.reactor_timing) {
            fn();
            return;
        }
        const auto started = std::chrono::steady_clock::now();
        fn();
        deps.reactor_timing(std::chrono::steady_clock::now() - started);
    }

    void on_open(std::shared_ptr<net::WsConnection> ws) {
        auto conn = std::make_shared<HostConnection>();
        conn->ws = ws;
        conn->strand = std::make_unique<Strand>(*pool, "fv-remote-host-connection", 1,
                                                ShutdownMode::CancelQueued);
        const std::string remote = ws->remote_endpoint().address;
        conn->address = remote.empty() ? "unknown" : remote;
        if (deps.peer_address) {
            conn->address = deps.peer_address(conn->address);
        }
        conn->local_address = ws->local_endpoint().address;
        {
            std::lock_guard lock(conns_mutex);
            conns[ws.get()] = conn;
        }
        post(conn, [this](const std::shared_ptr<HostConnection>& c) { opened(c); });
    }

    std::shared_ptr<HostConnection> find(const net::WsConnection* ws) {
        std::lock_guard lock(conns_mutex);
        const auto found = conns.find(ws);
        return found == conns.end() ? nullptr : found->second;
    }

    void on_message(const net::WsConnection* ws, bool binary, std::string data) {
        if (auto conn = find(ws)) {
            post(conn, [this, binary,
                        data = std::move(data)](const std::shared_ptr<HostConnection>& c) mutable {
                c->inbox.push_back({binary, std::move(data)});
                pump(c);
            });
        }
    }

    void on_close(const net::WsConnection* ws) {
        auto conn = find(ws);
        if (!conn) {
            return;
        }
        post(conn, [this](const std::shared_ptr<HostConnection>& c) { closed(c); });
    }

    // ---- connection life ------------------------------------------------------------------------

    void opened(const std::shared_ptr<HostConnection>& conn) {
        // Every connection starts in a handshake slot; a paired computer's hello trades it for
        // a long-lived connection slot.
        conn->handshake = Reservation(handshake_slots);
        if (!conn->handshake.acquire()) {
            refuse_busy(conn);
            end(conn);
            return;
        }
        conn->timer = post_after(conn, deps.limits.handshake,
                                 [this](const std::shared_ptr<HostConnection>& c) {
                                     if (c->phase == HostConnection::Phase::Waiting && c->open) {
                                         close(c, remote::CLOSE_TIMEOUT, "handshake timeout");
                                         end(c);
                                     }
                                 });
    }

    void closed(const std::shared_ptr<HostConnection>& conn) {
        conn->open = false;
        conn->timer.cancel();
        switch (conn->phase) {
        case HostConnection::Phase::Waiting:
            end(conn);
            break;
        case HostConnection::Phase::Approval:
            if (conn->awaiting_nonce) {
                logger.info("Pairing request from {} at {} ended with the connection",
                            py::repr(conn->request_name), conn->address);
                finish_request(conn);
                end(conn);
            } else {
                poll_answer(conn);
            }
            break;
        case HostConnection::Phase::Serving:
            if (conn->ticket) {
                admission.cancel(std::exchange(conn->ticket, nullptr));
            }
            end_serving(conn);
            break;
        case HostConnection::Phase::History:
            if (deps.history) {
                deps.history->on_close(conn->ws);
            }
            end(conn);
            break;
        case HostConnection::Phase::Done:
            break;
        }
        std::lock_guard lock(conns_mutex);
        conns.erase(conn->ws.get());
    }

    // The handler is over: no slot is held any more and later frames are ignored.
    void end(const std::shared_ptr<HostConnection>& conn) {
        conn->timer.cancel();
        conn->handshake.release();
        conn->connection.release();
        conn->phase = HostConnection::Phase::Done;
    }

    void handled(const std::shared_ptr<HostConnection>& conn) {
        conn->handling = false;
        conn->ws->release_message();
    }

    void pump(const std::shared_ptr<HostConnection>& conn) {
        while (!conn->handling && !conn->inbox.empty()) {
            HostConnection::Message message = std::move(conn->inbox.front());
            conn->inbox.pop_front();
            conn->handling = true;
            guarded(conn, [&] { handle(conn, message); });
        }
    }

    void handle(const std::shared_ptr<HostConnection>& conn, HostConnection::Message& message) {
        switch (conn->phase) {
        case HostConnection::Phase::Waiting:
            first_message(conn, message);
            handled(conn);
            return;
        case HostConnection::Phase::Approval:
            if (conn->awaiting_nonce) {
                nonce_message(conn, message);
            }
            handled(conn);
            return;
        case HostConnection::Phase::Serving:
            serve_frame(conn, message);
            return;
        case HostConnection::Phase::History:
            if (message.binary) {
                // HistoryBroker reads text frames only; anything else ends the session.
                close(conn, 1000, "");
            } else if (deps.history) {
                deps.history->on_text(conn->ws, message.data);
            }
            handled(conn);
            return;
        case HostConnection::Phase::Done:
            handled(conn);
            return;
        }
    }

    // ---- the first message ------------------------------------------------------------------

    void first_message(const std::shared_ptr<HostConnection>& conn,
                       const HostConnection::Message& frame) {
        conn->timer.cancel();
        Json message;
        if (!frame.binary) {
            try {
                message = py::loads(frame.data);
            } catch (const py::JsonError&) {
                message = nullptr;
            }
        }
        if (!message.is_object()) {
            close(conn, remote::CLOSE_BAD_REQUEST, "expected hello");
            end(conn);
            return;
        }
        const Json& type = get(message, "type");
        if (is_text(type, "probe")) {
            // Answered before the version check, so discovery can say "update it there" instead
            // of skipping the host.
            send(conn, dispatcher.probe_message());
            close(conn, 1000, "");
            end(conn);
            return;
        }
        if (!py::py_equal(get(message, "protocol"), Json(remote::PROTOCOL_VERSION))) {
            send(conn, error_message("protocol", "This host speaks remote engine protocol " +
                                                     std::to_string(remote::PROTOCOL_VERSION) +
                                                     "; update " + std::string(identity().product) +
                                                     " on both computers."));
            close(conn, remote::CLOSE_BAD_REQUEST, "protocol version");
            end(conn);
            return;
        }
        if (is_text(type, "pair") && get(message, "tailscale") == Json(true)) {
            pair_tailscale(conn, message);
        } else if (is_text(type, "pair")) {
            pair_code(conn, message);
        } else if (is_text(type, "pair_request")) {
            pair_request(conn, message);
        } else if (is_text(type, "hello")) {
            hello(conn, message);
        } else {
            close(conn, remote::CLOSE_BAD_REQUEST, "expected hello");
            end(conn);
        }
    }

    // ---- pairing --------------------------------------------------------------------------------

    void finish_pairing(const std::shared_ptr<HostConnection>& conn, const std::string& name,
                        std::string_view via) {
        auto [device, token] = deps.registry.add(name, via);
        logger.info("Paired remote engine device {} from {} ({})", py::repr(device.name),
                    conn->address, via);
        Json paired = Json::object();
        paired["type"] = "paired";
        paired["token"] = token;
        paired["device_id"] = device.id;
        paired["host"] = dispatcher.host_info();
        send(conn, paired);
        close(conn, 1000, "");
        activity.paired(device.id, device.name, via);
        Json info = Json::object();
        info["name"] = device.name;
        emit("paired", std::move(info));
        emit("devices", Json::object());
    }

    void refuse_pairing(const std::shared_ptr<HostConnection>& conn,
                        const PairingRefusal& refusal) {
        send(conn, error_message(refusal.code, refusal.message));
        close(conn, remote::CLOSE_UNAUTHORIZED, refusal.code);
    }

    std::optional<remote::PeerIdentity>
    own_device(const std::string& remote, const std::string& local, const std::string& owner) {
        if (deps.own_device) {
            return deps.own_device(remote, local, owner);
        }
        if (deps.tailscale == nullptr) {
            return std::nullopt;
        }
        return remote::own_device(remote, local, owner, *deps.tailscale);
    }

    void pair_tailscale(const std::shared_ptr<HostConnection>& conn, const Json& message) {
        // Pairs one of the owner's own Tailscale computers without a code.
        const std::string owner = dispatcher.tailscale_pairing_owner();
        std::optional<remote::PeerIdentity> identity;
        if (!owner.empty()) {
            identity = own_device(conn->address, conn->local_address, owner);
        }
        if (!identity) {
            logger.info("Refused Tailscale pairing from {}", conn->address);
            refuse_pairing(
                conn, PairingRefusal{
                          "tailscale_refused",
                          owner.empty()
                              ? "This host doesn't pair over Tailscale without a code. Enter the "
                                "pairing code shown there instead."
                              : "This host only lets its owner's own Tailscale computers pair "
                                "without a code. Enter the pairing code shown there instead."});
            end(conn);
            return;
        }
        const Json& given = get(message, "device_name");
        const std::string name =
            clean_device_name(detail::truthy(given) ? given : Json(identity->node));
        finish_pairing(conn, name, "tailscale");
        end(conn);
    }

    void pair_code(const std::shared_ptr<HostConnection>& conn, const Json& message) {
        const Json& given = get(message, "code");
        const std::string code = py::strip(detail::truthy(given) ? detail::py_str(given) : "");
        const std::string name = clean_device_name(get(message, "device_name"));
        const auto check = pairing.check_code(code);
        if (check.refusal) {
            logger.info("Refused remote engine pairing from {}: {}", conn->address,
                        check.refusal->code);
            refuse_pairing(conn, *check.refusal);
            if (check.closed_now) {
                Json info = Json::object();
                info["open"] = false;
                emit("pairing", std::move(info));
            }
            end(conn);
            return;
        }
        finish_pairing(conn, name, "code");
        end(conn);
    }

    // Pairs a computer found on the network once this computer's owner allows it. The nonce
    // exchange gives both screens the same six digits only when nobody is in the middle; the
    // owner allows the request if they match.
    void pair_request(const std::shared_ptr<HostConnection>& conn, const Json& message) {
        const std::string name = clean_device_name(get(message, "device_name"));
        auto admitted = pairing.admit_request(name, conn->address);
        if (auto* refusal = std::get_if<PairingRefusal>(&admitted)) {
            logger.info("Refused a pairing request from {}: {}", conn->address, refusal->code);
            refuse_pairing(conn, *refusal);
            end(conn);
            return;
        }
        conn->request = std::get<std::shared_ptr<PairRequest>>(admitted);
        conn->request_name = name;
        conn->phase = HostConnection::Phase::Approval;
        conn->awaiting_nonce = true;
        random_bytes(std::as_writable_bytes(std::span(conn->host_nonce)));
        Json commit = Json::object();
        commit["type"] = "pair_commit";
        commit["commit"] = remote::pairing_commitment(conn->host_nonce);
        commit["timeout_s"] =
            std::chrono::duration_cast<std::chrono::seconds>(deps.limits.approval).count();
        send(conn, commit);
        conn->timer = post_after(
            conn, deps.limits.handshake, [this](const std::shared_ptr<HostConnection>& c) {
                if (c->phase == HostConnection::Phase::Approval && c->awaiting_nonce) {
                    nonce_received(c, {});
                }
            });
    }

    void nonce_message(const std::shared_ptr<HostConnection>& conn,
                       const HostConnection::Message& frame) {
        std::vector<std::uint8_t> nonce;
        if (!frame.binary) {
            Json reply;
            try {
                reply = py::loads(frame.data);
            } catch (const py::JsonError&) {
                reply = nullptr;
            }
            if (reply.is_object() && is_text(get(reply, "type"), "pair_nonce")) {
                const Json& given = get(reply, "nonce");
                nonce = from_hex(detail::truthy(given) ? detail::py_str(given) : "")
                            .value_or(std::vector<std::uint8_t>{});
            }
        }
        nonce_received(conn, std::move(nonce));
    }

    void nonce_received(const std::shared_ptr<HostConnection>& conn,
                        std::vector<std::uint8_t> client_nonce) {
        conn->timer.cancel();
        conn->awaiting_nonce = false;
        if (client_nonce.size() != remote::PAIRING_NONCE_BYTES) {
            close(conn, remote::CLOSE_BAD_REQUEST, "expected pair_nonce");
            finish_request(conn);
            end(conn);
            return;
        }
        std::string sas =
            remote::pairing_sas(conn->host_nonce, client_nonce, deps.identity.fingerprint);
        Json reveal = Json::object();
        reveal["type"] = "pair_reveal";
        reveal["nonce"] = to_hex(conn->host_nonce);
        send(conn, reveal);
        std::weak_ptr<HostConnection> weak = conn;
        pairing.reveal(conn->request, std::move(sas), [this, weak] {
            if (auto alive = weak.lock()) {
                post(alive, [this](const std::shared_ptr<HostConnection>& c) { poll_answer(c); });
            }
        });
        conn->shown = true;
        logger.info("Pairing request from {} at {} is waiting for an answer here",
                    py::repr(conn->request_name), conn->address);
        Json info = Json::object();
        info["name"] = conn->request_name;
        info["address"] = conn->address;
        emit("pair_request", std::move(info));
        conn->timer =
            post_after(conn, kApprovalPoll,
                       [this](const std::shared_ptr<HostConnection>& c) { poll_answer(c); });
    }

    void poll_answer(const std::shared_ptr<HostConnection>& conn) {
        if (conn->phase != HostConnection::Phase::Approval || conn->awaiting_nonce ||
            !conn->request) {
            return;
        }
        const auto outcome = pairing.outcome(conn->request, conn->open && conn->ws->is_open());
        if (!outcome) {
            conn->timer =
                post_after(conn, kApprovalPoll,
                           [this](const std::shared_ptr<HostConnection>& c) { poll_answer(c); });
            return;
        }
        conn->timer.cancel();
        logger.info("Pairing request from {} at {}: {}", py::repr(conn->request_name),
                    conn->address, outcome_name(*outcome));
        const std::string& host = deps.host_name;
        switch (*outcome) {
        case ApprovalOutcome::allowed:
            finish_pairing(conn, conn->request_name, "approval");
            break;
        case ApprovalOutcome::canceled:
            break;
        case ApprovalOutcome::denied:
            refuse_pairing(conn, {"pair_denied", host + " declined the request."});
            break;
        case ApprovalOutcome::stopped:
            refuse_pairing(conn, {"pair_closed", host + " stopped sharing its engine."});
            break;
        case ApprovalOutcome::timeout:
            refuse_pairing(conn, {"pair_timeout", "Nobody answered on " + host +
                                                      ". Try again while someone is at it, or "
                                                      "pair with a code."});
            break;
        }
        finish_request(conn);
        end(conn);
    }

    void finish_request(const std::shared_ptr<HostConnection>& conn) {
        if (!conn->request) {
            return;
        }
        pairing.finish(std::exchange(conn->request, nullptr));
        conn->awaiting_nonce = false;
        if (std::exchange(conn->shown, false)) {
            emit("pair_request", Json::object());
        }
    }

    // ---- a paired computer --------------------------------------------------------------------

    // DeviceRegistry.authenticate: a token that is not a non-empty str is nobody's (host.py:179),
    // so a hello whose token is a number or a list is answered "unauthorized", not dropped.
    std::optional<remote::PairedDevice> authenticate(const Json& token) {
        if (!detail::nonempty_string(token)) {
            return std::nullopt;
        }
        return deps.registry.authenticate(token.get_ref<const std::string&>());
    }

    void hello(const std::shared_ptr<HostConnection>& conn, const Json& message) {
        const Json& token = get(message, "token");
        const auto device = authenticate(token);
        if (!device) {
            send(conn, error_message("unauthorized", "This computer isn't paired with the host "
                                                     "anymore. Pair it again."));
            close(conn, remote::CLOSE_UNAUTHORIZED, "unauthorized");
            end(conn);
            return;
        }
        conn->connection = Reservation(connection_slots);
        if (!conn->connection.acquire()) {
            refuse_busy(conn);
            end(conn);
            return;
        }
        conn->handshake.release();
        conn->token = token.get<std::string>();
        conn->device = *device;
        if (is_text(get(message, "purpose"), "history")) {
            conn->phase = HostConnection::Phase::History;
            if (deps.history) {
                deps.history->serve(conn->ws, *device, conn->token,
                                    get(message, "history_enabled") == Json(true));
            } else {
                close(conn, 1001, "host stopped sharing");
            }
            return;
        }
        auto engine = dispatcher.engine();
        auto identity = engine->identity();
        Json ready = dispatcher.ready_message(*device, *engine, update_active());
        const Json& capabilities = get(message, "capabilities");
        const std::string connection_id = uuid4_hex().substr(0, 8);
        {
            std::lock_guard lock(state_mutex);
            if (!server) {
                // Sharing was turned off while this client was handshaking.
                close(conn, 1001, "host stopped sharing");
                end(conn);
                return;
            }
            HostClient client;
            client.device_id = device->id;
            client.name = device->name;
            client.address = conn->address;
            client.ws = conn->ws;
            client.engine_identity = identity;
            client.since = wall_seconds(deps.clock);
            client.reports_recording =
                capabilities.is_object() && get(capabilities, "recording_activity") == Json(true);
            clients.emplace_back(connection_id, std::move(client));
            if (update_active_locked(deps.clock.steady())) {
                ready["engine"]["available"] = false;
                ready["engine"]["status"] = host_update_message();
            }
        }
        conn->connection_id = connection_id;
        conn->engine = engine;
        conn->identity = identity;
        conn->registered = true;
        conn->phase = HostConnection::Phase::Serving;
        activity.connected(device->id, device->name);
        emit("clients", Json::object());
        // Registered before ready so an immediate engine change can find this client; changes
        // made while preparing the reply are checked again here.
        if (dispatcher.engine()->identity() != identity) {
            close(conn, remote::CLOSE_ENGINE_CHANGED, "engine changed");
            end_serving(conn);
            return;
        }
        send(conn, ready);
    }

    void set_in_request(const std::string& connection_id, bool value) {
        std::lock_guard lock(state_mutex);
        if (auto* client = client_locked(connection_id)) {
            client->in_request = value;
        }
    }

    void set_busy(const std::string& connection_id, bool busy) {
        std::string name;
        {
            std::lock_guard lock(state_mutex);
            auto* client = client_locked(connection_id);
            if (client == nullptr || client->busy == busy) {
                return;
            }
            client->busy = busy;
            name = client->name;
        }
        Json info = Json::object();
        info["name"] = name;
        info["busy"] = busy;
        emit("activity", std::move(info));
    }

    void serve_frame(const std::shared_ptr<HostConnection>& conn, HostConnection::Message& frame) {
        set_in_request(conn->connection_id, true);
        // Revocation is checked before accepting more work, including requests already buffered
        // when the device was removed.
        const auto current = authenticate(Json(conn->token));
        if (!current) {
            close(conn, remote::CLOSE_UNAUTHORIZED, "device removed");
            set_in_request(conn->connection_id, false);
            end_serving(conn);
            handled(conn);
            return;
        }
        const auto received = deps.clock.steady();
        auto bytes = std::make_shared<std::string>(std::move(frame.data));
        remote::Frame unpacked;
        try {
            unpacked = remote::unpack_frame(detail::bytes_of(*bytes), frame.binary);
        } catch (const remote::ProtocolError& error) {
            Json reply = Json::object();
            reply["id"] = nullptr;
            reply["error"] = error.what();
            reply["code"] = "bad_request";
            finish_frame(conn, std::move(reply), received);
            return;
        }
        auto header = std::make_shared<Json>(std::move(unpacked.header));
        auto payload = std::make_shared<std::vector<std::byte>>(std::move(unpacked.payload));
        const Json& op = get(*header, "op");
        const Json& id = get(*header, "id");
        if (is_text(op, "recording_begin") || is_text(op, "recording_end")) {
            finish_frame(conn, recording_request(*header, *payload, conn->connection_id), received);
            return;
        }
        if (!is_costly(op)) {
            finish_frame(conn, dispatch(conn, *header, *payload, current->name), received);
            return;
        }
        if (update_active()) {
            Json reply = Json::object();
            reply["id"] = id;
            reply["code"] = std::string(remote::ENGINE_RELOADING);
            reply["error"] = host_update_message();
            finish_frame(conn, std::move(reply), received);
            return;
        }
        std::weak_ptr<HostConnection> weak = conn;
        const std::string device_name = current->name;
        conn->ticket =
            admission.acquire([this, weak, header, payload, device_name, received](bool admitted) {
                // An admitted request holds a work slot until this guard goes: after its
                // dispatch, or when the task never runs because the connection (and with it
                // its strand, which drops queued tasks) went away first. Otherwise a client
                // that left just as it was admitted would keep the slot for good.
                std::shared_ptr<AdmittedSlot> slot;
                if (admitted) {
                    slot = std::make_shared<AdmittedSlot>(admission);
                }
                const auto alive = weak.lock();
                if (!alive) {
                    return;
                }
                post(alive, [this, header, payload, device_name, received, admitted,
                             slot = std::move(slot)](const std::shared_ptr<HostConnection>& c) {
                    c->ticket = nullptr;
                    if (!admitted) {
                        Json reply = Json::object();
                        reply["id"] = get(*header, "id");
                        reply["code"] = "busy";
                        reply["error"] = std::string(BUSY_MESSAGE);
                        reply["retry_after_ms"] = 1000;
                        finish_frame(c, std::move(reply), received);
                        return;
                    }
                    Json reply;
                    {
                        // The slot is free again before the reply goes out, as Python's
                        // release() in _admit_and_dispatch's finally precedes _send.
                        const auto held = slot;
                        reply = dispatch(c, *header, *payload, device_name);
                        held->release();
                    }
                    finish_frame(c, std::move(reply), received);
                });
            });
    }

    Json dispatch(const std::shared_ptr<HostConnection>& conn, const Json& header,
                  std::span<const std::byte> payload, const std::string& device_name) {
        const std::string connection_id = conn->connection_id;
        Dispatcher::Request request{
            .header = header,
            .payload = payload,
            .identity = conn->identity,
            .connection_id = connection_id,
            .streams = conn->streams,
            .device_name = device_name,
            .device_id = conn->device.id,
            .set_busy = [this, connection_id](bool busy) { set_busy(connection_id, busy); },
        };
        return dispatcher.dispatch_unpacked(request);
    }

    void finish_frame(const std::shared_ptr<HostConnection>& conn, Json reply,
                      std::chrono::steady_clock::time_point received) {
        if (reply.contains("result")) {
            // All of the time spent here, waiting for a work slot included, so the client
            // doesn't count this host's queue as network time.
            const double ms =
                std::chrono::duration<double, std::milli>(deps.clock.steady() - received).count();
            reply["host_ms"] = py::round_ndigits(ms, 1);
        }
        send(conn, reply);
        set_in_request(conn->connection_id, false);
        if (is_text(get(reply, "code"), "engine_changed")) {
            close(conn, remote::CLOSE_ENGINE_CHANGED, "engine changed");
            end_serving(conn);
        }
        handled(conn);
        pump(conn);
    }

    Json recording_request(const Json& header, std::span<const std::byte> payload,
                           const std::string& connection_id) {
        const Json& id = get(header, "id");
        const Json& recording = get(header, "recording_id");
        const auto coded = [&](std::string_view code, std::string error) {
            Json reply = Json::object();
            reply["id"] = id;
            reply["code"] = std::string(code);
            reply["error"] = std::move(error);
            return reply;
        };
        const auto valid = [](const Json& value) {
            if (!value.is_string() || value.get_ref<const std::string&>().size() != 32) {
                return false;
            }
            return std::ranges::all_of(value.get_ref<const std::string&>(), [](char c) {
                return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
            });
        };
        if (!payload.empty() || !valid(recording)) {
            return coded("bad_request", "Invalid recording ID");
        }
        const std::string recording_id = recording.get<std::string>();
        {
            std::lock_guard lock(state_mutex);
            auto* client = client_locked(connection_id);
            if (client == nullptr || !client->reports_recording) {
                return coded("bad_request", "Recording activity is disabled");
            }
            if (is_text(get(header, "op"), "recording_begin")) {
                if (update_active_locked(deps.clock.steady())) {
                    return coded(remote::ENGINE_RELOADING, host_update_message());
                }
                client->recording_id = recording_id;
                reconnecting.erase(std::pair{client->device_id, recording_id});
            } else if (client->recording_id == recording_id) {
                client->recording_id.clear();
            }
        }
        Json reply = Json::object();
        reply["id"] = id;
        reply["result"] = Json::object();
        return reply;
    }

    // Cancels a departed client's open streams and forgets it (_drop_client).
    void end_serving(const std::shared_ptr<HostConnection>& conn) {
        if (conn->ticket) {
            admission.cancel(std::exchange(conn->ticket, nullptr));
        }
        if (std::exchange(conn->registered, false)) {
            if (deps.mcp_peer_disconnect) {
                deps.mcp_peer_disconnect(conn->connection_id);
            }
            for (const auto& session : conn->streams) {
                try {
                    conn->engine->cancel_stream(session);
                } catch (const std::exception&) {
                    logger.debug("Could not cancel remote stream {}", session);
                }
            }
            conn->streams.clear();
            std::optional<HostClient> client;
            {
                std::lock_guard lock(state_mutex);
                const auto found = std::ranges::find_if(
                    clients, [&](const auto& entry) { return entry.first == conn->connection_id; });
                if (found != clients.end()) {
                    client = std::move(found->second);
                    clients.erase(found);
                }
                if (client && !client->recording_id.empty()) {
                    const auto now = deps.clock.steady();
                    recording_reconnecting_locked(now);
                    reconnecting[{client->device_id, client->recording_id}] =
                        now + deps.limits.recording_grace;
                }
            }
            activity.disconnected(conn->device.id, client ? client->name : conn->device.name);
            emit("clients", Json::object());
        }
        end(conn);
    }
};

// ---- SpeechHost
// ----------------------------------------------------------------------------------

SpeechHost::SpeechHost(SpeechHostDeps deps)
    : impl_(std::make_shared<Impl>(std::move(deps))), events(impl_->events) {}

SpeechHost::~SpeechHost() {
    stop();
    // A callback still running holds no reference: Impl goes here, on the owner's thread.
    impl_->gate->close();
}

std::expected<std::uint16_t, std::string> SpeechHost::start(int port, std::string bind,
                                                            std::optional<int> discovery_port) {
    Impl& impl = *impl_;
    std::uint16_t bound = 0;
    {
        std::lock_guard lock(impl.state_mutex);
        if (impl.server) {
            return *impl.port;
        }
        if (port < 0 || port > 65535) {
            // socket.bind's OverflowError, rather than a port cut down to 16 bits.
            return std::unexpected(std::string("bind(): port must be 0-65535."));
        }
        auto server = net::HttpServer::create(impl.deps.reactor);
        const std::shared_ptr<HandlerGate> gate = impl.gate;
        Impl* const host = &impl;
        net::WsHandlers handlers;
        handlers.permessage_deflate = false;
        handlers.max_message_bytes = std::max(remote::MAX_REQUEST_BYTES, remote::MAX_REPLY_BYTES);
        handlers.open_timeout = impl.deps.limits.handshake;
        handlers.ping_interval = impl.deps.limits.ping;
        handlers.ping_timeout = impl.deps.limits.ping;
        handlers.max_queued_messages = kMaxQueuedMessages;
        handlers.on_open = [gate, host](std::shared_ptr<net::WsConnection> ws,
                                        const net::HttpRequest&) {
            through_gate(gate, host, [&](Impl& self) {
                self.on_reactor([&] { self.on_open(std::move(ws)); });
            });
        };
        handlers.on_text = [gate, host](const std::shared_ptr<net::WsConnection>& ws,
                                        std::string_view text) {
            through_gate(gate, host, [&](Impl& self) {
                self.on_reactor([&] { self.on_message(ws.get(), false, std::string(text)); });
            });
        };
        handlers.on_binary = [gate, host](const std::shared_ptr<net::WsConnection>& ws,
                                          std::span<const std::byte> data) {
            through_gate(gate, host, [&](Impl& self) {
                self.on_reactor(
                    [&] { self.on_message(ws.get(), true, std::string(detail::chars_of(data))); });
            });
        };
        handlers.on_close = [gate, host](const std::shared_ptr<net::WsConnection>& ws,
                                         std::uint16_t, std::string_view) {
            through_gate(gate, host,
                         [&](Impl& self) { self.on_reactor([&] { self.on_close(ws.get()); }); });
        };
        server->websocket(std::string(remote::PATH), std::move(handlers));
        // websockets' process_request: anything but the engine's path is no remote engine.
        server->set_websocket_fallback(
            [](const net::HttpRequest&, std::shared_ptr<net::Responder> responder) {
                responder->send(404, {{"content-type", "text/plain; charset=utf-8"}},
                                std::string(remote::NOT_AN_ENDPOINT_BODY));
            });
        server->set_fallback(
            [](const net::HttpRequest& request, bool, std::shared_ptr<net::Responder> responder) {
                const auto path = request.path.substr(0, request.path.find('?'));
                if (path != remote::PATH) {
                    responder->send(404, {{"content-type", "text/plain; charset=utf-8"}},
                                    std::string(remote::NOT_AN_ENDPOINT_BODY));
                    return;
                }
                // websockets refuses a plain request on the endpoint as it parses the upgrade.
                std::string problem;
                const auto connection = net::header(request.headers, "connection");
                const auto upgrade = net::header(request.headers, "upgrade");
                if (!connection) {
                    problem = "missing Connection header";
                } else if (py::lower(*connection).find("upgrade") == std::string::npos) {
                    problem = "invalid Connection header: " + std::string(*connection);
                } else if (!upgrade) {
                    problem = "missing Upgrade header";
                } else {
                    problem = "invalid Upgrade header: " + std::string(*upgrade);
                }
                responder->send(
                    426, {{"upgrade", "websocket"}, {"content-type", "text/plain; charset=utf-8"}},
                    "Failed to open a WebSocket connection: " + problem + ".\n");
            });
        auto endpoint = server->listen(net::Listen{.address = bind,
                                                   .port = static_cast<std::uint16_t>(port),
                                                   .tls = impl.deps.identity.tls});
        if (!endpoint) {
            return std::unexpected(endpoint.error());
        }
        bound = endpoint->port;
        impl.server = std::move(server);
        impl.port = bound;
        if (impl.deps.history) {
            impl.deps.history->start();
        }
        impl.activity.reset();
    }
    logger.info("Remote engine host listening on {}:{}", bind, bound);
    if (discovery_port && impl.deps.discovery) {
        impl.responder.start(impl.deps.discovery, discovery_port, bind);
    }
    impl.emit("state", Json::object());
    return bound;
}

void SpeechHost::stop() {
    Impl& impl = *impl_;
    if (impl.deps.history) {
        impl.deps.history->remove(std::nullopt);
    }
    impl.responder.stop();
    std::unique_ptr<net::HttpServer> server;
    {
        std::lock_guard lock(impl.state_mutex);
        server = std::move(impl.server);
        impl.port.reset();
    }
    impl.pairing.stop(impl.deps.limits.stop_grace);
    if (!server) {
        return;
    }
    impl.admission.cancel_all();
    // websockets' shutdown: every open connection is closed 1001, then the server waits for
    // their handlers.
    std::vector<std::shared_ptr<HostConnection>> open;
    {
        std::lock_guard lock(impl.conns_mutex);
        for (const auto& [key, conn] : impl.conns) {
            open.push_back(conn);
        }
    }
    for (const auto& conn : open) {
        conn->ws->close(1001, "");
    }
    server->stop(impl.deps.limits.stop_grace);
    server.reset();
    // Turning sharing off must cut off computers already connected.
    std::vector<std::shared_ptr<net::WsConnection>> sockets;
    {
        std::lock_guard lock(impl.state_mutex);
        for (const auto& [id, client] : impl.clients) {
            sockets.push_back(client.ws);
        }
        impl.clients.clear();
        impl.update_token.clear();
        impl.update_until = {};
        impl.reconnecting.clear();
    }
    for (const auto& ws : sockets) {
        ws->close(1001, "host stopped sharing");
    }
    logger.info("Remote engine host stopped");
    impl.emit("state", Json::object());
}

bool SpeechHost::running() const {
    return impl_->running();
}

std::optional<std::uint16_t> SpeechHost::port() const {
    return impl_->port_now();
}

std::string SpeechHost::open_pairing() {
    return open_pairing(new_pairing_code());
}

std::string SpeechHost::open_pairing(std::string code) {
    impl_->pairing.open(code, impl_->deps.limits.pairing_ttl_s);
    Json info = Json::object();
    info["open"] = true;
    impl_->emit("pairing", std::move(info));
    return code;
}

void SpeechHost::close_pairing() {
    if (impl_->pairing.close()) {
        Json info = Json::object();
        info["open"] = false;
        impl_->emit("pairing", std::move(info));
    }
}

std::optional<std::pair<std::string, double>> SpeechHost::pairing_status() {
    return impl_->pairing.status();
}

std::optional<remote::PairRequestInfo> SpeechHost::pending_request() {
    return impl_->pairing.pending();
}

bool SpeechHost::answer_request(std::string_view id, bool allow) {
    return impl_->pairing.answer(id, allow);
}

std::vector<remote::ConnectedClient> SpeechHost::connected_clients() {
    std::lock_guard lock(impl_->state_mutex);
    std::vector<remote::ConnectedClient> out;
    for (const auto& [id, client] : impl_->clients) {
        remote::ConnectedClient entry;
        entry.device_id = client.device_id;
        entry.name = client.name;
        entry.address = client.address;
        entry.busy = client.busy;
        entry.since = client.since;
        entry.recording = !client.recording_id.empty();
        out.push_back(std::move(entry));
    }
    return out;
}

bool SpeechHost::update_active() {
    return impl_->update_active();
}

Json SpeechHost::update_status() {
    Impl& impl = *impl_;
    std::lock_guard lock(impl.state_mutex);
    const auto now = impl.deps.clock.steady();
    std::int64_t recordings = 0;
    std::int64_t requests = 0;
    std::int64_t untracked = 0;
    for (const auto& [id, client] : impl.clients) {
        recordings += client.recording_id.empty() ? 0 : 1;
        requests += client.in_request ? 1 : 0;
        untracked += client.reports_recording ? 0 : 1;
    }
    Json out = Json::object();
    out["recordings"] = recordings;
    out["requests"] = requests;
    out["untracked_clients"] = untracked;
    out["reconnecting_recording"] = impl.recording_reconnecting_locked(now);
    out["maintenance"] = impl.update_active_locked(now);
    return out;
}

Json SpeechHost::prepare_update() {
    Impl& impl = *impl_;
    const bool settled = impl.dispatcher.engine_settled();
    std::lock_guard lock(impl.state_mutex);
    const auto now = impl.deps.clock.steady();
    Json out = Json::object();
    if (!impl.server || !settled) {
        out["reason"] = "The speech host is starting or reloading";
        return out;
    }
    if (impl.update_active_locked(now)) {
        out["reason"] = "Another update already reserved the host";
        return out;
    }
    const bool busy = impl.recording_reconnecting_locked(now) ||
                      std::ranges::any_of(impl.clients, [](const auto& entry) {
                          return !entry.second.recording_id.empty() || entry.second.in_request;
                      });
    if (busy) {
        out["reason"] = "A paired client is recording or using the speech host";
        return out;
    }
    if (std::ranges::any_of(impl.clients,
                            [](const auto& entry) { return !entry.second.reports_recording; })) {
        out["reason"] = "A connected client cannot report its recording state";
        return out;
    }
    impl.update_token = random_hex(32);
    impl.update_until = now + impl.deps.limits.update_hold;
    out["token"] = impl.update_token;
    out["expires_in"] = std::chrono::duration<double>(impl.deps.limits.update_hold).count();
    return out;
}

bool SpeechHost::cancel_update(std::string_view token) {
    Impl& impl = *impl_;
    std::lock_guard lock(impl.state_mutex);
    if (token.empty() || token != impl.update_token) {
        return false;
    }
    impl.update_token.clear();
    impl.update_until = {};
    return true;
}

int SpeechHost::engine_changed() {
    assert_not_gui_thread();
    Impl& impl = *impl_;
    const auto current = impl.dispatcher.engine()->identity();
    std::vector<std::shared_ptr<net::WsConnection>> stale;
    {
        std::lock_guard lock(impl.state_mutex);
        for (const auto& [id, client] : impl.clients) {
            if (client.engine_identity != current && !client.in_request) {
                stale.push_back(client.ws);
            }
        }
    }
    for (const auto& ws : stale) {
        ws->close(remote::CLOSE_ENGINE_CHANGED, "engine changed");
    }
    if (!stale.empty()) {
        logger.info("Engine changed; told {} paired computer(s) to reconnect", stale.size());
    }
    return static_cast<int>(stale.size());
}

bool SpeechHost::remove_device(std::string_view id) {
    Impl& impl = *impl_;
    const bool removed = impl.deps.registry.remove(id);
    if (impl.deps.history) {
        impl.deps.history->remove(std::string(id));
    }
    std::vector<std::shared_ptr<net::WsConnection>> sockets;
    {
        std::lock_guard lock(impl.state_mutex);
        for (const auto& [connection_id, client] : impl.clients) {
            if (client.device_id == id) {
                sockets.push_back(client.ws);
            }
        }
    }
    for (const auto& ws : sockets) {
        ws->close(remote::CLOSE_UNAUTHORIZED, "device removed");
    }
    if (removed) {
        impl.emit("devices", Json::object());
    }
    return removed;
}

std::optional<remote::PairedDevice> SpeechHost::rename_device(std::string_view id,
                                                              std::string_view name) {
    Impl& impl = *impl_;
    auto device = impl.deps.registry.rename(id, name);
    if (!device) {
        return std::nullopt;
    }
    {
        std::lock_guard lock(impl.state_mutex);
        for (auto& [connection_id, client] : impl.clients) {
            if (client.device_id == id) {
                client.name = device->name;
            }
        }
    }
    impl.activity.renamed(id, device->name);
    impl.emit("devices", Json::object());
    impl.emit("clients", Json::object());
    return device;
}

HostActivity& SpeechHost::activity() {
    return impl_->activity;
}

std::string SpeechHost::discovery_error() const {
    return impl_->responder.error();
}

std::optional<std::uint16_t> SpeechHost::discovery_port() const {
    return impl_->responder.port();
}

int SpeechHost::free_connection_slots() const {
    return impl_->connection_slots.available();
}

int SpeechHost::free_handshake_slots() const {
    return impl_->handshake_slots.available();
}

std::size_t SpeechHost::queued_jobs() const {
    return impl_->admission.queued();
}

} // namespace fv::remote_host
