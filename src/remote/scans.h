#pragma once

#include <string_view>
#include <vector>

#include "net/tailscale.h"
#include "remote/client.h"
#include "remote/control.h"
#include "remote/discovery_codec.h"
#include "remote/tailscale_cli.h"

// Looking for computers that share their engine (services/remote_asr/service.py's scan_tailnet,
// scan_lan and scan_nearby), as free functions: the service passes its Tailscale CLI and this
// computer's own certificate. Every function blocks: never on the GUI thread or the reactor.
namespace fv::remote {

struct TailnetScan {
    net::TailscaleStatus status;
    std::vector<TailnetHost> hosts;
};

// Computers on the tailnet sharing their engine: the online desktop peers (windows, linux,
// macos) that answer a probe on `port` (0: DEFAULT_PORT), PROBE_WORKERS at a time; a probe that
// fails leaves its peer out. Only one port is probed; a host on another one is paired by typing
// its address. Without a running Tailscale nothing is probed.
TailnetScan scan_tailnet(ClientEnv&, TailscaleCli&, int port = DEFAULT_PORT);

// Computers on this network sharing their engine, one per certificate: those answering a
// search, then with `sweep` every address of this network that accepts a connection on `port`
// (for routers that drop broadcasts) and answers a probe there.
std::vector<LanHost> scan_lan(ClientEnv&, bool sweep, int port = DEFAULT_PORT);

// This network and the tailnet searched together, one entry per computer (matched by
// certificate; a tailnet host without one is its own entry), sorted by name() in lower case. A
// broadcast reaches this computer too, so its own answer (`own_fingerprint`, when it shares as
// well) is left out; Tailscale never lists this computer. A failed search of this network is
// logged and finds nothing.
NearbyScan scan_nearby(ClientEnv&, TailscaleCli&, bool sweep, std::string_view own_fingerprint,
                       int port = DEFAULT_PORT);

} // namespace fv::remote
