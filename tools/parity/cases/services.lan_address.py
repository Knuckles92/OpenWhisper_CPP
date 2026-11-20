"""Curated inputs for services/lan_address.py (M7.1 net-websocket): which addresses are usable,
netmask prefixes, how one interface address is classified, and how the listed and probed
addresses are ranked.

Interfaces are plain dicts turned into _Interface by the wrappers; LocalAddress results are
recorded as dataclasses.asdict plus the `private` property. The listing functions are replaced
for the length of one call, as tests/test_lan_address.py patches them, so no vector reads this
computer's adapters: `_interfaces`, `_route_address` and `_hostname_addresses` answer from the
case, and the Linux sysfs probes say nothing (the vectors are made on Windows, where classify
never asks them). The fixtures are the two machines of test_lan_address.py plus adapters named
in the M7.1 spec (Wi-Fi, Hyper-V and WSL vEthernet, Docker br-, TAP, WireGuard, Tailscale,
APIPA, loopback).
"""

import dataclasses

ARCH_LAPTOP = [
    {"name": "wlo1", "address": "192.168.0.118", "prefix": 24},
    {"name": "tailscale0", "address": "100.82.22.3", "prefix": 32, "point_to_point": True},
    {"name": "br-7ba443d1c480", "address": "172.18.0.1", "prefix": 16},
    {"name": "docker0", "address": "172.17.0.1", "prefix": 16},
    {"name": "proton0", "address": "10.2.0.2", "prefix": 32, "point_to_point": True},
]
WINDOWS_DESKTOP = [
    {"name": "Tailscale", "address": "100.65.148.53", "prefix": 32,
     "description": "Tailscale Tunnel", "if_type": 53},
    {"name": "Ethernet", "address": "192.168.0.206", "prefix": 24,
     "description": "Realtek(R) PCI(e) Ethernet Controller", "if_type": 6},
    {"name": "vEthernet (WSL (Hyper-V firewall))", "address": "172.21.32.1", "prefix": 20,
     "description": "Hyper-V Virtual Ethernet Adapter", "if_type": 6},
    {"name": "ProtonVPN", "address": "10.2.0.2", "prefix": 32,
     "description": "ProtonVPN Tunnel", "if_type": 53},
]
WINDOWS_MORE = [
    {"name": "Wi-Fi", "address": "192.168.1.23", "prefix": 24,
     "description": "Intel(R) Wi-Fi 6 AX201 160MHz", "if_type": 71},
    {"name": "vEthernet (Default Switch)", "address": "172.29.112.1", "prefix": 20,
     "description": "Hyper-V Virtual Ethernet Adapter #2", "if_type": 6},
    {"name": "vEthernet (WSL)", "address": "172.30.0.1", "prefix": 20,
     "description": "Some Adapter", "if_type": 6},
    {"name": "OpenVPN TAP", "address": "10.8.0.6", "prefix": 24,
     "description": "TAP-Windows Adapter V9", "if_type": 6},
    {"name": "wg-home", "address": "10.9.0.2", "prefix": 32,
     "description": "WireGuard Tunnel", "if_type": 53},
    {"name": "Ethernet 3", "address": "192.168.56.1", "prefix": 24,
     "description": "VirtualBox Host-Only Ethernet Adapter", "if_type": 6},
    {"name": "Ethernet 4", "address": "10.0.75.1", "prefix": 24,
     "description": "DockerNAT", "if_type": 6},
    {"name": "Dial-up", "address": "10.64.1.2", "prefix": 32, "description": "WAN Miniport",
     "if_type": 23},
    {"name": "Teredo", "address": "10.70.0.1", "prefix": 32, "description": "Microsoft Teredo",
     "if_type": 131},
    {"name": "Ethernet 5", "address": "169.254.10.20", "prefix": 16,
     "description": "Realtek USB GbE", "if_type": 6},
    {"name": "Bluetooth", "address": "192.168.44.1", "prefix": 24,
     "description": "Bluetooth Device (Personal Area Network)", "if_type": 6},
    {"name": "Cellular", "address": "100.120.4.9", "prefix": 30,
     "description": "Mobile Broadband", "if_type": 243},
]
POSIX_MORE = [
    {"name": "enp3s0", "address": "192.168.1.5", "prefix": 24},
    {"name": "wg0", "address": "10.8.0.2", "prefix": 24},
    {"name": "tun0", "address": "10.8.1.2", "prefix": 24},
    {"name": "virbr0", "address": "192.168.122.1", "prefix": 24},
    {"name": "veth1234", "address": "172.19.0.5", "prefix": 16},
    {"name": "eth1", "address": "8.8.4.4", "prefix": 24},
    {"name": "eth2", "address": "10.10.10.10", "prefix": 32},
    {"name": "ppp0", "address": "10.11.0.1", "prefix": 24, "point_to_point": True},
    {"name": "Bridge0", "address": "192.168.2.1", "prefix": 24},
    {"name": "UTUN3", "address": "10.12.0.1", "prefix": 24},
    {"name": "zt7nnig26", "address": "10.147.17.5", "prefix": 24},
    {"name": "awdl0", "address": "192.168.3.1", "prefix": 24},
]


def _interface(module, data):
    return module._Interface(
        name=data.get("name", ""),
        address=data["address"],
        prefix=data.get("prefix", 0),
        point_to_point=data.get("point_to_point", False),
        description=data.get("description", ""),
        if_type=data.get("if_type"),
    )


def _record(entry):
    if entry is None:
        return None
    out = dataclasses.asdict(entry)
    out["private"] = entry.private
    return out


class _Machine:
    """Replaces the probes for one call, as the tests' monkeypatch does."""

    def __init__(self, module, interfaces, route, hostname):
        self.module = module
        self.values = {
            "_interfaces": lambda: (None if interfaces is None
                                    else [_interface(module, i) for i in interfaces]),
            "_route_address": lambda: route,
            "_hostname_addresses": lambda: list(hostname),
            "_linux_kind": lambda interface: None,
            "_linux_physical": lambda name: False,
        }
        self.saved = {}

    def __enter__(self):
        for name, value in self.values.items():
            self.saved[name] = getattr(self.module, name)
            setattr(self.module, name, value)

    def __exit__(self, *exc):
        for name, value in self.saved.items():
            setattr(self.module, name, value)


def _classify(module, data):
    return _record(module.classify(_interface(module, data)))


def _prefix_length(module, mask):
    return module._prefix_length(bytes(mask))


def _local_ipv4_addresses(module, interfaces, route, hostname):
    with _Machine(module, interfaces, route, hostname):
        return [_record(entry) for entry in module.local_ipv4_addresses()]


def _best_lan_address(module, interfaces, route, hostname):
    with _Machine(module, interfaces, route, hostname):
        return _record(module.best_lan_address())


def _discover_lan_ipv4(module, interfaces, route, hostname):
    with _Machine(module, interfaces, route, hostname):
        return module.discover_lan_ipv4()


_MACHINES = [
    # The two machines of test_lan_address.py, with the routes they were seen with.
    [ARCH_LAPTOP, "10.2.0.2", []],
    [ARCH_LAPTOP, "192.168.0.118", []],
    [ARCH_LAPTOP, None, ["192.168.9.9"]],
    [WINDOWS_DESKTOP, "10.2.0.2", []],
    [WINDOWS_DESKTOP, "192.168.0.206", []],
    [WINDOWS_MORE, "192.168.1.23", []],
    [WINDOWS_MORE, "10.9.0.2", []],
    [WINDOWS_MORE + WINDOWS_DESKTOP, "10.2.0.2", []],
    [POSIX_MORE, "192.168.1.5", []],
    [POSIX_MORE, "8.8.4.4", []],
    # Only a VPN; a VPN and a bridge; nothing usable.
    [[ARCH_LAPTOP[4], ARCH_LAPTOP[3]], "10.2.0.2", []],
    [[ARCH_LAPTOP[3]], None, []],
    [[ARCH_LAPTOP[2], ARCH_LAPTOP[1]], None, []],
    # The default route breaks a tie between LANs, then physical cards come first.
    [[{"name": "eth0", "address": "192.168.1.5", "prefix": 24},
      {"name": "eth1", "address": "10.0.0.8", "prefix": 24}], "10.0.0.8", []],
    [[{"name": "eth0", "address": "192.168.1.5", "prefix": 24},
      {"name": "eth1", "address": "10.0.0.8", "prefix": 24}], None, []],
    [[{"name": "Ethernet", "address": "10.0.0.8", "prefix": 24, "description": "NIC",
       "if_type": 6},
      {"name": "Other", "address": "10.0.0.9", "prefix": 24, "description": "NIC",
       "if_type": 1}], None, []],
    # A public LAN address ranks after private ones.
    [[{"name": "eth0", "address": "8.8.8.8", "prefix": 24},
      {"name": "eth1", "address": "192.168.7.7", "prefix": 24}], "8.8.8.8", []],
    # No listing, or an empty one: the route, then the host name's addresses.
    [None, "192.168.1.44", ["127.0.1.1"]],
    [[], "192.168.1.44", ["127.0.1.1"]],
    [[], "127.0.0.1", ["127.0.1.1", "10.0.0.8"]],
    [None, None, ["10.0.0.8", "100.70.1.2", "10.0.0.8", "169.254.1.1", "junk", ""]],
    [None, "100.101.102.103", []],
    [None, None, []],
    [None, "0.0.0.0", ["224.0.0.1", "::1", "fe80::1"]],
    # The route probe adds an address the listing missed; the host name is never asked.
    [[ARCH_LAPTOP[3]], "192.168.0.50", ["10.99.99.99"]],
    # Duplicates and unusable listed addresses are dropped.
    [[{"name": "eth0", "address": "192.168.1.5", "prefix": 24},
      {"name": "eth0:1", "address": "192.168.1.5", "prefix": 24},
      {"name": "lo", "address": "127.0.0.1", "prefix": 8},
      {"name": "apipa", "address": "169.254.3.4", "prefix": 16},
      {"name": "none", "address": "0.0.0.0", "prefix": 0}], None, []],
]

_CLASSIFY = (ARCH_LAPTOP + WINDOWS_DESKTOP + WINDOWS_MORE + POSIX_MORE + [
    {"name": "eth0", "address": "100.64.0.1", "prefix": 24},
    {"name": "eth0", "address": "100.127.255.254", "prefix": 24},
    {"name": "eth0", "address": "100.128.0.1", "prefix": 24},
    {"name": "TailScale-x", "address": "192.168.5.5", "prefix": 24},
    {"name": "eth9", "address": "192.168.5.6", "prefix": 24, "description": "my TAILSCALE"},
    {"name": "Ethernet", "address": "192.168.5.7", "prefix": 24, "description": "",
     "if_type": 6},
    {"name": "vEthernet (nat)", "address": "172.20.0.1", "prefix": 20, "description": "",
     "if_type": 6},
    {"name": "Ethernet", "address": "192.168.5.8", "prefix": 32, "description": "",
     "if_type": 6},
    {"name": "Wi-Fi", "address": "192.168.5.9", "prefix": 24,
     "description": "Npcap Loopback Adapter", "if_type": 71},
    {"name": "VPN", "address": "10.5.5.5", "prefix": 24, "description": "", "if_type": 6},
    {"name": "lo", "address": "127.0.0.1", "prefix": 8},
    {"name": "eth0", "address": "169.254.1.1", "prefix": 16},
    {"name": "eth0", "address": "8.8.8.8", "prefix": 24},
    {"name": "", "address": "10.1.1.1", "prefix": 24},
    {"name": "Ethernet", "address": "fe80::1", "prefix": 64},
    {"name": "Ethernet", "address": "::ffff:100.64.0.1", "prefix": 64},
])

CASES = {
    "_usable": [[address] for address in [
        "192.168.0.118", "10.0.0.1", "8.8.8.8", "100.64.0.1", "127.0.0.1", "127.1.2.3",
        "0.0.0.0", "169.254.1.1", "224.0.0.1", "239.255.255.250", "255.255.255.255",
        "240.0.0.1", "::1", "fe80::1", "2001:db8::1", "::ffff:192.168.0.1", "", "junk",
        "192.168.0.1%3", "192.168.000.1", "1.2.3", " 10.0.0.1", "10.0.0.256",
    ]],
    "_prefix_length": [[mask] for mask in [
        [255, 255, 255, 0], [255, 255, 0, 0], [255, 255, 255, 255], [0, 0, 0, 0],
        [255, 255, 240, 0], [255, 0, 255, 0], [128, 0, 0, 0], [255, 255, 255, 252],
    ]],
    "classify": [[data] for data in _CLASSIFY],
    "local_ipv4_addresses": _MACHINES,
    "best_lan_address": _MACHINES,
    "discover_lan_ipv4": _MACHINES,
}

WRAPPERS = {
    "classify": _classify,
    "_prefix_length": _prefix_length,
    "local_ipv4_addresses": _local_ipv4_addresses,
    "best_lan_address": _best_lan_address,
    "discover_lan_ipv4": _discover_lan_ipv4,
}

PINS = {"note": "probes replaced per call; Linux sysfs says nothing (vectors made on win32)"}
