#include "remote/discovery_search.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <deque>
#include <set>
#include <utility>

#include "base/executor.h"
#include "base/ids.h"
#include "base/log.h"
#include "net/tcp_knock.h"
#include "pyport/text.h"
#include "remote/detail/client_socket.h"
#include "remote/detail/parallel.h"
#include "remote/wire.h"

namespace fv::remote {

namespace {

constexpr log::Logger kLog("services.remote_asr.discovery");

using Clock = std::chrono::steady_clock;

std::vector<LanCard> lan_cards(ClientEnv& env) {
    return env.lan_cards ? env.lan_cards() : system_lan_cards();
}

// _destinations: (address, port) pairs to query, `targets` or this network's broadcast
// addresses.
std::vector<std::pair<std::string, int>>
destinations(ClientEnv& env, std::optional<std::vector<std::pair<std::string, int>>> targets) {
    std::vector<std::pair<std::string, int>> out;
    if (!targets) {
        targets.emplace();
        try {
            for (auto& address : broadcast_targets(lan_cards(env))) {
                targets->emplace_back(std::move(address), DISCOVERY_PORT);
            }
        } catch (const std::exception& error) {
            kLog.debug("Could not list broadcast addresses: {}", detail::exception_class(error));
            targets->clear();
        }
    }
    for (auto& target : *targets) {
        if (target.second != 0) {
            out.push_back(std::move(target));
        }
    }
    return out;
}

// _send_due: sends the query to every destination for each round of `sends` now due.
void send_due(DiscoveryTransport& socket, std::span<const std::byte> query,
              const std::vector<std::pair<std::string, int>>& to, std::deque<double>& sends,
              double elapsed) {
    while (!sends.empty() && sends.front() <= elapsed) {
        sends.pop_front();
        for (const auto& [address, port] : to) {
            // A port outside 0..65535 is no socket's (Python's sendto refuses it).
            if (port < 0 || port > 65535 ||
                !socket.send_to(query, address, static_cast<std::uint16_t>(port))) {
                kLog.debug("Discovery query to ({}, {}) failed", address, port);
            }
        }
    }
}

std::chrono::milliseconds ms(double seconds) {
    return std::chrono::milliseconds(static_cast<std::int64_t>(seconds * 1000.0 + 0.5));
}

} // namespace

std::vector<LanHost> search(ClientEnv& env, std::chrono::milliseconds timeout,
                            std::optional<std::vector<std::pair<std::string, int>>> targets,
                            std::function<bool(const LanHost&)> stop_when) {
    assert_not_gui_thread();
    const auto to = destinations(env, std::move(targets));
    if (to.empty() || !env.discovery) {
        return {};
    }
    std::array<std::byte, DISCOVERY_NONCE_BYTES> nonce_bytes{};
    random_bytes(nonce_bytes);
    std::vector<std::uint8_t> nonce;
    for (const std::byte value : nonce_bytes) {
        nonce.push_back(std::to_integer<std::uint8_t>(value));
    }
    const std::vector<std::byte> query = make_query(nonce);
    std::vector<LanHost> found;
    std::set<std::string> fingerprints;
    std::deque<double> sends(SEND_AT_S.begin(), SEND_AT_S.end());
    const double limit = std::chrono::duration<double>(timeout).count();
    // No bind(): the first send picks a port, as any client's would, so searching never
    // registers this app as something that listens.
    const auto socket = env.discovery(true);
    struct Closer {
        DiscoveryTransport& socket;
        ~Closer() { socket.close(); }
    } closer{*socket};
    const auto started = Clock::now();
    while (true) {
        const double elapsed = std::chrono::duration<double>(Clock::now() - started).count();
        send_due(*socket, query, to, sends, elapsed);
        const double remaining = limit - elapsed;
        if (remaining <= 0) {
            break;
        }
        const double until_next_send = sends.empty() ? remaining : sends.front() - elapsed;
        const double wait = std::max(0.01, std::min(remaining, until_next_send));
        // _receive_reply: a timeout, an error or a stray packet is no host.
        const auto datagram = socket->receive(ms(wait));
        if (!datagram) {
            continue;
        }
        auto host = parse_reply(datagram->bytes, nonce, datagram->sender);
        if (!host || fingerprints.contains(host->fingerprint)) {
            continue;
        }
        fingerprints.insert(host->fingerprint);
        found.push_back(*host);
        if (stop_when && stop_when(*host)) {
            break;
        }
    }
    return found;
}

std::optional<LanHost> find_host(ClientEnv& env, std::string_view fingerprint,
                                 std::chrono::milliseconds timeout) {
    const std::string wanted = py::upper(fingerprint);
    if (wanted.empty()) {
        return std::nullopt;
    }
    const auto hosts = search(env, timeout, std::nullopt, [&wanted](const LanHost& host) {
        return host.fingerprint == wanted;
    });
    const auto match = std::ranges::find(hosts, wanted, &LanHost::fingerprint);
    if (match == hosts.end()) {
        return std::nullopt;
    }
    return *match;
}

std::vector<std::string> sweep(ClientEnv& env, int port,
                               std::optional<std::vector<std::string>> addresses,
                               std::chrono::milliseconds connect_timeout) {
    assert_not_gui_thread();
    const std::vector<std::string> candidates =
        addresses ? std::move(*addresses) : sweep_addresses(lan_cards(env));
    if (candidates.empty() || port < 0 || port > 65535) {
        return {};
    }
    const auto knock = [&](const std::string& address) -> std::optional<std::string> {
        const auto to = static_cast<std::uint16_t>(port);
        const bool answered = env.knock ? env.knock(address, to, connect_timeout)
                                        : net::tcp_knock(address, to, connect_timeout);
        return answered ? std::optional<std::string>(address) : std::nullopt;
    };
    std::vector<std::string> out;
    for (auto& address : detail::parallel_map<std::optional<std::string>>(
             static_cast<std::size_t>(SWEEP_WORKERS), candidates, knock)) {
        if (address) {
            out.push_back(std::move(*address));
        }
    }
    return out;
}

} // namespace fv::remote
