#pragma once

#include <chrono>
#include <cstddef>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "base/json.h"
#include "remote/client.h"

// One authenticated connection to a paired host (services/remote_asr/client.py's
// RemoteConnection): the speech connection of the dictation and meeting backends, the one-shot
// management and record-sync connections, and the direct-history channel.
namespace fv::remote {

namespace detail {
class ClientSocket;
}

// `alternates` are the host's other addresses (its LAN and Tailscale IPs). They are tried in
// order when `host` doesn't answer, or answers with a different certificate, which is what a
// laptop away from home sees at its home LAN address. The token is only ever sent after the
// pinned certificate matches, whichever address answered. When no known address has the host,
// it is looked for on this network by its certificate (find_host), which follows a host the
// router gave a new address; a forged discovery answer gets no token either.
//
// The connection reads close frames on the reactor, and a keepalive thread pings every
// keepalive_interval, so alive(), latency() and beats() describe the socket between requests
// without anything more going over it.
//
// Every connection of the process shares two memories (one lock): the address that last worked
// for each host certificate, tried first next time so only the first connection pays for a dead
// address, and when each certificate was last looked for on the network, so a host that is
// simply off isn't searched for on every retry.
// Affinity: thread-safe; one request in flight at a time (the others wait); connect(),
// request(), recording_activity() and the history calls block on the network and never run on
// the GUI thread or the reactor; the readers answer from memory. `env` must outlive it.
class RemoteConnection {
public:
    RemoteConnection(ClientEnv& env, std::string host, int port, std::string token,
                     std::string fingerprint, std::vector<std::string> alternates = {},
                     std::optional<bool> history_enabled = {}, bool reports_recording = false,
                     std::chrono::milliseconds timeout = CONNECT_TIMEOUT);
    // close(), then waits for the keepalive thread.
    ~RemoteConnection();
    RemoteConnection(const RemoteConnection&) = delete;
    RemoteConnection& operator=(const RemoteConnection&) = delete;

    // Opens, checks the pinned certificate and authenticates. If already open, returns the
    // existing `ready`. RemoteHostBusy when
    // the host is full, RemoteEngineError with the host's message when it refuses, else the
    // error of the address paired with (the others were a bonus). std::runtime_error
    // "Transcription canceled" when close() came first.
    Json connect(const CancelToken& cancel = {});

    // One request: a binary frame {"id", "op", ...fields} with the audio as PCM16 or `payload`
    // as it is, then the reply's `result` (an empty object when it has none). The reply holding
    // `error` is RemoteRequestError; `engine_changed` closes and is RemoteConnectionLost(sent);
    // a failed send RemoteConnectionLost(not sent), a failed receive RemoteConnectionLost(sent);
    // past `timeout` the connection is closed: "The remote engine on <where> timed out; the
    // connection was closed."; abandon() or close() meanwhile: "Transcription canceled".
    Json request(std::string_view op, const Json& fields = Json::object(),
                 std::chrono::milliseconds timeout = std::chrono::milliseconds(180'000),
                 std::span<const float> audio = {}, std::span<const std::byte> payload = {});
    // request(), plus the seconds the host says it spent on it (nullopt from a host too old to
    // report it).
    std::pair<Json, std::optional<double>>
    request_timed(std::string_view op, const Json& fields = Json::object(),
                  std::chrono::milliseconds timeout = std::chrono::milliseconds(180'000),
                  std::span<const float> audio = {}, std::span<const std::byte> payload = {},
                  const CancelToken& cancel = {});
    // `recording_begin` or `recording_end` for `recording_id`: a capture start waits only
    // briefly, RECORDING_WAIT for the request lock ("The remote speech connection is busy. Try
    // again shortly.") and for the reply.
    void recording_activity(std::string_view op, std::string_view recording_id);

    // The direct-history channel (history_enabled set): the next text frame, nullopt after
    // `timeout`. RemoteEngineError "The client history connection is closed." on a speech
    // connection or once closed here, "The host closed the connection." when the host did, and
    // "The host sent a reply this version can't read." for a binary frame (each ends the
    // session, as a frame that is not a str does in Python).
    std::optional<std::string>
    receive_history(std::chrono::milliseconds timeout = std::chrono::milliseconds(1'000));
    // Sends one text frame; RemoteEngineError "The client history connection is closed." when
    // it can't.
    void send_history(std::string text);

    // Stops waiting for the request in flight but keeps the connection: the host finishes that
    // request either way, since a disconnect doesn't interrupt a decode, so closing would only
    // cost the next dictation a reconnect. Its late reply is skipped by the next request.
    void abandon();
    // Drops the connection; a request waiting on another thread is canceled.
    void close();

    bool closed() const;
    // Open as far as this computer can tell, without sending anything. A host that quits or
    // stops sharing closes the socket at once; one that sleeps or leaves the network is noticed
    // when a keepalive ping goes unanswered, 20 to 40 s later.
    bool alive() const;
    // The host closed this connection because it switched engines (close code 4409).
    bool closed_for_engine_change() const;
    // Watches the current authenticated socket only. The observer does not open a connection.
    void watch_closed(std::function<void(std::uint16_t)>);
    // The last keepalive's round trip in seconds (steady clock); nullopt before one returns.
    std::optional<double> latency() const;
    // Keepalive pongs so far: one right after connecting, then every keepalive_interval.
    int beats() const;
    // Whether the address that answered is on the tailnet, judged by the peer's IP rather than
    // the name paired with, which may be a MagicDNS or LAN host name.
    bool via_tailscale() const;
    std::string where() const; // format_address(host(), port())
    // A snapshot of the host's `ready` (an empty object before connect()).
    Json ready() const;
    // The address that answered (the one paired with until then), and its port, which a host
    // found on the network may have changed.
    std::string host() const;
    int port() const;

    // Forgets every connection's remembered routes and search times (tests).
    static void clear_route_memory();

    struct State;

private:
    std::shared_ptr<State> state_;
};

// How long recording_activity() waits for the request lock and for its reply.
inline constexpr std::chrono::milliseconds RECORDING_WAIT{750};

} // namespace fv::remote
