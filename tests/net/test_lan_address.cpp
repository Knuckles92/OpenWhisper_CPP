// net/lan_address.h: tests/test_lan_address.py and two of tests/test_meeting_web_server_address.py
// over injected interface lists and route probes, and services/lan_address.py replayed
// (tests/golden/services.lan_address.json).
//
// Fixtures are the interfaces of two real machines (2026-09-26): an Arch laptop with Wi-Fi,
// Tailscale, Docker and Proton VPN, where Proton held the default route; and a Windows desktop
// with Ethernet, Tailscale and a WSL adapter.

#include <catch2/catch_test_macros.hpp>

#include <map>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

#include "golden/golden.h"
#include "net/lan_address.h"

using namespace fv;
using namespace fv::net;

namespace {

constexpr std::string_view kModule = "services.lan_address";

LanInterface iface(std::string name, std::string address, int prefix, bool point_to_point = false,
                   std::string description = {}, std::optional<int> if_type = std::nullopt) {
    return {std::move(name), std::move(address),     prefix,
            point_to_point,  std::move(description), if_type};
}

const std::vector<LanInterface> kArchLaptop = {
    iface("wlo1", "192.168.0.118", 24),         iface("tailscale0", "100.82.22.3", 32, true),
    iface("br-7ba443d1c480", "172.18.0.1", 16), iface("docker0", "172.17.0.1", 16),
    iface("proton0", "10.2.0.2", 32, true),
};

const std::vector<LanInterface> kWindowsDesktop = {
    iface("Tailscale", "100.65.148.53", 32, false, "Tailscale Tunnel", 53),
    iface("Ethernet", "192.168.0.206", 24, false, "Realtek(R) PCI(e) Ethernet Controller", 6),
    iface("vEthernet (WSL (Hyper-V firewall))", "172.21.32.1", 20, false,
          "Hyper-V Virtual Ethernet Adapter", 6),
    iface("ProtonVPN", "10.2.0.2", 32, false, "ProtonVPN Tunnel", 53),
};

// The test file's `machine` fixture: the listing, the route and the host name answer from the
// test; sysfs belongs to the machine running the tests, so it says nothing but "wlo1 is a
// device".
LanProbes machine(std::optional<std::vector<LanInterface>> interfaces,
                  std::optional<std::string> route, std::vector<std::string> hostname = {}) {
    LanProbes probes;
    probes.interfaces = [interfaces] {
        return interfaces;
    };
    probes.route_address = [route] {
        return route;
    };
    probes.hostname_addresses = [hostname] {
        return hostname;
    };
    probes.linux_sysfs =
        LinuxSysfs{[](const LanInterface&) { return std::optional<std::string>(); },
                   [](const std::string& name) {
                       return name == "wlo1";
                   }};
    return probes;
}

LanInterface interface_from(const Json& data) {
    LanInterface out;
    out.name = data.value("name", "");
    out.address = data.at("address").get<std::string>();
    out.prefix = data.value("prefix", 0);
    out.point_to_point = data.value("point_to_point", false);
    out.description = data.value("description", "");
    if (data.contains("if_type") && !data.at("if_type").is_null()) {
        out.if_type = data.at("if_type").get<int>();
    }
    return out;
}

// The case file's probes: whatever it lists, and Linux sysfs that says nothing.
LanProbes probes_from(const golden::Case& c) {
    std::optional<std::vector<LanInterface>> listed;
    if (!c.args()[0].is_null()) {
        listed.emplace();
        for (const auto& data : c.args()[0]) {
            listed->push_back(interface_from(data));
        }
    }
    LanProbes probes =
        machine(listed, c.arg<std::optional<std::string>>(1), c.arg<std::vector<std::string>>(2));
    probes.linux_sysfs.reset();
    return probes;
}

// dataclasses.asdict(entry) plus `private`, as the case file records it.
Json as_record(const LocalAddress& entry) {
    return {{"address", entry.address},   {"interface", entry.interface_name},
            {"kind", entry.kind},         {"prefix", entry.prefix},
            {"physical", entry.physical}, {"private", entry.is_private}};
}

} // namespace

TEST_CASE("test_the_lan_wins_over_the_vpn_holding_the_default_route") {
    const auto best = best_lan_address(machine(kArchLaptop, "10.2.0.2"));

    REQUIRE(best);
    CHECK(best->address == "192.168.0.118");
    CHECK(best->interface_name == "wlo1");
    CHECK(best->kind == kLan);
    CHECK(best->is_lan);
}

TEST_CASE("test_every_arch_laptop_interface_is_classified") {
    std::map<std::string, std::string> kinds;
    for (const auto& entry : local_ipv4_addresses(machine(kArchLaptop, "10.2.0.2"))) {
        kinds[entry.interface_name] = entry.kind;
    }

    CHECK(kinds == std::map<std::string, std::string>{{"wlo1", "lan"},
                                                      {"proton0", "vpn"},
                                                      {"tailscale0", "tailscale"},
                                                      {"br-7ba443d1c480", "virtual"},
                                                      {"docker0", "virtual"}});
}

TEST_CASE("test_windows_adapters_are_classified") {
    const auto probes = machine(kWindowsDesktop, "10.2.0.2");
    std::map<std::string, std::string> kinds;
    for (const auto& entry : local_ipv4_addresses(probes)) {
        kinds[entry.address] = entry.kind;
    }

    CHECK(kinds == std::map<std::string, std::string>{{"192.168.0.206", "lan"},
                                                      {"100.65.148.53", "tailscale"},
                                                      {"172.21.32.1", "virtual"},
                                                      {"10.2.0.2", "vpn"}});
    CHECK(discover_lan_ipv4(probes) == "192.168.0.206");
}

TEST_CASE("test_only_a_vpn_is_offered_as_a_vpn") {
    const auto best = best_lan_address(machine({{kArchLaptop[4], kArchLaptop[3]}}, "10.2.0.2"));

    REQUIRE(best);
    CHECK(best->address == "10.2.0.2");
    CHECK(best->kind == kVpn);
    CHECK(best->is_vpn);
    CHECK_FALSE(best->is_lan);
}

TEST_CASE("test_the_default_route_breaks_a_tie_between_lans") {
    const auto probes =
        machine({{iface("eth0", "192.168.1.5", 24), iface("eth1", "10.0.0.8", 24)}}, "10.0.0.8");

    CHECK(discover_lan_ipv4(probes) == "10.0.0.8");
}

TEST_CASE("test_without_an_interface_list_the_route_probe_still_answers") {
    CHECK(discover_lan_ipv4(machine(std::vector<LanInterface>{}, "192.168.1.44", {"127.0.1.1"})) ==
          "192.168.1.44");
}

TEST_CASE("test_a_listed_computer_never_resolves_its_host_name") {
    // The lookup stalled pairing and the Remote page's refresh on macOS 15 CI runners; the
    // listing already holds every address it could add.
    auto probes = machine(kArchLaptop, "10.2.0.2");
    bool resolved = false;
    probes.hostname_addresses = [&resolved] {
        resolved = true;
        return std::vector<std::string>{};
    };

    CHECK(discover_lan_ipv4(probes) == "192.168.0.118");
    CHECK_FALSE(resolved);
}

TEST_CASE("test_loopback_host_names_are_skipped") {
    CHECK(discover_lan_ipv4(machine(std::vector<LanInterface>{}, "127.0.0.1",
                                    {"127.0.1.1", "10.0.0.8"})) == "10.0.0.8");
}

TEST_CASE("test_nothing_usable_is_none") {
    CHECK_FALSE(best_lan_address(machine({{kArchLaptop[3]}}, std::nullopt)).has_value());
}

TEST_CASE("test_route_selected_lan_address_wins") {
    CHECK(discover_lan_ipv4(machine(std::vector<LanInterface>{}, "192.168.1.44")) ==
          "192.168.1.44");
}

TEST_CASE("test_loopback_hostname_mapping_is_skipped_for_real_interface") {
    CHECK(discover_lan_ipv4(machine(std::vector<LanInterface>{}, "127.0.0.1",
                                    {"127.0.1.1", "10.0.0.8"})) == "10.0.0.8");
}

TEST_CASE("the probes are asked in Python's order and the host name only without a listing") {
    std::vector<std::string> calls;
    LanProbes probes;
    probes.interfaces = [&] {
        calls.push_back("interfaces");
        return std::optional<std::vector<LanInterface>>();
    };
    probes.route_address = [&] {
        calls.push_back("route");
        return std::optional<std::string>("192.168.4.4");
    };
    probes.hostname_addresses = [&] {
        calls.push_back("hostname");
        return std::vector<std::string>{"10.4.4.4"};
    };

    const auto addresses = local_ipv4_addresses(probes);

    CHECK(calls == std::vector<std::string>{"interfaces", "route", "hostname"});
    REQUIRE(addresses.size() == 2);
    CHECK(addresses[0].address == "192.168.4.4"); // the default route breaks the tie
    CHECK(addresses[1].address == "10.4.4.4");
}

TEST_CASE("test_linux_sysfs_marks_tunnels") {
    // With the sysfs answers injected, on any platform; a tunnel with an ordinary-looking name
    // and prefix still counts.
    const LinuxSysfs sysfs{[](const LanInterface& interface) {
                               return interface.name == "wg0" ? std::optional<std::string>("vpn")
                                                              : std::nullopt;
                           },
                           [](const std::string&) {
                               return false;
                           }};

    CHECK(classify(iface("wg0", "10.8.0.2", 24), &sysfs).kind == kVpn);
    CHECK(classify(iface("br0", "192.168.1.9", 24), &sysfs).kind == kLan);
}

TEST_CASE("classify refuses what is not an address, as Python raises ValueError") {
    CHECK_THROWS_AS(classify(iface("eth0", "junk", 24), nullptr), std::invalid_argument);
}

TEST_CASE("test_this_computers_listing_does_not_fail", "[platform]") {
    // Runs the real platform listing (GetAdaptersAddresses or getifaddrs); sends nothing.
    for (const auto& entry : local_ipv4_addresses()) {
        CHECK((entry.kind == kLan || entry.kind == kVpn || entry.kind == kVirtual ||
               entry.kind == kTailscale));
        CHECK(usable(entry.address));
    }
}

TEST_CASE("_usable matches Python", "[golden]") {
    golden::for_each(kModule, "_usable", [](const golden::Case& c) {
        CHECK(usable(c.arg<std::string>(0)) == c.result<bool>());
    });
}

TEST_CASE("_prefix_length matches Python", "[golden]") {
    golden::for_each(kModule, "_prefix_length", [](const golden::Case& c) {
        std::vector<std::uint8_t> mask;
        for (const auto value : c.arg<std::vector<int>>(0)) {
            mask.push_back(static_cast<std::uint8_t>(value));
        }
        CHECK(prefix_length(mask) == c.result<int>());
    });
}

TEST_CASE("classify matches Python", "[golden]") {
    golden::for_each(kModule, "classify", [](const golden::Case& c) {
        const LanInterface interface = interface_from(c.args()[0]);
        if (c.raises()) {
            CHECK_THROWS_AS(classify(interface, nullptr), std::invalid_argument);
            return;
        }
        const LocalAddress entry = classify(interface, nullptr);
        INFO(c.label());
        CHECK(golden::equal(c.result<Json>(), as_record(entry)));
        CHECK(entry.is_lan == (entry.kind == kLan));
        CHECK(entry.is_vpn == (entry.kind == kVpn));
        CHECK(entry.is_virtual == (entry.kind == kVirtual));
        CHECK(entry.is_tailscale == (entry.kind == kTailscale));
    });
}

TEST_CASE("local_ipv4_addresses matches Python", "[golden]") {
    golden::for_each(kModule, "local_ipv4_addresses", [](const golden::Case& c) {
        Json actual = Json::array();
        for (const auto& entry : local_ipv4_addresses(probes_from(c))) {
            actual.push_back(as_record(entry));
        }
        INFO(c.label());
        CHECK(golden::equal(c.result<Json>(), actual));
    });
}

TEST_CASE("best_lan_address matches Python", "[golden]") {
    golden::for_each(kModule, "best_lan_address", [](const golden::Case& c) {
        const auto best = best_lan_address(probes_from(c));
        INFO(c.label());
        CHECK(golden::equal(c.result<Json>(), best ? as_record(*best) : Json(nullptr)));
    });
}

TEST_CASE("discover_lan_ipv4 matches Python", "[golden]") {
    golden::for_each(kModule, "discover_lan_ipv4", [](const golden::Case& c) {
        INFO(c.label());
        CHECK(discover_lan_ipv4(probes_from(c)) == c.result<std::optional<std::string>>());
    });
}
