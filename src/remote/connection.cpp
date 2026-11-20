#include "remote/connection.h"

#include <algorithm>
#include <condition_variable>
#include <exception>
#include <format>
#include <map>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <utility>

#include "base/executor.h"
#include "base/identity.h"
#include "base/log.h"
#include "net/tailscale.h"
#include "pyport/json.h"
#include "pyport/text.h"
#include "remote/detail/client_socket.h"
#include "remote/detail/pyvalue.h"
#include "remote/discovery_search.h"
#include "remote/errors.h"
#include "remote/protocol.h"
#include "remote/wire.h"

namespace fv::remote {

namespace {

constexpr log::Logger kLog("services.remote_asr.client");

using detail::ClientSocket;
using Clock = std::chrono::steady_clock;

// Nothing, or some other computer, answered at this address; try the next.
class WrongPlace : public RemoteEngineError {
public:
    using RemoteEngineError::RemoteEngineError;
};

// RemoteConnection's class attributes _last_good and _searched_at, under one lock.
struct Routes {
    std::mutex mutex;
    std::map<std::string, std::string> last_good;
    std::map<std::string, Clock::time_point> searched_at;
};

Routes& routes() {
    static Routes shared;
    return shared;
}

const Json& member(const Json& object, std::string_view key) {
    static const Json none;
    if (!object.is_object()) {
        return none;
    }
    const auto found = object.find(std::string(key));
    return found == object.end() ? none : *found;
}

// _host_seconds: the time the host says it spent on a request; nullopt from a host too old to
// say. isinstance(host_ms, (int, float)) admits a bool, as Python's does.
std::optional<double> host_seconds(const Json& reply) {
    const Json& host_ms = member(reply, "host_ms");
    double value = 0.0;
    if (host_ms.is_boolean()) {
        value = host_ms.get<bool>() ? 1.0 : 0.0;
    } else if (host_ms.is_number()) {
        value = host_ms.get<double>();
    } else {
        return std::nullopt;
    }
    if (!(value >= 0)) {
        return std::nullopt;
    }
    return value / 1000;
}

std::runtime_error canceled() {
    return std::runtime_error("Transcription canceled");
}

double seconds(std::chrono::nanoseconds duration) {
    return std::chrono::duration<double>(duration).count();
}

// errors as Python's dict keeps them: insertion order, a second error for an address replacing
// the first in place.
class Errors {
public:
    void set(const std::string& host, std::exception_ptr error) {
        for (auto& [key, value] : items_) {
            if (key == host) {
                value = std::move(error);
                return;
            }
        }
        items_.emplace_back(host, std::move(error));
    }
    // errors.get(paired) or the first one.
    [[noreturn]] void raise(const std::string& paired) const {
        for (const auto& [key, value] : items_) {
            if (key == paired) {
                std::rethrow_exception(value);
            }
        }
        if (items_.empty()) {
            throw RemoteEngineError("The host closed the connection.");
        }
        std::rethrow_exception(items_.front().second);
    }

private:
    std::vector<std::pair<std::string, std::exception_ptr>> items_;
};

} // namespace

namespace {

std::unique_lock<std::timed_mutex> lock_request(std::timed_mutex& mutex,
                                                const CancelToken& cancel) {
    std::unique_lock lock(mutex, std::defer_lock);
    while (!cancel.cancelled()) {
        if (lock.try_lock_for(REQUEST_POLL) && !cancel.cancelled()) {
            return lock;
        }
    }
    throw canceled();
}

} // namespace

struct RemoteConnection::State {
    ClientEnv& env;
    std::vector<std::string> hosts;
    std::string token;
    std::string fingerprint; // uppercase
    std::chrono::milliseconds timeout;
    std::optional<bool> history_enabled;
    bool reports_recording;

    // One request or handshake in flight (Python's _lock); a timed one for recording_activity.
    std::timed_mutex request_lock;

    // Python's _state_lock: everything below.
    mutable std::mutex mutex;
    std::condition_variable_any stop_changed;
    std::string host;
    int port;
    int serial = 0;
    // Requests up to this serial were given up on; their replies are skipped.
    int abandoned = 0;
    bool closed = false;
    bool stopped = false; // the keepalive's stop
    std::shared_ptr<ClientSocket> socket;
    std::optional<double> rtt;
    int beats = 0;
    Json ready = Json::object();
    std::vector<std::thread> keepalives;

    State(ClientEnv& env_, std::string host_, int port_)
        : env(env_), host(std::move(host_)), port(port_) {}

    std::string where() const {
        std::lock_guard lock(mutex);
        return format_address(host, port);
    }

    std::vector<std::string> candidates() const {
        std::optional<std::string> preferred;
        {
            std::lock_guard lock(routes().mutex);
            if (const auto found = routes().last_good.find(fingerprint);
                found != routes().last_good.end()) {
                preferred = found->second;
            }
        }
        std::lock_guard lock(mutex);
        if (preferred && std::ranges::find(hosts, *preferred) != hosts.end()) {
            std::vector<std::string> ordered{*preferred};
            for (const auto& candidate : hosts) {
                if (candidate != *preferred) {
                    ordered.push_back(candidate);
                }
            }
            return ordered;
        }
        return hosts;
    }

    void mark_closed(const std::shared_ptr<ClientSocket>& expected = {}) {
        std::shared_ptr<ClientSocket> socket_now;
        {
            std::lock_guard lock(mutex);
            if (expected && socket != expected) {
                return;
            }
            closed = true;
            stopped = true;
            socket_now = std::move(socket);
        }
        stop_changed.notify_all();
        if (socket_now) {
            detail::quiet_close(*socket_now);
        }
    }

    // _search_network: where the host answers discovery now, if that's somewhere not yet tried.
    std::optional<LanHost> search_network(const std::vector<std::string>& tried) {
        if (fingerprint.empty()) {
            return std::nullopt;
        }
        const auto now = Clock::now();
        {
            std::lock_guard lock(routes().mutex);
            const auto last = routes().searched_at.find(fingerprint);
            if (last != routes().searched_at.end() && now - last->second < REDISCOVER_INTERVAL) {
                return std::nullopt;
            }
            routes().searched_at[fingerprint] = now;
        }
        std::optional<LanHost> found;
        try {
            found = find_host(env, fingerprint);
        } catch (const std::exception& error) {
            kLog.debug("Looking for the remote engine on the network failed: {}",
                       detail::exception_class(error));
            return std::nullopt;
        }
        int current_port = 0;
        {
            std::lock_guard lock(mutex);
            current_port = port;
        }
        if (!found || (std::ranges::find(tried, found->address) != tried.end() &&
                       found->port == current_port)) {
            return std::nullopt;
        }
        return found;
    }

    void arrived(const std::string& at, const Json& reply, bool remember_route) {
        if (remember_route) {
            std::lock_guard lock(routes().mutex);
            routes().last_good[fingerprint] = at;
        }
        const Json& addresses = member(member(reply, "host"), "addresses");
        if (!addresses.is_array()) {
            return;
        }
        std::vector<std::string> listed;
        for (const Json& address : addresses) {
            if (address.is_string() && !address.get_ref<const std::string&>().empty()) {
                listed.push_back(address.get<std::string>());
            }
        }
        int current_port = 0;
        {
            std::lock_guard lock(mutex);
            current_port = port;
        }
        try {
            // The host lists where it is now; a saved pairing follows it.
            if (env.remember_addresses) {
                env.remember_addresses(fingerprint, at, current_port, listed);
            }
        } catch (const std::exception& error) {
            // Type only: the pairing being saved holds the access token.
            kLog.warning("Could not save the remote engine's addresses: {}",
                         detail::exception_class(error));
        }
    }

    Json connect_to(const std::string& at, std::chrono::milliseconds wait,
                    const std::shared_ptr<State>& self, const CancelToken& cancel) {
        if (cancel.cancelled()) {
            throw canceled();
        }
        int current_port = 0;
        {
            std::lock_guard lock(mutex);
            current_port = port;
        }
        std::shared_ptr<ClientSocket> opened;
        try {
            opened = detail::open_engine_socket(env.reactor, at, current_port, wait);
        } catch (const RemoteEngineError& error) {
            throw WrongPlace(error.what());
        }
        const std::string where_now = format_address(at, current_port);
        Json reply;
        try {
            const std::string seen = opened->fingerprint();
            if (fingerprint.empty() || !detail::compare_digest(seen, fingerprint)) {
                throw WrongPlace(std::format(
                    "{} presented a different certificate than when this computer paired with "
                    "it. If {} was reinstalled there, pair again; otherwise something is "
                    "intercepting the connection.",
                    where_now, identity().product));
            }
            Json hello = {{"type", "hello"}, {"protocol", PROTOCOL_VERSION}, {"token", token}};
            if (history_enabled) {
                hello["purpose"] = "history";
                hello["history_enabled"] = *history_enabled;
            } else if (reports_recording) {
                hello["capabilities"] = Json{{"recording_activity", true}};
            }
            // A full host can send its refusal and close before hello: a failed send is no
            // error, and the queued reply still brings its retry delay to the caller.
            if (cancel.cancelled()) {
                throw canceled();
            }
            (void)opened->send_text(dumps_default(hello));
            reply = detail::read_json(*opened, HANDSHAKE_TIMEOUT, cancel);
            if (member(reply, "type") == "error") {
                if (member(reply, "code") == "busy") {
                    throw RemoteHostBusy(detail::truthy(member(reply, "message"))
                                             ? detail::py_str(member(reply, "message"))
                                             : std::string("The host is busy. Try again shortly."),
                                         member(reply, "retry_after_ms"));
                }
                throw RemoteEngineError(detail::truthy(member(reply, "message"))
                                            ? detail::py_str(member(reply, "message"))
                                            : std::string("The host refused the connection."));
            }
            if (member(reply, "type") != "ready") {
                throw RemoteEngineError("The host sent a reply this version can't read.");
            }
        } catch (...) {
            detail::quiet_close(*opened);
            throw;
        }
        bool was_closed = false;
        {
            std::lock_guard lock(mutex);
            was_closed = closed || cancel.cancelled();
            if (!was_closed) {
                socket = opened;
                host = at;
                ready = reply;
                rtt.reset();
                beats = 0;
                stopped = false;
            }
        }
        if (was_closed) {
            detail::quiet_close(*opened);
            throw canceled();
        }
        stop_changed.notify_all();
        std::lock_guard lock(mutex);
        keepalives.emplace_back([self, opened] { keepalive(self, opened); });
        return reply;
    }

    // _heartbeat: a ping now and every keepalive_interval, timing each pong; a pong missing for
    // keepalive_timeout closes the connection, as websockets' keepalive would. It stands in for
    // that keepalive because the round trip is wanted, timed with a steady clock.
    static void keepalive(const std::shared_ptr<State>& self,
                          const std::shared_ptr<ClientSocket>& socket) {
        struct Pong {
            std::mutex mutex;
            std::condition_variable changed;
            bool done = false;
            std::optional<std::chrono::nanoseconds> round_trip;
        };
        while (true) {
            {
                std::lock_guard lock(self->mutex);
                if (self->stopped || self->socket != socket) {
                    return;
                }
            }
            auto pong = std::make_shared<Pong>();
            socket->ping(self->env.keepalive_timeout,
                         [pong](std::optional<std::chrono::nanoseconds> round_trip) {
                             {
                                 std::lock_guard lock(pong->mutex);
                                 pong->done = true;
                                 pong->round_trip = round_trip;
                             }
                             pong->changed.notify_all();
                         });
            {
                // The ping reports within its timeout; the margin covers a reactor that stopped.
                std::unique_lock lock(pong->mutex);
                pong->changed.wait_for(lock, self->env.keepalive_timeout + std::chrono::seconds(1),
                                       [&] { return pong->done; });
            }
            {
                std::lock_guard lock(self->mutex);
                if (self->stopped || self->socket != socket) {
                    return;
                }
            }
            if (!socket->is_open()) {
                return;
            }
            std::optional<std::chrono::nanoseconds> round_trip;
            {
                std::lock_guard lock(pong->mutex);
                round_trip = pong->round_trip;
            }
            if (!round_trip) {
                kLog.info("Remote engine at {} missed a keepalive ping; closing", self->where());
                self->mark_closed(socket);
                return;
            }
            std::unique_lock lock(self->mutex);
            const auto inactive = [&] {
                return self->stopped || self->socket != socket;
            };
            if (inactive()) {
                return;
            }
            self->rtt = seconds(*round_trip);
            ++self->beats;
            self->stop_changed.wait_for(lock, self->env.keepalive_interval, inactive);
        }
    }

    // _send_request: sends one request; the socket it went out on and its serial.
    std::pair<std::shared_ptr<ClientSocket>, int> send_request(std::string_view op,
                                                               std::span<const float> audio,
                                                               std::span<const std::byte> payload,
                                                               const Json& fields) {
        std::shared_ptr<ClientSocket> on;
        int number = 0;
        std::string where_now;
        {
            std::lock_guard lock(mutex);
            where_now = format_address(host, port);
            if (closed || !socket) {
                throw RemoteConnectionLost(std::format("Lost the connection to {}.", where_now),
                                           false);
            }
            on = socket;
            number = ++serial;
        }
        Json header = {{"id", number}, {"op", std::string(op)}};
        if (fields.is_object()) {
            for (const auto& [key, value] : fields.items()) {
                header[key] = value;
            }
        }
        std::vector<std::byte> frame;
        if (!audio.empty()) {
            frame = pack_request(header, audio);
        } else if (!payload.empty()) {
            frame = pack_request(header, payload);
        } else {
            frame = pack_request(header);
        }
        if (!on->send_binary(std::move(frame))) {
            mark_closed();
            throw RemoteConnectionLost(std::format("Lost the connection to {}.", where_now), false);
        }
        return {on, number};
    }

    // _receive: the next frame, or nullopt when a poll interval passed without one.
    std::optional<detail::Message> receive(ClientSocket& on, int number, Clock::time_point deadline,
                                           const CancelToken& cancel) {
        std::string where_now;
        {
            std::lock_guard lock(mutex);
            if (abandoned >= number) {
                throw canceled();
            }
            where_now = format_address(host, port);
        }
        const auto remaining = deadline - Clock::now();
        if (remaining <= Clock::duration::zero()) {
            close();
            throw std::runtime_error(std::format(
                "The remote engine on {} timed out; the connection was closed.", where_now));
        }
        const auto poll = std::min<std::chrono::milliseconds>(
            REQUEST_POLL, std::chrono::ceil<std::chrono::milliseconds>(remaining));
        auto received = on.receive(poll, cancel);
        const bool now_closed = closed_now();
        if (!received) {
            if (received.error() == detail::ReceiveError::canceled) {
                throw canceled();
            }
            if (received.error() == detail::ReceiveError::timeout) {
                if (now_closed) {
                    throw canceled();
                }
                return std::nullopt;
            }
            if (now_closed) {
                throw canceled();
            }
            mark_closed();
            throw RemoteConnectionLost(std::format("Lost the connection to {}.", where_now), true);
        }
        if (now_closed) {
            throw canceled();
        }
        return std::move(*received);
    }

    // _await_reply: the reply to request `number`; host errors are raised.
    Json await_reply(ClientSocket& on, int number, Clock::time_point deadline,
                     const CancelToken& cancel = {}) {
        while (true) {
            if (cancel.cancelled()) {
                throw canceled();
            }
            auto raw = receive(on, number, deadline, cancel);
            if (cancel.cancelled()) {
                throw canceled();
            }
            if (!raw) {
                continue;
            }
            auto reply = detail::loads_message(*raw);
            // A reply to an abandoned request arrives late and is skipped here, by the next
            // request, since its id is older.
            if (!reply || !reply->is_object() || !py::py_equal(member(*reply, "id"), number)) {
                continue;
            }
            if (member(*reply, "code") == "engine_changed") {
                mark_closed();
                throw RemoteConnectionLost(detail::py_str(member(*reply, "error")), true);
            }
            if (reply->contains("error")) {
                throw RemoteRequestError(detail::py_str((*reply)["error"]), member(*reply, "code"),
                                         member(*reply, "retry_after_ms"));
            }
            return std::move(*reply);
        }
    }

    bool closed_now() const {
        std::lock_guard lock(mutex);
        return closed;
    }

    void close() {
        {
            std::lock_guard lock(mutex);
            if (closed) {
                return;
            }
        }
        mark_closed();
    }
};

RemoteConnection::RemoteConnection(ClientEnv& env, std::string host, int port, std::string token,
                                   std::string fingerprint, std::vector<std::string> alternates,
                                   std::optional<bool> history_enabled, bool reports_recording,
                                   std::chrono::milliseconds timeout)
    : state_(std::make_shared<State>(env, host, port)) {
    auto& state = *state_;
    // dict.fromkeys([host, *[a for a in alternates if a]])
    state.hosts.push_back(host);
    for (auto& alternate : alternates) {
        if (!alternate.empty() && std::ranges::find(state.hosts, alternate) == state.hosts.end()) {
            state.hosts.push_back(std::move(alternate));
        }
    }
    state.token = std::move(token);
    state.fingerprint = py::upper(fingerprint);
    state.timeout = timeout;
    state.history_enabled = history_enabled;
    state.reports_recording = reports_recording;
}

RemoteConnection::~RemoteConnection() {
    state_->close();
    {
        std::lock_guard lock(state_->mutex);
        state_->stopped = true;
    }
    state_->stop_changed.notify_all();
    std::vector<std::thread> keepalives;
    {
        std::lock_guard lock(state_->mutex);
        keepalives = std::move(state_->keepalives);
    }
    for (auto& thread : keepalives) {
        if (thread.joinable()) {
            thread.join();
        }
    }
}

Json RemoteConnection::connect(const CancelToken& cancel) {
    assert_not_gui_thread();
    auto& state = *state_;
    const auto one_at_a_time = lock_request(state.request_lock, cancel);
    {
        std::lock_guard lock(state.mutex);
        if (state.closed) {
            throw canceled();
        }
        if (state.socket && state.socket->is_open()) {
            return state.ready;
        }
    }
    const std::vector<std::string> candidates = state.candidates();
    Errors errors;
    for (std::size_t index = 0; index < candidates.size(); ++index) {
        const bool final = index + 1 == candidates.size();
        const auto wait = final ? state.timeout : std::min(state.timeout, FALLBACK_CONNECT_TIMEOUT);
        Json reply;
        try {
            reply = state.connect_to(candidates[index], wait, state_, cancel);
        } catch (const WrongPlace& error) {
            errors.set(candidates[index], std::current_exception());
            if (!final) {
                kLog.info("Remote engine not at {} ({}); trying {}", candidates[index],
                          error.what(), candidates[index + 1]);
            }
            continue;
        }
        state.arrived(candidates[index], reply, candidates.size() > 1);
        return reply;
    }
    if (cancel.cancelled()) {
        throw canceled();
    }
    if (const auto moved = state.search_network(candidates)) {
        kLog.info("Remote engine isn't at its saved addresses; trying {}, where it answered",
                  moved->where());
        int previous = 0;
        {
            std::lock_guard lock(state.mutex);
            previous = state.port;
            state.port = moved->port;
        }
        try {
            Json reply = state.connect_to(moved->address, state.timeout, state_, cancel);
            {
                std::lock_guard lock(state.mutex);
                state.hosts.push_back(moved->address);
            }
            state.arrived(moved->address, reply, true);
            return reply;
        } catch (const WrongPlace&) {
            {
                std::lock_guard lock(state.mutex);
                state.port = previous;
            }
            errors.set(moved->address, std::current_exception());
        }
    }
    // Report the address paired with; the others were a bonus.
    std::string paired;
    {
        std::lock_guard lock(state.mutex);
        paired = state.hosts.front();
    }
    errors.raise(paired);
}

Json RemoteConnection::request(std::string_view op, const Json& fields,
                               std::chrono::milliseconds timeout, std::span<const float> audio,
                               std::span<const std::byte> payload) {
    return request_timed(op, fields, timeout, audio, payload).first;
}

std::pair<Json, std::optional<double>>
RemoteConnection::request_timed(std::string_view op, const Json& fields,
                                std::chrono::milliseconds timeout, std::span<const float> audio,
                                std::span<const std::byte> payload, const CancelToken& cancel) {
    assert_not_gui_thread();
    auto& state = *state_;
    const auto one_at_a_time = lock_request(state.request_lock, cancel);
    if (cancel.cancelled()) {
        throw canceled();
    }
    auto [on, number] = state.send_request(op, audio, payload, fields);
    const auto deadline = Clock::now() + timeout;
    const Json reply = state.await_reply(*on, number, deadline, cancel);
    const Json& result = member(reply, "result");
    return {result.is_object() ? result : Json::object(), host_seconds(reply)};
}

void RemoteConnection::recording_activity(std::string_view op, std::string_view recording_id) {
    assert_not_gui_thread();
    auto& state = *state_;
    const auto deadline = Clock::now() + RECORDING_WAIT;
    std::unique_lock one_at_a_time(state.request_lock, std::defer_lock);
    if (!one_at_a_time.try_lock_for(RECORDING_WAIT)) {
        throw RemoteEngineError("The remote speech connection is busy. Try again shortly.");
    }
    auto [on, number] =
        state.send_request(op, {}, {}, Json{{"recording_id", std::string(recording_id)}});
    (void)state.await_reply(*on, number, deadline);
}

std::optional<std::string> RemoteConnection::receive_history(std::chrono::milliseconds timeout) {
    assert_not_gui_thread();
    std::shared_ptr<ClientSocket> on;
    {
        std::lock_guard lock(state_->mutex);
        if (!state_->history_enabled || state_->closed || !state_->socket) {
            throw RemoteEngineError("The client history connection is closed.");
        }
        on = state_->socket;
    }
    auto received = on->receive(timeout);
    if (!received) {
        if (received.error() == detail::ReceiveError::timeout) {
            return std::nullopt;
        }
        throw RemoteEngineError("The host closed the connection.");
    }
    if (received->binary) {
        throw RemoteEngineError("The host sent a reply this version can't read.");
    }
    return std::move(received->data);
}

void RemoteConnection::send_history(std::string text) {
    std::shared_ptr<ClientSocket> on;
    {
        std::lock_guard lock(state_->mutex);
        if (!state_->history_enabled || state_->closed || !state_->socket) {
            throw RemoteEngineError("The client history connection is closed.");
        }
        on = state_->socket;
    }
    if (!on->send_text(std::move(text))) {
        throw RemoteEngineError("The client history connection is closed.");
    }
}

void RemoteConnection::abandon() {
    std::lock_guard lock(state_->mutex);
    state_->abandoned = state_->serial;
}

void RemoteConnection::close() {
    state_->close();
}

void RemoteConnection::watch_closed(std::function<void(std::uint16_t)> observer) {
    std::shared_ptr<ClientSocket> authenticated;
    {
        std::lock_guard lock(state_->mutex);
        authenticated = state_->socket;
    }
    if (authenticated) {
        authenticated->watch_closed(std::move(observer));
    } else {
        observer(1006);
    }
}

bool RemoteConnection::closed() const {
    return state_->closed_now();
}

bool RemoteConnection::alive() const {
    std::shared_ptr<ClientSocket> on;
    {
        std::lock_guard lock(state_->mutex);
        if (state_->closed) {
            return false;
        }
        on = state_->socket;
    }
    return on && on->is_open();
}

bool RemoteConnection::closed_for_engine_change() const {
    std::shared_ptr<ClientSocket> on;
    {
        std::lock_guard lock(state_->mutex);
        on = state_->socket;
    }
    return on && on->close_code() == CLOSE_ENGINE_CHANGED;
}

std::optional<double> RemoteConnection::latency() const {
    std::lock_guard lock(state_->mutex);
    return state_->rtt;
}

int RemoteConnection::beats() const {
    std::lock_guard lock(state_->mutex);
    return state_->beats;
}

bool RemoteConnection::via_tailscale() const {
    std::shared_ptr<ClientSocket> on;
    std::string peer;
    {
        std::lock_guard lock(state_->mutex);
        on = state_->socket;
        peer = state_->host;
    }
    if (on) {
        if (std::string address = on->peer_address(); !address.empty()) {
            peer = std::move(address);
        }
    }
    return net::is_tailscale_address(peer);
}

std::string RemoteConnection::where() const {
    return state_->where();
}

Json RemoteConnection::ready() const {
    std::lock_guard lock(state_->mutex);
    return state_->ready;
}

std::string RemoteConnection::host() const {
    std::lock_guard lock(state_->mutex);
    return state_->host;
}

int RemoteConnection::port() const {
    std::lock_guard lock(state_->mutex);
    return state_->port;
}

void RemoteConnection::clear_route_memory() {
    std::lock_guard lock(routes().mutex);
    routes().last_good.clear();
    routes().searched_at.clear();
}

} // namespace fv::remote
