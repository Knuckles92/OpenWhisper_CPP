"""remote_pair_code: pairing with the code the host shows. Pairing closed; a wrong code; the
fifth wrong code, which closes pairing; the right code (paired, close 1000); a code typed with
spaces and dashes (OpenWhisper's client sends only its digits); a code with no digits, refused
before connecting; the same padded and dashed codes sent as they are by a raw client. The host's
random code is replaced by setup.pairing_code as soon as pairing opens."""
from __future__ import annotations

import remote_wire

NORMALIZE = [
    {"pointer": "/token", "as": "token", "when": {"type": "paired"}},
    {"pointer": "/device_id", "as": "device_id", "when": {"type": "paired"}},
    {"pointer": "/host/fingerprint", "as": "fingerprint"},
]
WRONG = "111111"


def build(tmp):
    return remote_wire.Context(tmp, wrong_code=WRONG)


def _pair(ctx, host, code):
    from services.remote_asr.client import pair_with_host

    ctx.client("pair_with_host")
    return ctx.call("pair_with_host", pair_with_host, remote_wire.LOOPBACK, host.port, code,
                    remote_wire.DEVICE_NAME)


def pairing_closed(ctx):
    _pair(ctx, ctx.host(), remote_wire.PAIRING_CODE)


def wrong_code(ctx):
    host = ctx.host()
    ctx.open_pairing(host)
    _pair(ctx, host, WRONG)
    ctx.outcome("pairing_open_after", host.pairing_status() is not None)


def fifth_wrong_code(ctx):
    host = ctx.host()
    ctx.open_pairing(host)
    for _ in range(5):
        _pair(ctx, host, WRONG)
    ctx.outcome("pairing_open_after", host.pairing_status() is not None)
    # Pairing closed itself: even the right code is turned away now.
    _pair(ctx, host, remote_wire.PAIRING_CODE)


def right_code(ctx):
    host = ctx.host()
    ctx.open_pairing(host)
    _pair(ctx, host, remote_wire.PAIRING_CODE)
    ctx.outcome("pairing_open_after", host.pairing_status() is not None)


def spaced_code(ctx):
    host = ctx.host()
    ctx.open_pairing(host)
    code = remote_wire.PAIRING_CODE
    _pair(ctx, host, f" {code[:2]}-{code[2:4]} {code[4:]} ")


def no_digits(ctx):
    host = ctx.host()
    ctx.open_pairing(host)
    _pair(ctx, host, " - ")


def raw_codes(ctx):
    """The host's side of a code typed with spaces and dashes, which OpenWhisper's client never
    sends: the host only strips surrounding whitespace before comparing, so the padded code pairs
    and the dashed one is a wrong code. Each code has its own host."""
    import json

    ctx.client("raw (pair frames whose codes the client did not clean)")
    code = remote_wire.PAIRING_CODE
    for sent in (f" {code} ", f"{code[:2]}-{code[2:4]}-{code[4:]}"):
        host = ctx.host()
        ctx.open_pairing(host)
        ws = ctx.raw(host)
        ws.send(json.dumps({"type": "pair", "protocol": 1,
                            "device_name": remote_wire.DEVICE_NAME, "code": sent}))
        ctx.outcome(f"received ({sent!r})", [json.loads(m) for m in ctx.drain(ws)])
        ctx.outcome(f"pairing_open_after ({sent!r})", host.pairing_status() is not None)


STEPS = [
    ("pairing_closed", pairing_closed),
    ("wrong_code", wrong_code),
    ("fifth_wrong_code", fifth_wrong_code),
    ("right_code", right_code),
    ("spaced_code", spaced_code),
    ("no_digits", no_digits),
    ("raw_codes", raw_codes),
]
