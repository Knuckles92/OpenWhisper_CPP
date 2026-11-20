"""remote_hello_ready: a paired computer's hello and the host's ready. A host with models, runtime,
engine controls, records and MCP callbacks; a host with none; an unknown token; an engine still
loading; an update hold; the busy refusal of a host whose one connection slot is taken (after
hello) and of one whose one handshake slot is taken (before hello, read by a raw client that
sends nothing first, so the order of the frames does not depend on timing)."""
from __future__ import annotations

import remote_wire

UNPAIRED = "not-a-paired-token"
NORMALIZE = [
    {"pointer": "/token", "as": "token", "when": {"type": "paired"}},
    {"pointer": "/device_id", "as": "device_id", "when": {"type": "paired"}},
    {"pointer": "/token", "as": "token", "when": {"type": "hello"}, "keep": [UNPAIRED]},
    {"pointer": "/host/fingerprint", "as": "fingerprint"},
    {"pointer": "/device/id", "as": "device_id", "when": {"type": "ready"}},
]

MODELS = [
    {"family": "parakeet", "model": "parakeet-v3", "label": "Parakeet TDT 0.6B v3"},
    {"family": "parakeet", "model": "parakeet-v2", "label": "Parakeet TDT 0.6B v2"},
]
RUNTIME = {
    "family": "parakeet",
    "model": "parakeet-v3",
    "selected": {"device": "auto", "language": "auto"},
    "devices": ["auto", "cpu", "cuda"],
    "compute_types": {},
    "languages": ["auto", "en", "de"],
    "gpu": {"name": "NVIDIA GeForce RTX 4070", "total_mib": 12282},
    "dependencies": [],
}
RECORDS_SUMMARY = {"dictation": {"count": 3, "bytes": 30720}, "meeting": {"count": 1, "bytes": 4096}}


def build(tmp):
    return remote_wire.Context(tmp, models=MODELS, runtime=RUNTIME,
                               records_summary=RECORDS_SUMMARY)


def full_host(ctx, **kwargs):
    """Every optional callback set: what a OpenWhisper_CPP host with records and MCP serves."""
    return ctx.host(
        models=lambda: [dict(m) for m in MODELS],
        runtime=lambda: dict(RUNTIME),
        select_model=lambda family, model, device_name, device=None: {},
        model_management=lambda: True,
        manage_models=lambda op, header, device_name: {},
        configure_runtime=lambda family, model, settings, device_name: {},
        records=lambda op, header, payload, device: {},
        records_enabled=lambda: True,
        records_summary=lambda device_id: dict(RECORDS_SUMMARY),
        mcp=lambda op, header, device_name: {},
        # Both permissions on, so ready reports records and mcp_control true (remote_requests
        # records them false).
        mcp_enabled=lambda: True,
        **kwargs)


def _connect(ctx, host, token, label="connect", **kwargs):
    from services.remote_asr.client import RemoteConnection

    connection = RemoteConnection(remote_wire.LOOPBACK, host.port, token,
                                  ctx.values["fingerprint"], **kwargs)
    ctx.call(label, connection.connect)
    return connection


def pair(ctx):
    ctx.values["host"] = host = full_host(ctx, keep=True)
    ctx.client("pair_with_host")
    result = ctx.pair(host)
    ctx.values["token"] = result.token
    ctx.values["fingerprint"] = result.fingerprint


def ready_full(ctx):
    ctx.client("RemoteConnection(reports_recording=True).connect")
    _connect(ctx, ctx.values["host"], ctx.values["token"], reports_recording=True).close()


def ready_plain(ctx):
    host = ctx.host()
    ctx.client("RemoteConnection.connect")
    _connect(ctx, host, ctx.values["token"]).close()


def unauthorized(ctx):
    ctx.client("RemoteConnection.connect")
    _connect(ctx, ctx.values["host"], UNPAIRED).close()


def loading(ctx):
    host = full_host(ctx, engine_settled=lambda: False)
    ctx.client("RemoteConnection(reports_recording=True).connect")
    _connect(ctx, host, ctx.values["token"], reports_recording=True).close()


def update_hold(ctx):
    host = ctx.values["host"]
    hold = host.prepare_update()
    ctx.outcome("prepare_update", {"token": bool(hold.get("token")),
                                   "expires_in": hold.get("expires_in")})
    ctx.client("RemoteConnection(reports_recording=True).connect")
    _connect(ctx, host, ctx.values["token"], reports_recording=True).close()
    ctx.outcome("cancel_update", host.cancel_update(hold["token"]))


def busy_connections(ctx):
    host = ctx.host(max_connections=1)
    ctx.client("RemoteConnection.connect (the second of two)")
    ctx.note("max_connections=1; the first connection holds the one slot")
    first = _connect(ctx, host, ctx.values["token"], label="connect (first)")
    _connect(ctx, host, ctx.values["token"], label="connect (second)").close()
    first.close()


def busy_handshakes(ctx):
    host = ctx.host(max_handshakes=1)
    ctx.client("raw (the second of two, reading before it sends)")
    ctx.note("max_handshakes=1; the first connection holds the one slot by saying nothing")
    first = ctx.raw(host)
    ctx.wait(lambda: host._handshake_slots._value == 0, "the first connection's slot")
    second = ctx.raw(host)
    ctx.outcome("received (second)", ctx.drain(second))
    first.close()


STEPS = [
    ("pair", pair),
    ("ready_full", ready_full),
    ("ready_plain", ready_plain),
    ("unauthorized", unauthorized),
    ("loading", loading),
    ("update_hold", update_hold),
    ("busy_connections", busy_connections),
    ("busy_handshakes", busy_handshakes),
]
