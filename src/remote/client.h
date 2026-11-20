#pragma once

#include <chrono>
#include <cstdint>
#include <functional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "base/cancel.h"
#include "net/reactor.h"
#include "remote/discovery_codec.h"
#include "remote/discovery_transport.h"
#include "remote/errors.h"
#include "remote/protocol.h"

// The client side of the remote engine (services/remote_asr/client.py): asking a host whether it
// shares an engine, and pairing with it by code, through Tailscale, or by asking its owner. The
// paired connection itself is remote/connection.h.
//
// The host's certificate is never checked against a CA or a name: it is self-signed and reached
// by IP, name or tailnet address alike. What a pairing returns is the fingerprint this computer's
// socket saw, which is pinned from then on, so whoever pairs compares it with the one the host
// shows. Every function here blocks on the network: never on the GUI thread or the reactor.
namespace fv::remote {

// Seconds, as client.py's constants.
inline constexpr std::chrono::milliseconds CONNECT_TIMEOUT{6'000};
// Per address when there are others to try after it, so a laptop away from home doesn't wait
// the full timeout on its LAN address before Tailscale.
inline constexpr std::chrono::milliseconds FALLBACK_CONNECT_TIMEOUT{2'500};
// Waiting for `ready` or `paired` after the socket opens.
inline constexpr std::chrono::milliseconds HANDSHAKE_TIMEOUT{10'000};
inline constexpr std::chrono::milliseconds PROBE_TIMEOUT{2'000};
// A paired connection pings the host this often, and closes when a pong takes longer than the
// timeout (as websockets' own keepalive would).
inline constexpr std::chrono::milliseconds KEEPALIVE_INTERVAL{20'000};
inline constexpr std::chrono::milliseconds KEEPALIVE_TIMEOUT{20'000};
// How often a request waiting on the host checks for a cancel, which keeps the connection and so
// can't interrupt the read the way closing it did.
inline constexpr std::chrono::milliseconds REQUEST_POLL{50};
// A host missing from its saved addresses is looked for on the network at most this often.
inline constexpr std::chrono::milliseconds REDISCOVER_INTERVAL{30'000};
// Longest a pairing request waits for the host's owner to answer.
inline constexpr std::chrono::milliseconds APPROVAL_WAIT{150'000};
// How often that wait checks for a cancel.
inline constexpr std::chrono::milliseconds APPROVAL_POLL{200};
// After close(), how long the host's close frame is waited for (websockets' close_timeout).
inline constexpr std::chrono::milliseconds CLOSING_TIMEOUT{2'000};
// Probes running at once while a scan checks what answered.
inline constexpr int PROBE_WORKERS = 12;

// What the client side runs on and reaches. Members left empty fall back as each says.
// Affinity: the functions are called from whichever thread calls the client; they must be
// thread-safe.
struct ClientEnv {
    // Every WebSocket of the client runs on it.
    net::Reactor& reactor;
    // UDP sockets for searches (remote/discovery_transport.h): udp_discovery() in the app, a
    // FakeDiscoveryBus in tests. Empty: searches find nothing and send nothing.
    DiscoveryTransportFactory discovery;
    // This computer's IPv4 addresses, where a search broadcasts and a sweep knocks. Empty:
    // system_lan_cards().
    std::function<std::vector<LanCard>()> lan_cards;
    // A paired host just listed where it is now (`ready.host.addresses`, the strings among
    // them): pairing_settings' remember_host_addresses in the app. Empty: nothing is saved.
    std::function<void(std::string_view fingerprint, std::string_view reached, int port,
                       std::span<const std::string> listed)>
        remember_addresses;

    // The probe a scan runs on what answered. Empty: probe_host with its default timeout.
    std::function<ProbeResult(std::string_view host, int port)> probe;
    // A sweep's TCP knock. Empty: net::tcp_knock.
    std::function<bool(std::string_view address, std::uint16_t port,
                       std::chrono::milliseconds timeout)>
        knock;
    // A paired connection's keepalive, as client.py's KEEPALIVE_INTERVAL_S and
    // KEEPALIVE_TIMEOUT_S, which tests shorten.
    std::chrono::milliseconds keepalive_interval = KEEPALIVE_INTERVAL;
    std::chrono::milliseconds keepalive_timeout = KEEPALIVE_TIMEOUT;
};

// This computer's addresses as discovery reads them: net::local_ipv4_addresses() with each
// one's kind, prefix and privacy. Blocking (it lists the network cards).
std::vector<LanCard> system_lan_cards();

// "wss://<format_address(host, port)>/<PATH>".
std::string engine_uri(std::string_view host, int port);

// Asks whether `host` shares an engine, without pairing or a token. RemoteEngineError (and its
// kinds) for a host that can't be reached, isn't a remote engine or "<where> didn't answer the
// probe.".
ProbeResult probe_host(ClientEnv&, std::string_view host, int port = DEFAULT_PORT,
                       std::chrono::milliseconds timeout = PROBE_TIMEOUT);

// Trades the code shown on the host for this computer's own token. With `tailscale` no code is
// sent: the host checks through Tailscale that this computer belongs to its owner. Otherwise the
// code's digits (str.isdigit) are sent, and none is "Enter the pairing code shown on the host."
// before anything connects.
PairingResult pair_with_host(ClientEnv&, std::string_view host, int port, std::string_view code,
                             std::string_view device_name, bool tailscale = false,
                             std::chrono::milliseconds timeout = CONNECT_TIMEOUT);

// Asks the host's owner to allow this computer, with no code to type. `on_code` gets the six
// digits to show here once both sides have them (pairing_sas); the host shows the same six while
// it asks. Blocks until the host answers (at most APPROVAL_WAIT), or `canceled` is, which tells
// the host and raises PairingCanceled("Pairing canceled.").
PairingResult request_pairing(ClientEnv&, std::string_view host, int port,
                              std::string_view device_name,
                              std::function<void(std::string sas)> on_code,
                              CancelToken canceled = {},
                              std::chrono::milliseconds timeout = CONNECT_TIMEOUT);

} // namespace fv::remote
