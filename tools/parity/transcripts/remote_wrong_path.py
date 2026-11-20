"""remote_wrong_path: an upgrade to /x is answered HTTP 404 with a text body and no upgrade, and
the client says the address isn't a remote engine."""
from __future__ import annotations

import remote_wire

NORMALIZE: list = []


def build(tmp):
    return remote_wire.Context(tmp, path="/x")


def wrong_path(ctx):
    from services.remote_asr import client
    from services.remote_asr.protocol import format_address

    host = ctx.host()
    ctx.client("probe_host")
    ctx.patch(client, "_uri", lambda h, p: f"wss://{format_address(h, p)}/x",
              "services.remote_asr.client._uri asks for /x instead of the engine path")
    ctx.call("probe_host", client.probe_host, remote_wire.LOOPBACK, host.port)


STEPS = [("wrong_path", wrong_path)]
