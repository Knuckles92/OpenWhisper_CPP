"""remote_protocol: first messages the host refuses. A hello of another protocol version gets the
"protocol" error and close 4400; a binary first frame, text that is not JSON, JSON that is not
an object and an unknown type close 4400 "expected hello"; silence closes 4408 "handshake
timeout" (the handshake timeout is patched to 0.5 s for that step only)."""
from __future__ import annotations

import json

import remote_wire

NORMALIZE: list = []


def build(tmp):
    return remote_wire.Context(tmp, handshake_timeout_s={"silence": 0.5})


def _send_first(ctx, message):
    host = ctx.host()
    ctx.client("raw")
    ws = ctx.raw(host)
    ws.send(message)
    ctx.outcome("received", ctx.drain(ws))


def protocol_2(ctx):
    _send_first(ctx, json.dumps({"type": "hello", "protocol": 2, "token": "not-a-token"}))


def pair_protocol_2(ctx):
    _send_first(ctx, json.dumps({"type": "pair", "protocol": 2, "device_name": "laptop",
                                 "code": "246810"}))


def binary_first(ctx):
    _send_first(ctx, b"\x00\x00\x00\x02{}")


def not_json(ctx):
    _send_first(ctx, "hello")


def not_an_object(ctx):
    _send_first(ctx, json.dumps([1, 2]))


def unknown_type(ctx):
    _send_first(ctx, json.dumps({"type": "bogus", "protocol": 1}))


def silence(ctx):
    from services.remote_asr import host as host_module

    ctx.patch(host_module, "HANDSHAKE_TIMEOUT_S", 0.5,
              "services.remote_asr.host.HANDSHAKE_TIMEOUT_S = 0.5 (10.0 in OpenWhisper)")
    host = ctx.host()
    ctx.client("raw")
    ws = ctx.raw(host)
    ctx.outcome("received", ctx.drain(ws))


STEPS = [
    ("protocol_2", protocol_2),
    ("pair_protocol_2", pair_protocol_2),
    ("binary_first", binary_first),
    ("not_json", not_json),
    ("not_an_object", not_an_object),
    ("unknown_type", unknown_type),
    ("silence", silence),
]
