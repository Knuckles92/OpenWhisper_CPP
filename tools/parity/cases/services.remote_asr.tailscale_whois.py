"""Curated inputs for the whois half of services/remote_asr/tailscale.py (M9.2 remote-wire):
what `tailscale whois --json` documents parse into, and which connections own_device accepts.
M5.2's services.remote_asr.tailscale.py owns the status vectors of the same module, so these
live in a file of their own.

There is no module of this name: the case file registers an empty placeholder under it, which
golden_vectors.py then "imports", and every function is a wrapper over the real
services.remote_asr.tailscale. The CLI is never run: the wrappers replace tailscale._run_json for
the call with one that answers the given document, raising TailscaleError for a document that is
not an object (as the real one does) or for the string "error:<text>". A PeerIdentity is
recorded as dataclasses.asdict.
"""

import dataclasses
import sys
import types

_NAME = "services.remote_asr.tailscale_whois"
sys.modules.setdefault(_NAME, types.ModuleType(_NAME))

OWNER = "owner@example.com"


def _answering(document):
    from services.remote_asr import tailscale

    def run_json(args, timeout):
        if isinstance(document, str) and document.startswith("error:"):
            raise tailscale.TailscaleError(document[len("error:"):])
        if not isinstance(document, dict):
            raise tailscale.TailscaleError("Tailscale's answer was unreadable")
        return document

    return tailscale, run_json


def _call(document, fn):
    tailscale, run_json = _answering(document)
    original = tailscale._run_json
    tailscale._run_json = run_json
    try:
        identity = fn(tailscale)
    finally:
        tailscale._run_json = original
    return None if identity is None else dataclasses.asdict(identity)


def _whois(module, document, address):
    return _call(document, lambda tailscale: tailscale.whois(address))


def _own_device(module, remote, local, owner, document):
    return _call(document, lambda tailscale: tailscale.own_device(remote, local, owner))


_DEVBOX = {"Node": {"Name": "devbox.tail0000.ts.net.", "ComputedName": "devbox", "Tags": None},
           "UserProfile": {"LoginName": OWNER, "DisplayName": "Owner"}}
_TAGGED = {"Node": {"Name": "gpu.tail0000.ts.net.", "ComputedName": "gpu",
                    "Tags": ["tag:server"]},
           "UserProfile": {"LoginName": "tagged-devices"}}

CASES = {
    "whois": [
        [_DEVBOX, "100.64.0.2"],
        [_TAGGED, "100.64.0.4"],
        [{"Node": {"Name": "gpu.tail0000.ts.net.", "Tags": ["tag:server"]},
          "UserProfile": {"LoginName": OWNER}}, "100.64.0.4"],
        [{"Node": {"Name": "box.tail0000.ts.net."}, "UserProfile": {"LoginName": "tagged-devices"}},
         "100.64.0.5"],
        [{"Node": {"Name": "laptop.tail0000.ts.net."}}, "100.64.0.6"],
        [{"Node": {"ComputedName": "", "Name": ""}, "UserProfile": {}}, "100.64.0.7"],
        [{"Node": {"Name": "..."}, "UserProfile": {"LoginName": None}}, "100.64.0.8"],
        [{"Node": {"Name": "x.y."}, "UserProfile": {"LoginName": 7}}, "fd7a:115c:a1e0::9"],
        [{"Node": "not a dict", "UserProfile": ["x"]}, "100.64.0.10"],
        [{"Node": {"Tags": [], "ComputedName": 12}, "UserProfile": {"LoginName": OWNER}},
         "100.64.0.11"],
        [{"Node": {"Tags": "tag:x", "Name": "t.example."}}, "100.64.0.12"],
        [{}, "100.64.0.13"],
        [[], "100.64.0.14"],
        ["error:failed to connect to local tailscaled", "100.64.0.15"],
        [_DEVBOX, "192.168.1.20"],
        [_DEVBOX, "devbox"],
        [_DEVBOX, "::ffff:100.64.0.2"],
        [_DEVBOX, "fe80::1%eth0"],
    ],
    "own_device": [
        ["100.64.0.2", "100.64.0.1", OWNER, _DEVBOX],
        ["100.64.0.3", "100.64.0.1", OWNER,
         {"Node": {"ComputedName": "friends-pc"},
          "UserProfile": {"LoginName": "friend@example.com"}}],
        ["100.64.0.4", "100.64.0.1", OWNER, _TAGGED],
        ["100.64.0.4", "100.64.0.1", OWNER,
         {"Node": {"ComputedName": "gpu", "Tags": ["tag:server"]},
          "UserProfile": {"LoginName": OWNER}}],
        ["100.64.0.2", "100.64.0.1", "", _DEVBOX],
        ["100.64.0.2", "192.168.1.5", OWNER, _DEVBOX],
        ["192.168.1.9", "100.64.0.1", OWNER, _DEVBOX],
        ["100.64.0.2", "100.64.0.1", OWNER, "error:no such peer"],
        ["100.64.0.2", "100.64.0.1", OWNER.upper(), _DEVBOX],
        ["::ffff:100.64.0.2", "::ffff:100.64.0.1", OWNER, _DEVBOX],
        ["fd7a:115c:a1e0::2", "fd7a:115c:a1e0::1", OWNER, _DEVBOX],
    ],
}

WRAPPERS = {"whois": _whois, "own_device": _own_device}
