#include "remote/scans.h"

#include <algorithm>
#include <exception>
#include <future>
#include <optional>
#include <set>
#include <string>
#include <utility>

#include "base/executor.h"
#include "base/log.h"
#include "pyport/text.h"
#include "remote/detail/client_socket.h"
#include "remote/detail/parallel.h"
#include "remote/discovery_search.h"

namespace fv::remote {

namespace {

constexpr log::Logger kLog("services.remote_asr.service");

// A probe that fails is no host: RemoteEngineError quietly, anything else logged.
std::optional<ProbeResult> try_probe(ClientEnv& env, const std::string& address, int port) {
    try {
        if (env.probe) {
            return env.probe(address, port);
        }
        return probe_host(env, address, port);
    } catch (const RemoteEngineError&) {
        return std::nullopt;
    } catch (const std::exception& error) {
        kLog.debug("Probe of {} failed: {}", address, detail::exception_class(error));
        return std::nullopt;
    }
}

// dict semantics for scan_nearby's merge: insertion order, a key set again keeps its place.
class Merged {
public:
    NearbyHost* find(const std::string& key) {
        for (auto& [name, host] : items_) {
            if (name == key) {
                return &host;
            }
        }
        return nullptr;
    }
    void set(const std::string& key, NearbyHost host) {
        if (NearbyHost* known = find(key)) {
            *known = std::move(host);
            return;
        }
        items_.emplace_back(key, std::move(host));
    }
    std::vector<NearbyHost> values() && {
        std::vector<NearbyHost> out;
        for (auto& item : items_) {
            out.push_back(std::move(item.second));
        }
        return out;
    }

private:
    std::vector<std::pair<std::string, NearbyHost>> items_;
};

} // namespace

TailnetScan scan_tailnet(ClientEnv& env, TailscaleCli& cli, int port) {
    assert_not_gui_thread();
    if (port == 0) {
        port = DEFAULT_PORT;
    }
    TailnetScan scan;
    scan.status = cli.status();
    if (!scan.status.running()) {
        return scan;
    }
    std::vector<net::TailscalePeer> peers;
    for (const auto& peer : scan.status.peers) {
        if (peer.online && peer.is_desktop()) {
            peers.push_back(peer);
        }
    }
    if (peers.empty()) {
        return scan;
    }
    const auto probe = [&](const net::TailscalePeer& peer) -> std::optional<TailnetHost> {
        const auto result = try_probe(env, peer.address, port);
        if (!result) {
            return std::nullopt;
        }
        TailnetHost host;
        host.peer = peer;
        host.port = port;
        host.host_name = result->host_name;
        host.engine = result->engine;
        host.tailscale_pairing = result->tailscale_pairing;
        host.compatible = result->compatible();
        host.fingerprint = result->fingerprint;
        host.approval = result->approval;
        return host;
    };
    for (auto& host : detail::parallel_map<std::optional<TailnetHost>>(
             static_cast<std::size_t>(PROBE_WORKERS), peers, probe)) {
        if (host) {
            scan.hosts.push_back(std::move(*host));
        }
    }
    return scan;
}

std::vector<LanHost> scan_lan(ClientEnv& env, bool sweep_too, int port) {
    assert_not_gui_thread();
    std::vector<LanHost> found = search(env);
    if (!sweep_too) {
        return found;
    }
    std::set<std::string> seen;
    for (const auto& host : found) {
        seen.insert(host.address);
    }
    std::vector<std::string> answered;
    for (auto& address : sweep(env, port)) {
        if (!seen.contains(address)) {
            answered.push_back(std::move(address));
        }
    }
    if (answered.empty()) {
        return found;
    }
    const auto probe = [&](const std::string& address) -> std::optional<LanHost> {
        const auto result = try_probe(env, address, port);
        if (!result) {
            return std::nullopt;
        }
        LanHost host;
        host.address = address;
        host.port = port;
        host.host_name = result->host_name;
        host.fingerprint = result->fingerprint;
        host.engine = result->engine;
        host.protocol = result->protocol;
        host.approval = result->approval;
        return host;
    };
    for (auto& host : detail::parallel_map<std::optional<LanHost>>(
             static_cast<std::size_t>(PROBE_WORKERS), answered, probe)) {
        if (host && !host->fingerprint.empty() &&
            std::ranges::find(found, host->fingerprint, &LanHost::fingerprint) == found.end()) {
            found.push_back(std::move(*host));
        }
    }
    return found;
}

NearbyScan scan_nearby(ClientEnv& env, TailscaleCli& cli, bool sweep_too,
                       std::string_view own_fingerprint, int port) {
    assert_not_gui_thread();
    auto tailnet_future =
        std::async(std::launch::async, [&] { return scan_tailnet(env, cli, port); });
    std::vector<LanHost> lan_hosts;
    try {
        lan_hosts = scan_lan(env, sweep_too, port);
    } catch (const std::exception& error) {
        kLog.warning("Searching this network for hosts failed: {}", detail::exception_class(error));
    }
    TailnetScan tailnet = tailnet_future.get();

    const std::string own(own_fingerprint);
    std::erase_if(lan_hosts,
                  [&](const LanHost& host) { return !own.empty() && host.fingerprint == own; });
    Merged merged;
    for (const auto& host : tailnet.hosts) {
        NearbyHost entry;
        entry.host_name = host.host_name;
        entry.fingerprint = host.fingerprint;
        entry.engine = host.engine;
        entry.compatible = host.compatible;
        entry.approval = host.approval;
        entry.tailnet = host;
        merged.set(host.fingerprint.empty() ? "tailnet:" + host.peer.address : host.fingerprint,
                   std::move(entry));
    }
    for (const auto& host : lan_hosts) {
        if (const NearbyHost* known = merged.find(host.fingerprint)) {
            NearbyHost entry;
            entry.host_name = known->host_name;
            entry.fingerprint = known->fingerprint;
            entry.engine = known->engine;
            entry.compatible = known->compatible;
            entry.approval = known->approval || host.approval;
            entry.lan = host;
            entry.tailnet = known->tailnet;
            merged.set(host.fingerprint, std::move(entry));
        } else {
            NearbyHost entry;
            entry.host_name = host.host_name;
            entry.fingerprint = host.fingerprint;
            entry.engine = host.engine;
            entry.compatible = host.compatible();
            entry.approval = host.approval;
            entry.lan = host;
            merged.set(host.fingerprint, std::move(entry));
        }
    }
    NearbyScan scan;
    scan.hosts = std::move(merged).values();
    std::ranges::stable_sort(scan.hosts, {},
                             [](const NearbyHost& host) { return py::lower(host.name()); });
    scan.status = std::move(tailnet.status);
    scan.swept = sweep_too;
    return scan;
}

} // namespace fv::remote
