#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "base/clock.h"
#include "base/json.h"
#include "base/signal.h"
#include "net/reactor.h"
#include "remote/control.h"
#include "remote/discovery_transport.h"
#include "remote/tailscale_cli.h"
#include "remote_host/activity.h"
#include "remote_host/history_channel.h"
#include "remote_host/host_engine.h"
#include "remote_host/identity.h"
#include "remote_host/registry.h"

// Serve this computer's speech engine to paired computers (services/remote_asr/host.py
// SpeechHost): a TLS WebSocket server on the remote engine's path, which answers probes, pairs
// computers (by code, by Tailscale, by the owner's approval), greets a paired computer's hello
// with `ready`, and then answers its requests one binary frame at a time. The wire is
// OpenWhisper's (remote/protocol.h), so either app's client dictates through either app's host.
namespace fv::remote_host {

// How long the host waits and how much it takes at once (host.py's module constants).
struct HostLimits {
    // Paired computers' open connections (dictation and history hold one each, a meeting adds
    // one), and connections that haven't yet said who they are, counted apart so they can't
    // crowd paired computers out.
    int max_connections = 32;
    int max_handshakes = 12;
    // Engine and model operations share a small FIFO work pool; waiting work is bounded too.
    int max_active_jobs = 2;
    int max_queued_jobs = 4;
    std::chrono::milliseconds job_wait{30'000};
    // The opening handshake, the first message, and a pairing request's nonce.
    std::chrono::milliseconds handshake{10'000};
    // How long a pairing request waits for this computer's owner, how far back an address's
    // requests count, and how many it may make in that window.
    std::chrono::milliseconds approval{120'000};
    std::chrono::milliseconds approval_window{600'000};
    // How long turning sharing off waits for a waiting request to hear why.
    std::chrono::milliseconds stop_grace{2'000};
    std::chrono::milliseconds update_hold{60'000};
    // How long a recording reservation outlives its connection, for the client to reconnect.
    std::chrono::milliseconds recording_grace{15'000};
    // Keepalive: a ping this often, closed when its pong takes as long.
    std::chrono::milliseconds ping{20'000};
    int max_approval_requests = 4;
    // A pairing code stays valid this long, for one successful pairing.
    double pairing_ttl_s = 300;
};

// What the host serves and asks. Every function is optional (OpenWhisper's None) unless marked,
// is called on a pool thread of the host (never the reactor or the GUI thread), may block, and
// must be thread-safe. Exceptions become the request's error, as Python's do.
struct SpeechHostDeps {
    net::Reactor& reactor;
    DeviceRegistry& registry; // outlives the host
    HostIdentity identity;
    std::string host_name; // as clients show it (socket.gethostname())
    Clock& clock;          // pairing, approval, update hold and reservation deadlines; host_ms
    // Code-free pairing asks whois through it (remote::own_device); null refuses it.
    remote::TailscaleCli* tailscale = nullptr;
    // The direct-history broker; null closes history connections 1001 "host stopped sharing".
    HistoryChannel* history = nullptr;
    // UDP sockets for the discovery responder; null answers no discovery query.
    remote::DiscoveryTransportFactory discovery;

    // Required: the engine to serve, asked for every request; null or a throw serves an
    // unavailable one.
    std::function<std::shared_ptr<HostEngine>()> engine_provider;
    // The Tailscale login whose own computers pair without a code; "" when that is off.
    std::function<std::string()> tailscale_owner;
    // This computer's other addresses (LAN and Tailscale), which clients fall back on.
    std::function<std::vector<std::string>()> addresses;
    // What a paired computer may switch this one to: [{family, model, label}].
    std::function<Json()> models;
    // (family, model, device_name, device) -> the new engine's describe() once loaded; throws
    // with the reason it can't. Blocks for as long as the load takes.
    std::function<Json(std::string, std::string, std::string, std::optional<std::string>)>
        select_model;
    // Permissions, read for every request that needs them (true only when exactly true).
    std::function<bool()> model_management, engine_settled, records_enabled, mcp_enabled;
    // model_catalog, download_model, install_runtime: (op, header, device_name) -> result.
    std::function<Json(std::string_view op, const Json& header, std::string_view device_name)>
        manage_models;
    // ready.runtime and configure_runtime's answer.
    std::function<Json()> runtime;
    // (family, model, settings, device_name) -> the new engine's describe().
    std::function<Json(std::string, std::string, Json, std::string)> configure_runtime;
    // records_* for the authenticated device; throws RecordsBusy, RecordNotFound or
    // py::ValueError (dispatch.h) for the replies Python gives RecordsBusy, LookupError and
    // ValueError.
    std::function<Json(std::string_view op, const Json& header, std::span<const std::byte>,
                       const remote::PairedDevice&)>
        records;
    // {"dictation": {"count", "bytes"}, "meeting": {...}} this device stored here.
    std::function<Json(std::string_view device_id)> records_summary;
    // mcp_state, mcp_configure; py::ValueError("code: text") is the code and text the client
    // reads.
    std::function<Json(std::string_view op, const Json& header, std::string_view device_name)> mcp;

    // Optional per-device, per-connection consent for the two existing MCP operations.
    std::function<bool(std::string_view device_id)> mcp_peer_allowed;
    std::function<Json(std::string_view op, const Json& header, std::string_view device_id,
                       std::string_view connection_id)>
        mcp_peer_request;
    std::function<void(std::string_view connection_id)> mcp_peer_disconnect;

    // Seams for tests, null in the app. own_device replaces remote::own_device(remote, local,
    // owner, *tailscale); peer_address replaces the address a connection's socket reports
    // (numeric, or "unknown" when there is none) wherever the host uses it.
    std::function<std::optional<remote::PeerIdentity>(
        std::string_view remote, std::string_view local, std::string_view owner)>
        own_device;
    std::function<std::string(std::string_view address)> peer_address;
    // Told how long each of the reactor's callbacks took (they only parse and post).
    std::function<void(std::chrono::nanoseconds took)> reactor_timing;

    HostLimits limits;
};

// Affinity: thread-safe. The reactor only parses and posts: each connection's frames are
// handled in order on a strand of the host's own pool ("fv-remote-host", 8 threads), and engine
// and switch work runs there too, at most max_active_jobs at a time. Events are emitted from
// whichever thread changed the state.
class SpeechHost {
public:
    struct Impl;

private:
    // Owned here alone: the reactor's callbacks reach it through a gate the destructor closes
    // (speech_host.cpp HandlerGate). Declared first, as `events` lives in it.
    std::shared_ptr<Impl> impl_;

public:
    explicit SpeechHost(SpeechHostDeps deps);
    // stop(), then waits for the server's callbacks still running and for work still running
    // on the pool (a model switch included). Never from a reactor callback.
    ~SpeechHost();
    SpeechHost(const SpeechHost&) = delete;
    SpeechHost& operator=(const SpeechHost&) = delete;

    // Listens on `bind`:`port` (0 picks a free port) and, with `discovery_port`, answers
    // discovery queries there (a taken UDP port leaves discovery_error() set and sharing
    // running). The port, or the system's reason it could not listen (a taken port). Already
    // running: the port it runs on.
    std::expected<std::uint16_t, std::string> start(int port, std::string bind,
                                                    std::optional<int> discovery_port);
    // Stops answering discovery, closes pairing, gives a waiting pairing request up to
    // stop_grace to say why, closes every connection (1001) and the server. Blocks up to
    // stop_grace plus the server's own grace; never waits for a handler that is still running.
    // Not on the reactor's threads.
    void stop();
    bool running() const;
    std::optional<std::uint16_t> port() const;

    // A fresh six-digit code, valid pairing_ttl_s for one pairing. The overload opens pairing
    // with a code of the caller's (tests and recorded exchanges).
    std::string open_pairing();
    std::string open_pairing(std::string code);
    void close_pairing();
    // (code, seconds left) while pairing is open.
    std::optional<std::pair<std::string, double>> pairing_status();
    std::optional<remote::PairRequestInfo> pending_request();
    // Allows or denies the waiting request; false when it's gone already.
    bool answer_request(std::string_view id, bool allow);
    std::vector<remote::ConnectedClient> connected_clients();

    // The update hold: an idle host reserved against new work until an updater restarts it.
    bool update_active();
    // {recordings, requests, untracked_clients, reconnecting_recording, maintenance}.
    Json update_status();
    // {"token", "expires_in"} or {"reason"}.
    Json prepare_update();
    bool cancel_update(std::string_view token);

    // Closes the connection of every client that was told about another engine in `ready` and
    // is not inside a request, 4409 "engine changed"; how many. Never on the GUI thread or the
    // reactor.
    int engine_changed();
    // Forgets a device and drops its open connections (4401 "device removed").
    bool remove_device(std::string_view id);
    // Calls a device `name` here, its open connections included.
    std::optional<remote::PairedDevice> rename_device(std::string_view id, std::string_view name);

    HostActivity& activity();
    std::string discovery_error() const;
    // The UDP port the responder answers on, while it does.
    std::optional<std::uint16_t> discovery_port() const;

    // Free slots and queued work, as the admission tests watch them.
    int free_connection_slots() const;
    int free_handshake_slots() const;
    std::size_t queued_jobs() const;

    // (kind, detail) as on_event: state, pairing{open}, pair_request{name,address} and {},
    // paired{name}, devices, clients, activity{name,busy}, engine{family,model,by} or {by},
    // mcp{by}, records{device,op}.
    Signal<std::string, Json>& events;
};

} // namespace fv::remote_host
