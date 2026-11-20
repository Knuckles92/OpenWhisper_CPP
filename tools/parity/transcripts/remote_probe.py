"""remote_probe: probe_host against a host whose engine is available, one whose engine is not,
and one with a trusted Tailscale owner. The host answers before any version check and closes
1000."""
from __future__ import annotations

import remote_wire

NORMALIZE = [{"pointer": "/host/fingerprint", "as": "fingerprint"}]


def build(tmp):
    from tests.fakes.remote import FakeEngine

    return remote_wire.Context(tmp, engines={"available": FakeEngine().describe(),
                                             "unavailable": FakeEngine(available=False).describe()})


def _probe(ctx, host):
    from services.remote_asr.client import probe_host

    ctx.client("probe_host")
    ctx.call("probe_host", probe_host, remote_wire.LOOPBACK, host.port)


def available(ctx):
    _probe(ctx, ctx.host())


def unavailable(ctx):
    from tests.fakes.remote import FakeEngine

    _probe(ctx, ctx.host(engine=FakeEngine(available=False)))


def tailscale_owner(ctx):
    ctx.note("tailscale_owner returns owner@example.com")
    _probe(ctx, ctx.host(tailscale_owner=lambda: "owner@example.com"))


STEPS = [
    ("available", available),
    ("unavailable", unavailable),
    ("tailscale_owner", tailscale_owner),
]
