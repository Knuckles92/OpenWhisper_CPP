"""remote_pair_approval: request_pairing, which the host's owner allows on that screen after both
screens show the same six digits. Allowed; denied; nobody answering (the waiting request's
deadline is moved to now); the client canceling; a host whose reveal breaks its commitment;
refused for an address outside this network (allowed_sender patched); limited (the address's
request log patched to four recent requests); busy (a second request while one waits); sharing
stopped while it waits. Each step has its own host, so request limits never carry over."""
from __future__ import annotations

import collections
import threading
import time

import remote_wire

NORMALIZE = [
    {"pointer": "/commit", "as": "commit", "when": {"type": "pair_commit"}},
    {"pointer": "/nonce", "as": "nonce", "when": {"type": "pair_nonce"}},
    {"pointer": "/nonce", "as": "nonce", "when": {"type": "pair_reveal"}},
    {"pointer": "/token", "as": "token", "when": {"type": "paired"}},
    {"pointer": "/device_id", "as": "device_id", "when": {"type": "paired"}},
    {"pointer": "/host/fingerprint", "as": "fingerprint"},
]
BROKEN_NONCE = "5a" * 32


def build(tmp):
    return remote_wire.Context(tmp)


class _Request:
    """request_pairing on its own thread, as the Settings page runs it."""

    def __init__(self, ctx, host, label="request_pairing"):
        from services.remote_asr.client import request_pairing

        self.codes = []
        self.canceled = threading.Event()
        ctx.client("request_pairing")
        self.thread = ctx.background(
            ctx.call, label, request_pairing, remote_wire.LOOPBACK, host.port,
            remote_wire.DEVICE_NAME, on_code=self.codes.append, canceled=self.canceled)


def _shown(ctx, host, request):
    """The request as the host shows it, once both sides have the six digits."""
    ctx.wait(lambda: host.pending_request() is not None and request.codes,
             "the host to show the request")
    pending = host.pending_request()
    ctx.outcome("same_digits_on_both_screens", pending["sas"] == request.codes[0])
    ctx.outcome("host_shows", {"name": pending["name"], "address": pending["address"]})
    return pending


def allowed(ctx):
    host = ctx.host()
    request = _Request(ctx, host)
    pending = _shown(ctx, host, request)
    host.answer_request(pending["id"], True)
    request.thread.join()


def denied(ctx):
    host = ctx.host()
    request = _Request(ctx, host)
    pending = _shown(ctx, host, request)
    host.answer_request(pending["id"], False)
    request.thread.join()


def timeout(ctx):
    host = ctx.host()
    request = _Request(ctx, host)
    _shown(ctx, host, request)
    ctx.note("the waiting request's expires_at is set to now (the host waits 120 s)")
    host._request.expires_at = time.monotonic()
    request.thread.join()


def canceled(ctx):
    host = ctx.host()
    request = _Request(ctx, host)
    _shown(ctx, host, request)
    request.canceled.set()
    request.thread.join()
    ctx.wait(lambda: host.pending_request() is None, "the host to drop the request")


def broken_reveal(ctx):
    host = ctx.host()
    original = host._send

    def send(ws, message):
        if message.get("type") == "pair_reveal":
            message = {**message, "nonce": BROKEN_NONCE}
        original(ws, message)

    ctx.patch(host, "_send", send,
              "this host's pair_reveal carries another nonce than the one it committed to")
    request = _Request(ctx, host)
    request.thread.join()
    ctx.wait(lambda: host._request is None, "the host to drop the request")


def refused(ctx):
    from services.remote_asr import discovery

    ctx.patch(discovery, "allowed_sender", lambda address: False,
              "services.remote_asr.discovery.allowed_sender refuses every address")
    host = ctx.host()
    request = _Request(ctx, host)
    request.thread.join()


def limited(ctx):
    host = ctx.host()
    ctx.patch(host, "_request_log",
              {remote_wire.LOOPBACK: collections.deque([time.monotonic()] * 4)},
              "this host's request log holds four requests from 127.0.0.1 made just now")
    request = _Request(ctx, host)
    request.thread.join()


def busy(ctx):
    host = ctx.host()
    first = _Request(ctx, host, "request_pairing (first)")
    pending = _shown(ctx, host, first)
    second = _Request(ctx, host, "request_pairing (second)")
    second.thread.join()
    host.answer_request(pending["id"], False)
    first.thread.join()


def stopped(ctx):
    host = ctx.host()
    request = _Request(ctx, host)
    _shown(ctx, host, request)
    host.stop()
    request.thread.join()


STEPS = [
    ("allowed", allowed),
    ("denied", denied),
    ("timeout", timeout),
    ("canceled", canceled),
    ("broken_reveal", broken_reveal),
    ("refused", refused),
    ("limited", limited),
    ("busy", busy),
    ("stopped", stopped),
]
