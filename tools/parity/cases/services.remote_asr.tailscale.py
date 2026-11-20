"""Curated inputs for services/remote_asr/tailscale.py (M5.2 net-server): which addresses are
Tailscale's, and what `tailscale status --json` documents parse into.

parse_status returns a frozen dataclass; the wrapper records it as dataclasses.asdict plus its
properties (running, and is_desktop per peer), so the C++ replay compares plain JSON. The status
documents are trimmed to the fields read, with made-up names, after the fixture of
tests/test_remote_engine.py; the junk cases hold what json.loads could give in each field.
"""

import copy
import dataclasses

OWNER = "owner@example.com"


def _status_json():
    return {
        "BackendState": "Running",
        "Self": {"HostName": "LAPTOP-1", "DNSName": "laptop.tail0000.ts.net.", "UserID": 1,
                 "TailscaleIPs": ["100.64.0.1", "fd7a:115c:a1e0::1"]},
        "CurrentTailnet": {"Name": OWNER, "MagicDNSSuffix": "tail0000.ts.net"},
        "User": {"1": {"LoginName": OWNER}, "2": {"LoginName": "friend@example.com"},
                 "3": {"LoginName": "tagged-devices"}},
        "Peer": {
            "a": {"HostName": "devbox", "DNSName": "devbox.tail0000.ts.net.", "OS": "linux",
                  "Online": True, "UserID": 1, "TailscaleIPs": ["100.64.0.2", "fd7a:115c:a1e0::2"]},
            "b": {"HostName": "friends-pc", "DNSName": "friends-pc.tail0000.ts.net.",
                  "OS": "windows", "Online": True, "UserID": 2, "TailscaleIPs": ["100.64.0.3"]},
            "c": {"HostName": "gpu-server", "DNSName": "gpu.tail0000.ts.net.", "OS": "linux",
                  "Online": True, "UserID": 3, "Tags": ["tag:server"],
                  "TailscaleIPs": ["100.64.0.4"]},
            "d": {"HostName": "localhost", "DNSName": "phone.tail0000.ts.net.", "OS": "iOS",
                  "Online": True, "UserID": 1, "TailscaleIPs": ["100.64.0.5"]},
            "e": {"HostName": "Shared Mac", "DNSName": "mac.other.ts.net.", "OS": "macOS",
                  "Online": False, "UserID": 1, "ShareeNode": True,
                  "TailscaleIPs": ["100.64.0.6"]},
        },
    }


def _with(changes=None, **top):
    data = _status_json()
    data.update(top)
    for path, value in (changes or {}).items():
        target = data
        keys = path.split("/")
        for key in keys[:-1]:
            target = target[key]
        if value is _DROP:
            target.pop(keys[-1], None)
        else:
            target[keys[-1]] = copy.deepcopy(value)
    return data


_DROP = object()


def _peer(**fields):
    node = {"HostName": "node", "DNSName": "node.tail0000.ts.net.", "OS": "linux",
            "Online": True, "UserID": 1, "TailscaleIPs": ["100.64.0.20"]}
    node.update(fields)
    return node


_STATUS_CASES = [
    # The fixture: running, the owner's own computers first, tags and shares never "mine".
    _status_json(),
    # Every backend state, and what is not one.
    *[_with(BackendState=state) for state in (
        "Stopped", "NeedsLogin", "NeedsMachineAuth", "Starting", "NoState", "", "running",
        "Weird", None, 1, True)],
    _with({"BackendState": _DROP}),
    # No IPv4 for this computer; no Self; a tagged Self owns nothing.
    _with({"Self/TailscaleIPs": ["fd7a:115c:a1e0::1"]}),
    _with({"Self": _DROP}),
    _with({"Self/Tags": ["tag:server"]}),
    _with({"Self/Tags": []}),
    # Stopped with nothing else, and an empty document.
    {"BackendState": "Stopped"},
    {},
    # Peers: none, not a dict, a node that is not a dict, peers without an IPv4.
    _with(Peer={}),
    _with(Peer=[]),
    _with(Peer={"x": "not a node", "y": None, "z": [1]}),
    _with(Peer={"v6": _peer(TailscaleIPs=["fd7a:115c:a1e0::9"]), "none": _peer(TailscaleIPs=[]),
                "missing": {"HostName": "bare"}}),
    # Addresses with a prefix, non-string entries, a string and a dict in place of the list.
    _with(Peer={"cidr": _peer(TailscaleIPs=["100.64.0.9/32", "fd7a:115c:a1e0::9/128"]),
                "mixed": _peer(HostName="mixed", DNSName="", TailscaleIPs=[7, None, "100.64.0.10"]),
                "text": _peer(HostName="text", DNSName="", TailscaleIPs="100.64.0.11"),
                "colon": _peer(HostName="colon", DNSName="", TailscaleIPs="fd7a::1"),
                "dict": _peer(HostName="dict", DNSName="", TailscaleIPs={"100.64.0.12": True})}),
    # A number where the list belongs: Python's iteration raises TypeError.
    _with(Peer={"n": _peer(TailscaleIPs=5)}),
    _with({"Self/TailscaleIPs": 1.5}),
    _with({"Self/TailscaleIPs": True}),
    _with({"Self/TailscaleIPs": 0}),
    # Names: no DNS name falls back to the host name, then the address; extra dots; non-strings.
    _with(Peer={"a": _peer(DNSName="", HostName="Only-Host"),
                "b": _peer(DNSName=None, HostName=None, TailscaleIPs=["100.64.0.21"]),
                "c": _peer(DNSName="dots.example...", HostName="x"),
                "d": _peer(DNSName=".leading", HostName="fallback"),
                "e": _peer(DNSName=12, HostName=3.0),
                "f": _peer(DNSName="", HostName=["a", 1], TailscaleIPs=["100.64.0.22"])}),
    # Users: ids as strings, floats and missing; users not a dict; a user that is not a dict.
    _with(Peer={"s": _peer(UserID="1", HostName="str-id", DNSName=""),
                "f": _peer(UserID=1.0, HostName="float-id", DNSName=""),
                "m": _peer(HostName="no-id", DNSName="")},
          User={"1": {"LoginName": OWNER}, "1.0": {"LoginName": "float@example.com"},
                "None": {"LoginName": "none@example.com"}}),
    _with(User=["not", "a", "dict"]),
    _with(User={"1": "not a dict"}),
    _with(User={"1": {"LoginName": None}}),
    _with({"CurrentTailnet": _DROP}),
    _with(CurrentTailnet={"Name": 42}),
    # Sorting: mine first, then by name without regard to case, stable for equal names.
    _with(Peer={"1": _peer(HostName="beta", DNSName="", TailscaleIPs=["100.64.1.1"]),
                "2": _peer(HostName="Alpha", DNSName="", TailscaleIPs=["100.64.1.2"]),
                "3": _peer(HostName="alpha", DNSName="", TailscaleIPs=["100.64.1.3"]),
                "4": _peer(HostName="Zed", DNSName="", UserID=2, TailscaleIPs=["100.64.1.4"]),
                "5": _peer(HostName="Ärger", DNSName="", TailscaleIPs=["100.64.1.5"]),
                "6": _peer(HostName="able", DNSName="", UserID=2, TailscaleIPs=["100.64.1.6"])}),
    # Operating systems in any case; online and share flags of every truthiness.
    _with(Peer={"w": _peer(HostName="w", DNSName="", OS="WINDOWS", Online=1,
                           TailscaleIPs=["100.64.2.1"]),
                "m": _peer(HostName="m", DNSName="", OS="MacOS", Online="", ShareeNode=0,
                           TailscaleIPs=["100.64.2.2"]),
                "a": _peer(HostName="a", DNSName="", OS="android", Online=None,
                           ShareeNode="yes", TailscaleIPs=["100.64.2.3"]),
                "n": _peer(HostName="n", DNSName="", OS=None, Tags=None,
                           TailscaleIPs=["100.64.2.4"]),
                "t": _peer(HostName="t", DNSName="", Tags="tag:x", TailscaleIPs=["100.64.2.5"])}),
]


def _parse_status(module, data):
    status = module.parse_status(data)
    out = dataclasses.asdict(status)
    out["running"] = status.running
    for peer, row in zip(status.peers, out["peers"]):
        row["is_desktop"] = peer.is_desktop
    return out


CASES = {
    "is_tailscale_address": [[address] for address in [
        "100.64.0.2", "100.127.255.254", "100.128.0.1", "100.63.255.255", "100.64.0.0",
        "100.100.100.100", "192.168.1.20", "127.0.0.1", "fd7a:115c:a1e0::9636:1603",
        "FD7A:115C:A1E0::1", "fd7a:115c:a1e0:ffff:ffff:ffff:ffff:ffff", "fd7a:115c:a1e1::1",
        "fd7a:115c:a1df::1", "::ffff:100.64.0.2", "::ffff:192.168.0.1", "::100.64.0.1",
        "fe80::1%eth0", "fd7a:115c:a1e0::1%3", "100.64.0.1%5", "100.64.0.1%", "devbox.local",
        "", "junk", "1.2.3", " 100.64.0.1", "100.64.0.1/32", "100.064.0.1",
    ]],
    "parse_status": [[data] for data in _STATUS_CASES],
}

WRAPPERS = {"parse_status": _parse_status}
