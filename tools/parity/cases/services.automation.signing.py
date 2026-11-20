"""Curated inputs for services/automation/signing.py (M5.3 automation-core).

Bodies are bytes in Python; the vectors give them as text, encoded as UTF-8 by the wrappers.

    signature(secret, msg_id, timestamp, body)
    headers(secret, msg_id, body, timestamp)          the header pairs, in order
    verify(secret, received, body, tolerance_s, now)  received as an object, in order
    key_bytes(secret)                                 the HMAC key as a list of byte values,
                                                      or "ValueError"
"""


def _key_bytes(module, secret):
    try:
        return list(module._key(secret))
    except ValueError:
        return "ValueError"


WRAPPERS = {
    "signature": lambda m, secret, msg_id, timestamp, body: m.signature(
        secret, msg_id, timestamp, body.encode("utf-8")),
    "headers": lambda m, secret, msg_id, body, timestamp: list(m.headers(
        secret, msg_id, body.encode("utf-8"), timestamp=timestamp).items()),
    "verify": lambda m, secret, received, body, tolerance_s, now: m.verify(
        secret, received, body.encode("utf-8"), tolerance_s=tolerance_s, now=now),
    "key_bytes": _key_bytes,
}

# The published Standard Webhooks example (standard-webhooks/standard-webhooks README).
SECRET = "whsec_MfKQ9r8GKYqrTwjUPD8ILPZIo2LaLaSw"
MSG_ID = "msg_p5jXN8AQM9LWM0D4loKWxJek"
TIMESTAMP = 1614265330
BODY = '{"test": 2432232314}'
EXPECTED = "v1,g0hM9SsE+OTPJTGt/tmIKtSyZlE3uFJELVlNIOLJ1OE="
OTHER = "whsec_" + "QUJDREVGR0hJSktMTU5PUFFSU1RVVldY"


def _received(signature, msg_id=MSG_ID, timestamp=str(TIMESTAMP)):
    return {"webhook-id": msg_id, "webhook-timestamp": timestamp, "webhook-signature": signature}


CASES = {
    "signature": [
        [SECRET, MSG_ID, TIMESTAMP, BODY],
        [SECRET, "evt_1", 1000, "{}"],
        [SECRET, "", 0, ""],
        [SECRET, "id", -5, "café"],
        [OTHER, "evt_é", 1790000000, '{"text":"\U0001f600"}'],
        ["whsec_AA==", "id", 1, "x"],
        ["whsec_AAA=", "id", 1, "x"],
        ["", "id", 1, ""], ["MfKQ9r8", "id", 1, ""], ["whsec_", "id", 1, ""],
        ["whsec_!!!", "id", 1, ""], ["whsec_AA", "id", 1, ""], ["whsec_A", "id", 1, ""],
        ["whsec_AA=", "id", 1, ""], ["whsec_AA==A", "id", 1, ""], ["whsec_=AA", "id", 1, ""],
        ["whsec_AAAA AAAA", "id", 1, ""], ["whsec_AAAA\n", "id", 1, ""],
        ["whsec_-_-_", "id", 1, ""], ["WHSEC_AAAA", "id", 1, ""],
    ],
    "headers": [
        [SECRET, "evt_1", "{}", 1000],
        [SECRET, MSG_ID, BODY, TIMESTAMP],
        [OTHER, "", "", 0],
    ],
    "verify": [
        [SECRET, _received(EXPECTED), BODY, 300, TIMESTAMP],
        [SECRET, _received(EXPECTED), BODY, 300, TIMESTAMP + 300],
        [SECRET, _received(EXPECTED), BODY, 300, TIMESTAMP + 301],
        [SECRET, _received(EXPECTED), BODY, 300, TIMESTAMP - 301],
        [SECRET, _received(EXPECTED), BODY, 0, TIMESTAMP],
        [SECRET, _received(EXPECTED), BODY, 300, TIMESTAMP - 300],
        [SECRET, _received(EXPECTED), '{"test": 2432232315}', 300, TIMESTAMP],
        [SECRET, _received("v1,bogus " + EXPECTED), BODY, 300, TIMESTAMP],
        [SECRET, _received(EXPECTED + " v1,bogus"), BODY, 300, TIMESTAMP],
        [SECRET, _received("v2," + EXPECTED[3:]), BODY, 300, TIMESTAMP],
        [SECRET, _received("v1," + EXPECTED[3:] + "x"), BODY, 300, TIMESTAMP],
        [SECRET, _received(EXPECTED[3:]), BODY, 300, TIMESTAMP],
        [SECRET, _received("  " + EXPECTED + "\t"), BODY, 300, TIMESTAMP],
        [SECRET, _received(EXPECTED, msg_id=""), BODY, 300, TIMESTAMP],
        [SECRET, _received(EXPECTED, timestamp="abc"), BODY, 300, TIMESTAMP],
        [SECRET, _received(EXPECTED, timestamp=""), BODY, 300, TIMESTAMP],
        [SECRET, _received(EXPECTED, timestamp=" 1614265330 "), BODY, 300, TIMESTAMP],
        [SECRET, _received(EXPECTED, timestamp="+1614265330"), BODY, 300, TIMESTAMP],
        [SECRET, _received(EXPECTED, timestamp="1_614_265_330"), BODY, 300, TIMESTAMP],
        [SECRET, _received(EXPECTED, timestamp="1614265330.0"), BODY, 300, TIMESTAMP],
        [SECRET, {"Webhook-Id": MSG_ID, "Webhook-Timestamp": str(TIMESTAMP),
                  "Webhook-Signature": "v1,bogus " + EXPECTED}, BODY, 300, TIMESTAMP],
        [SECRET, {"WEBHOOK-ID": MSG_ID, "webhook-timestamp": str(TIMESTAMP),
                  "webhook-signature": EXPECTED, "webhook-id": "other"}, BODY, 300, TIMESTAMP],
        [SECRET, {}, BODY, 300, TIMESTAMP],
        [SECRET, {"webhook-id": MSG_ID, "webhook-timestamp": str(TIMESTAMP)}, BODY, 300,
         TIMESTAMP],
        [OTHER, _received(EXPECTED), BODY, 300, TIMESTAMP],
        ["", _received("v1,x", msg_id="id", timestamp="1"), "", 300, 1],
        ["MfKQ9r8", _received("v1,x", msg_id="id", timestamp="1"), "", 300, 1],
        ["whsec_", _received("v1,x", msg_id="id", timestamp="1"), "", 300, 1],
        ["whsec_!!!", _received("v1,x", msg_id="id", timestamp="1"), "", 300, 1],
        [SECRET, _received(EXPECTED, timestamp="99999999999999999999999"), BODY, 300,
         TIMESTAMP],
    ],
    "key_bytes": [
        [SECRET], ["whsec_AA=="], ["whsec_AAA="], ["whsec_AAAA"], ["whsec_"], ["whsec_!!!"],
        ["whsec_AA"], ["whsec_A==="], ["whsec_AA=A"], ["whsec_AAAA===="], ["whsec_////"],
        ["whsec_++++"], ["whsec_-_-_"], ["whsec_AB=="], ["whsec_AB"], [""], ["MfKQ9r8"],
    ],
}
