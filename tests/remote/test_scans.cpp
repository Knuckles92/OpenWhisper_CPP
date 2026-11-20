// remote/scans.h against services/remote_asr/service.py's scan_tailnet, scan_lan and
// scan_nearby: the ported tests of tests/test_remote_engine.py and tests/test_remote_pairing_ux.py,
// with Tailscale faked, discovery on the in-memory bus, and probes against a loopback host (or a
// stand-in where the address is a tailnet one, which no test may reach).

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <mutex>
#include <string>
#include <vector>

#include "client_test_support.h"
#include "net/tailscale.h"
#include "remote/client.h"
#include "remote/errors.h"
#include "remote/scans.h"
#include "remote/testing/fake_tailscale_cli.h"

using namespace fv;
using namespace fv::remote;
using namespace fv::remote::test;
using namespace std::chrono_literals;

namespace {

constexpr const char* kOwner = "me@example.com";

std::string fingerprint_of(std::string_view pair) {
    std::string fp;
    for (int i = 0; i < 32; ++i) {
        fp += pair;
    }
    return fp;
}

net::TailscalePeer peer(std::string name, std::string address, std::string os, bool online) {
    net::TailscalePeer out;
    out.name = name;
    out.dns_name = name + ".tail0000.ts.net";
    out.address = std::move(address);
    out.os = std::move(os);
    out.online = online;
    out.owner = kOwner;
    out.mine = true;
    return out;
}

// _tailnet_status of tests/test_remote_engine.py: the host at `host_address`, a phone, an
// offline PC and a computer that shares nothing.
net::TailscaleStatus tailnet_status(std::string host_address = "127.0.0.1") {
    net::TailscaleStatus status;
    status.state = "running";
    status.name = "laptop";
    status.dns_name = "laptop.tail0000.ts.net";
    status.address = "100.64.0.1";
    status.owner = kOwner;
    status.tailnet = kOwner;
    status.peers = {peer("devbox", std::move(host_address), "linux", true),
                    peer("phone", "100.64.0.5", "iOS", true),
                    peer("old-pc", "100.64.0.7", "windows", false),
                    peer("nas", "100.64.0.8", "linux", true)};
    return status;
}

// Probes recorded, and answered for real only on loopback; anything else is "nothing
// listening", as the monkeypatched probe of the Python test answers.
struct ProbeRecorder {
    std::mutex mutex;
    std::vector<std::string> probed;
    std::vector<int> ports;

    void install(ClientRig& rig) {
        rig.env.probe = [this, &rig](std::string_view address, int port) {
            {
                std::lock_guard lock(mutex);
                probed.emplace_back(address);
                ports.push_back(port);
            }
            if (address != "127.0.0.1") {
                throw RemoteEngineError("nothing listening");
            }
            return probe_host(rig.env, address, port);
        };
    }
    std::vector<std::string> sorted() {
        std::lock_guard lock(mutex);
        auto out = probed;
        std::ranges::sort(out);
        return out;
    }
};

Json lan_info(std::string name, std::string fingerprint) {
    return Json{{"name", std::move(name)},
                {"port", 47821},
                {"fingerprint", std::move(fingerprint)},
                {"approval", true},
                {"engine", Json{{"label", "Parakeet"}}}};
}

} // namespace

// ---- tests/test_remote_engine.py ----

TEST_CASE("scan_tailnet probes online desktops and keeps those that answer",
          "[test_remote_engine]") {
    // from tests/test_remote_engine.py::
    // test_scan_tailnet_probes_online_desktops_and_keeps_those_that_answer
    ClientRig rig;
    // A host whose owner's Tailscale computers pair without a code (the tailnet_host fixture).
    MiniHost host(rig.reactor, {.tailscale_pairing = true});
    remote::testing::FakeTailscaleCli cli;
    cli.set_status(tailnet_status());
    ProbeRecorder recorder;
    recorder.install(rig);
    const TailnetScan scan = scan_tailnet(rig.env, cli, host.port());
    CHECK(scan.status.running());
    // Not the phone, not the offline PC.
    CHECK(recorder.sorted() == std::vector<std::string>{"100.64.0.8", "127.0.0.1"});
    REQUIRE(scan.hosts.size() == 1);
    const TailnetHost& found = scan.hosts[0];
    CHECK(found.peer.name == "devbox");
    CHECK(found.host_name == "devbox");
    CHECK(found.engine["label"] == "Fake Parakeet");
    CHECK(found.fingerprint == host.fingerprint());
    CHECK(found.approval);
    CHECK(found.port == host.port());
    CHECK(found.compatible);
    CHECK(found.can_pair_without_code());
}

TEST_CASE("scan_tailnet without tailscale probes nothing", "[test_remote_engine]") {
    // from tests/test_remote_engine.py::test_scan_tailnet_without_tailscale_probes_nothing
    ClientRig rig;
    std::atomic<int> probes{0};
    rig.env.probe = [&](std::string_view, int) -> ProbeResult {
        ++probes;
        throw RemoteEngineError("probed");
    };
    remote::testing::FakeTailscaleCli cli;
    const TailnetScan scan = scan_tailnet(rig.env, cli);
    CHECK(scan.status.state == "not_installed");
    CHECK(scan.hosts.empty());
    CHECK(probes == 0);
}

TEST_CASE("scan_tailnet probes the default port when given none") {
    ClientRig rig;
    remote::testing::FakeTailscaleCli cli;
    cli.set_status(tailnet_status("100.64.0.2"));
    ProbeRecorder recorder;
    recorder.install(rig);
    const TailnetScan scan = scan_tailnet(rig.env, cli, 0);
    CHECK(scan.hosts.empty());
    CHECK(recorder.ports == std::vector<int>{47821, 47821});
}

// ---- tests/test_remote_pairing_ux.py ----

TEST_CASE("finds on this network and the tailnet are one entry", "[test_remote_pairing_ux]") {
    // from
    // tests/test_remote_pairing_ux.py::test_finds_on_this_network_and_the_tailnet_are_one_entry
    ClientRig rig;
    const std::string jed = fingerprint_of("AA");
    net::TailscaleStatus status;
    status.state = "running";
    status.owner = kOwner;
    status.peers = {peer("jed", "100.64.0.9", "linux", true)};
    remote::testing::FakeTailscaleCli cli;
    cli.set_status(status);
    std::mutex mutex;
    std::vector<std::string> probed;
    rig.env.probe = [&](std::string_view address, int port) {
        {
            std::lock_guard lock(mutex);
            probed.push_back(std::string(address) + ":" + std::to_string(port));
        }
        ProbeResult result;
        result.host_name = "jed";
        result.protocol = 1;
        result.engine = Json{{"label", "Parakeet"}, {"available", true}};
        result.tailscale_pairing = true;
        result.fingerprint = jed;
        result.approval = true;
        return result;
    };
    BusResponder on_lan(rig.bus, "192.168.1.9", lan_info("jed", jed));
    BusResponder nas(rig.bus, "192.168.1.12", lan_info("nas", fingerprint_of("BB")));

    const NearbyScan scan = scan_nearby(rig.env, cli, false, "");
    CHECK(probed == std::vector<std::string>{"100.64.0.9:47821"});
    REQUIRE(scan.hosts.size() == 2);
    const NearbyHost& both = scan.hosts[0];
    const NearbyHost& other = scan.hosts[1];
    CHECK(both.name() == "jed");
    CHECK(other.name() == "nas");
    CHECK(both.lan.has_value());
    CHECK(both.tailnet.has_value());
    // The owner's own Tailscale computer pairs over the tailnet, without asking.
    CHECK(both.can_pair_without_code());
    CHECK(both.address() == "100.64.0.9:47821");
    CHECK(!other.can_pair_without_code());
    CHECK(other.address() == "192.168.1.12:47821");
    REQUIRE(scan.status.has_value());
    CHECK(scan.status->running());
    CHECK(!scan.swept);
}

TEST_CASE("a search leaves out this computer's own answer", "[test_remote_pairing_ux]") {
    // from tests/test_remote_pairing_ux.py::test_a_search_leaves_out_this_computers_own_answer
    ClientRig rig;
    const std::string own = fingerprint_of("C0");
    BusResponder me(rig.bus, "192.168.1.5", lan_info("me", own));
    remote::testing::FakeTailscaleCli cli;
    CHECK(scan_nearby(rig.env, cli, false, own).hosts.empty());
    // Without its own certificate (not sharing), nothing is left out.
    CHECK(scan_nearby(rig.env, cli, false, "").hosts.size() == 1);
}

TEST_CASE("checking every address probes what answers", "[test_remote_pairing_ux]") {
    // from tests/test_remote_pairing_ux.py::test_checking_every_address_probes_what_answers
    ClientRig rig;
    MiniHost host(rig.reactor);
    std::mutex mutex;
    std::vector<int> knocked;
    rig.env.knock = [&](std::string_view address, std::uint16_t port, std::chrono::milliseconds) {
        std::lock_guard lock(mutex);
        knocked.push_back(port);
        return address == "127.0.0.1";
    };
    rig.env.lan_cards = [] {
        return std::vector<LanCard>{{"127.0.0.5", 24, true, true}};
    };
    remote::testing::FakeTailscaleCli cli;
    CHECK(scan_nearby(rig.env, cli, false, "", host.port()).hosts.empty());
    CHECK(knocked.empty()); // only when asked
    const NearbyScan scan = scan_nearby(rig.env, cli, true, "", host.port());
    CHECK(!knocked.empty());
    CHECK(std::ranges::all_of(knocked, [&](int port) { return port == host.port(); }));
    CHECK(scan.swept);
    REQUIRE(scan.hosts.size() == 1);
    CHECK(scan.hosts[0].name() == "devbox");
    REQUIRE(scan.hosts[0].lan.has_value());
    CHECK(scan.hosts[0].lan->fingerprint == host.fingerprint());
    CHECK(scan.hosts[0].lan->address == "127.0.0.1");
    CHECK(scan.hosts[0].approval);
}

// ---- the rest of the merge ----

TEST_CASE("scan_nearby sorts by name in lower case and keys tailnet hosts without a "
          "certificate by address") {
    ClientRig rig;
    net::TailscaleStatus status;
    status.state = "running";
    status.owner = kOwner;
    status.peers = {peer("Zeta", "100.64.0.20", "windows", true),
                    peer("alpha", "100.64.0.21", "macOS", true),
                    peer("Alpha", "100.64.0.22", "linux", true)};
    remote::testing::FakeTailscaleCli cli;
    cli.set_status(status);
    rig.env.probe = [](std::string_view address, int) {
        ProbeResult result;
        result.host_name = std::string(address);
        result.protocol = 1;
        return result; // no fingerprint: an entry of its own per address
    };
    BusResponder mid(rig.bus, "192.168.1.30", lan_info("Mid", fingerprint_of("DD")));
    const NearbyScan scan = scan_nearby(rig.env, cli, false, "");
    std::vector<std::string> names;
    for (const auto& host : scan.hosts) {
        names.push_back(host.name());
    }
    // Stable: "alpha" and "Alpha" keep the tailnet's order.
    CHECK(names == std::vector<std::string>{"alpha", "Alpha", "Mid", "Zeta"});
}

TEST_CASE("a LAN find of a tailnet host keeps the tailnet's name and takes either's approval") {
    ClientRig rig;
    const std::string fp = fingerprint_of("AA");
    net::TailscaleStatus status;
    status.state = "running";
    status.owner = kOwner;
    status.peers = {peer("jed-ts", "100.64.0.9", "linux", true)};
    remote::testing::FakeTailscaleCli cli;
    cli.set_status(status);
    rig.env.probe = [&](std::string_view, int) {
        ProbeResult result;
        result.host_name = "jed";
        result.protocol = 1;
        result.fingerprint = fp;
        result.approval = false;
        return result;
    };
    BusResponder on_lan(rig.bus, "192.168.1.9", lan_info("jed-lan", fp));
    const NearbyScan scan = scan_nearby(rig.env, cli, false, "");
    REQUIRE(scan.hosts.size() == 1);
    CHECK(scan.hosts[0].host_name == "jed");
    CHECK(scan.hosts[0].name() == "jed-ts");
    CHECK(scan.hosts[0].approval);
    CHECK(!scan.hosts[0].can_pair_without_code());
    CHECK(scan.hosts[0].address() == "192.168.1.9:47821");
}
