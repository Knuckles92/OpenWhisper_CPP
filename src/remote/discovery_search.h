#pragma once

#include <chrono>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "remote/client.h"
#include "remote/discovery_codec.h"

// The searching side of discovery (services/remote_asr/discovery.py's search, find_host and
// sweep): the query goes out from an unbound socket to this network's broadcast addresses (or
// the targets given), and the answers are collected for about a second. Nothing in an answer is
// trusted: pairing pins the certificate, so a forged answer can at worst list a computer that
// then fails to connect. Every function blocks: never on the GUI thread or the reactor.
namespace fv::remote {

// Computers on this network sharing an engine, one entry per certificate (uppercase), in the
// order they answered. Blocks for `timeout` (less once `stop_when` accepts an answer). `targets`
// are (address, port) pairs to query instead of this network's broadcast addresses (a port of 0
// is left out). The query goes out at SEND_AT_S; a socket that refuses a send is logged and the
// search goes on. Nothing is sent without a discovery factory in `env`.
std::vector<LanHost> search(ClientEnv&,
                            std::chrono::milliseconds timeout = std::chrono::milliseconds(1'200),
                            std::optional<std::vector<std::pair<std::string, int>>> targets = {},
                            std::function<bool(const LanHost&)> stop_when = {});

// Where the host with this certificate (any case) answers on this network now, if it does; the
// search stops at its answer.
std::optional<LanHost>
find_host(ClientEnv&, std::string_view fingerprint,
          std::chrono::milliseconds timeout = std::chrono::milliseconds(1'200));

// Addresses here that accept a TCP connection on `port`, in the order given: `addresses`, or
// every other address of this computer's private networks (sweep_addresses). Only a connect,
// closed at once, SWEEP_WORKERS at a time: whatever answers is then probed the usual way, which
// says whether it shares an engine. Blocks about two seconds.
std::vector<std::string>
sweep(ClientEnv&, int port = DEFAULT_PORT, std::optional<std::vector<std::string>> addresses = {},
      std::chrono::milliseconds connect_timeout = std::chrono::milliseconds(400));

} // namespace fv::remote
