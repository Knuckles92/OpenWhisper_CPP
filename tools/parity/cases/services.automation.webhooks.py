"""Curated inputs for services/automation/webhooks.py (M5.11 hook-engines).

    retry_after(value, now)        _retry_after with datetime.now pinned to `now` (ISO 8601, UTC)
    no_answer_copy(timeout_s)
    transport_copy(name)           _transport_copy of a fresh httpx exception of that class
    signed_headers(secret, msg_id, body, timestamp)
                                   _signed_headers with time.time pinned to `timestamp`: the header
                                   pairs in order, or {"refused": message} for a ValueError

Bodies are bytes in Python; the vectors give them as text, encoded as UTF-8 by the wrappers.
"""

from types import SimpleNamespace

NOW = "2026-11-13T12:00:00+00:00"


def _retry_after(module, value, now):
    real = module.datetime
    pinned = real.fromisoformat(now)
    module.datetime = SimpleNamespace(now=lambda tz=None: pinned)
    try:
        return module._retry_after(value)
    finally:
        module.datetime = real


def _transport_copy(module, name):
    import httpx

    return module._transport_copy(getattr(httpx, name)("failed"))


def _signed_headers(module, secret, msg_id, body, timestamp):
    from services.automation import signing

    real = signing.time
    signing.time = SimpleNamespace(time=lambda: timestamp)
    try:
        return list(module._signed_headers(secret, msg_id, body.encode("utf-8")).items())
    except ValueError as exc:
        return {"refused": str(exc)}
    finally:
        signing.time = real


WRAPPERS = {
    "retry_after": _retry_after,
    "transport_copy": _transport_copy,
    "signed_headers": _signed_headers,
}

SECRET = "whsec_MfKQ9r8GKYqrTwjUPD8ILPZIo2LaLaSw"

CASES = {
    "retry_after": [[value, NOW] for value in [
        None, "", " ", "3", " 0 ", "-5", "1.5", "1e3", "1_000", "+7", " 12 ", "inf", "-inf", "nan",
        "0x10", "soon", "3 s", "１２",
        "Wed, 21 Oct 2015 07:28:00 GMT",
        "Fri, 13 Nov 2026 12:00:30 GMT",
        "Fri, 13 Nov 2026 12:00:30 +0100",
        "Fri, 13 Nov 2026 13:00:30 +0100",
        "Fri, 13 Nov 2026 07:01:00 -0500",
        "Fri, 13 Nov 2026 07:01:00 EST",
        "Fri, 13 Nov 2026 04:02:00 PST",
        "Fri, 13 Nov 2026 12:01:00 UT",
        "Fri, 13 Nov 2026 12:01:00 Z",
        "Fri, 13 Nov 2026 12:01:00 -0000",
        "Fri, 13 Nov 2026 12:01:00 +0000",
        "Fri, 13 Nov 2026 12:01:00",
        "Fri, 13 Nov 2026 12:01 GMT",
        "Fri, 13 Nov 2026 12.01.00 GMT",
        "Fri, 13 Nov 2026 12.01 GMT",
        "13 Nov 2026 12:01:00 GMT",
        "Fri,13 Nov 2026 12:01:00 GMT",
        "Friday, 13-Nov-26 12:01:00 GMT",
        "Fri Nov 13 12:01:00 2026",
        "Fri, Nov 13 2026 12:01:00 GMT",
        "Fri, 13 November 2026 12:01:00 GMT",
        "Fri, 13 nov 2026 12:01:00 gmt",
        "Fri, 13 Nov 26 12:01:00 GMT",
        "Fri, 13 Nov 69 12:01:00 GMT",
        "Fri, 13 Nov 2026, 12:01:00 GMT",
        "Fri, 13, Nov 2026 12:01:00 GMT",
        "Fri, 31 Feb 2026 12:00:00 GMT",
        "Fri, 29 Feb 2028 12:00:00 GMT",
        "Fri, 13 Nov 2026 25:00:00 GMT",
        "Fri, 13 Nov 2026 12:60:00 GMT",
        "Fri, 13 Nov 2026 12:01:60 GMT",
        "Fri, 13 Nov 2026 12:01:00 +2400",
        "Fri, 13 Nov 2026 12:01:00 +2359",
        "Fri, 13 Nov 2026 12:01:00 -2359",
        "Fri, 13 Nov 2026 12:01:00 +99999999999999999999",
        "Fri, 13 Nov 10000 12:00:00 GMT",
        "Fri, 13 Nov 0 12:00:00 GMT",
        "Fri, 13 Xyz 2026 12:00:00 GMT",
        "Fri, 13 Nov 2026 12:00:00:00 GMT",
        "Fri, 13 Nov 2026",
        "Fri, 13 Nov 2026 12:01:00+0100",
        "Fri, 13 Nov 2026 12:01:00-0100",
        "Fri, 13 Nov 2026 12:01:00 GMT extra words",
        "Fri, 13 Nov 2026 1_2:01:00 GMT",
    ]],
    "no_answer_copy": [[value] for value in [
        5.0, 1.5, 0.3, 2, 0.1234, 0.0005, 0.0004, 1e-7, 1234567.0, 0.0, 3.0004999, 2.675, 1e16,
        -1, 0.05, 2.5, 100,
    ]],
    "transport_copy": [[name] for name in [
        "ConnectError", "ProxyError", "RemoteProtocolError", "ReadError", "WriteError",
    ]],
    "signed_headers": [
        [SECRET, "evt_1", "{}", 1000],
        [SECRET, "msg_p5jXN8AQM9LWM0D4loKWxJek", '{"test": 2432232314}', 1614265330],
        [SECRET, "evt_é", '{"text":"café"}', 1790000000],
        ["not-a-secret", "evt_1", "{}", 1],
        ["whsec_!!!", "evt_1", "{}", 1],
        ["whsec_", "evt_1", "{}", 1],
    ],
}
