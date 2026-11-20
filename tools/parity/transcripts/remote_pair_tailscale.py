"""remote_pair_tailscale: pairing without a code over Tailscale. Refused by a host without a
trusted owner and, with one, for a computer that is not the owner's (127.0.0.1 is not a tailnet
address, and whois is stubbed as in tests/fakes/remote.py no_real_tailscale); paired when
tailscale.own_device is patched to vouch for the peer."""
from __future__ import annotations

import remote_wire

NORMALIZE = [
    {"pointer": "/token", "as": "token", "when": {"type": "paired"}},
    {"pointer": "/device_id", "as": "device_id", "when": {"type": "paired"}},
    {"pointer": "/host/fingerprint", "as": "fingerprint"},
]
OWNER = "owner@example.com"


def build(tmp):
    return remote_wire.Context(tmp, tailscale_owner=OWNER)


def _pair(ctx, host):
    from services.remote_asr.client import pair_with_host

    ctx.client("pair_with_host(tailscale=True)")
    ctx.call("pair_with_host", pair_with_host, remote_wire.LOOPBACK, host.port, None,
             remote_wire.DEVICE_NAME, tailscale=True)


def no_owner(ctx):
    _pair(ctx, ctx.host())


def not_owners_computer(ctx):
    _pair(ctx, ctx.host(tailscale_owner=lambda: OWNER))


def owners_computer(ctx):
    from services.remote_asr import tailscale

    ctx.patch(tailscale, "own_device",
              lambda remote, local, owner: tailscale.PeerIdentity("laptop-node", owner, False),
              "services.remote_asr.tailscale.own_device names laptop-node, the owner's own "
              "untagged computer")
    _pair(ctx, ctx.host(tailscale_owner=lambda: OWNER))


STEPS = [
    ("no_owner", no_owner),
    ("not_owners_computer", not_owners_computer),
    ("owners_computer", owners_computer),
]
