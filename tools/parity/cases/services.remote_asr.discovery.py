"""Curated inputs for services/remote_asr/discovery.py (M9.2 remote-wire): the UDP query and
answer datagrams, who gets an answer, and the addresses a search and a sweep reach.

Bytes travel as lowercase hex. parse_reply records dataclasses.asdict(host) plus `where` and
`compatible`. broadcast_targets and sweep_addresses read this computer's cards through
services.lan_address.local_ipv4_addresses, which the wrappers replace for the call with the
given [address, kind, prefix] entries (`private` stays the property LocalAddress computes).
"""

import dataclasses
import json

NONCE = "6e" * 16
OTHER = "6f" * 16
FP = "AB" * 32


def _make_query(module, nonce):
    return module.make_query(bytes.fromhex(nonce)).hex()


def _parse_query(module, data):
    nonce = module.parse_query(bytes.fromhex(data))
    return None if nonce is None else nonce.hex()


def _encode_reply(module, nonce, info):
    return module.encode_reply(bytes.fromhex(nonce), info).hex()


def _parse_reply(module, data, nonce, address):
    host = module.parse_reply(bytes.fromhex(data), bytes.fromhex(nonce), address)
    if host is None:
        return None
    out = dataclasses.asdict(host)
    out["where"] = host.where
    out["compatible"] = host.compatible
    return out


def _with_cards(cards, fn):
    import services.lan_address as lan_address

    original = lan_address.local_ipv4_addresses
    lan_address.local_ipv4_addresses = lambda: [
        lan_address.LocalAddress(address=address, kind=kind, prefix=prefix)
        for address, kind, prefix in cards
    ]
    try:
        return fn()
    finally:
        lan_address.local_ipv4_addresses = original


def _broadcast_targets(module, cards):
    return _with_cards(cards, module.broadcast_targets)


def _sweep_addresses(module, cards):
    return _with_cards(cards, module.sweep_addresses)


def _constants(module):
    names = ("DISCOVERY_PORT", "NONCE_BYTES", "QUERY_BYTES", "_DATAGRAM_MAX_BYTES", "SEARCH_S",
             "_SEND_AT", "_MAX_ANSWERS_PER_SENDER", "_MAX_ANSWERS", "_RATE_WINDOW_S",
             "SWEEP_CONNECT_S", "_SWEEP_WORKERS", "_SWEEP_NETWORKS", "_MAX_NAME")
    out = {name: getattr(module, name) for name in names}
    out["_SEND_AT"] = list(out["_SEND_AT"])
    out["QUERY_MAGIC"] = module.QUERY_MAGIC.hex()
    out["REPLY_MAGIC"] = module.REPLY_MAGIC.hex()
    return out


def _reply(text):
    """A datagram that starts with the reply magic and holds `text` (str or bytes)."""
    body = text.encode("utf-8") if isinstance(text, str) else text
    return (b"OWHISPER!1" + body).hex()


def _answer(**fields):
    reply = {"n": NONCE, "name": "devbox", "port": 47821, "fp": FP, "protocol": 1,
             "approval": True, "engine": {"label": "Parakeet", "device": "cuda",
                                          "available": True, "family": "parakeet"}}
    reply.update(fields)
    return _reply(json.dumps(reply, separators=(",", ":")))


_ENGINE = {"label": "Parakeet TDT 0.6B v3", "device": "cuda", "available": True,
           "family": "parakeet", "model": "parakeet-v3", "status": "ignored"}

_INFO = {"name": "devbox", "port": 47821, "fingerprint": FP, "approval": True, "engine": _ENGINE}


def _info(**fields):
    info = dict(_INFO)
    info.update(fields)
    return info


CASES = {
    "constants": [[]],
    "make_query": [[NONCE], [""], ["00" * 16], ["ff" * 20]],
    "parse_query": [
        ["4f57484953504552" + "3f31" + NONCE + "00" * 486],
        ["4f574849535045523f31" + NONCE + "00" * 486],
        ["4f574849535045523f31" + NONCE + "00" * 485],
        ["4f574849535045523f31" + NONCE + "00" * 487],
        ["4f574849535045523f31" + "00" * 502],
        ["4f574849535045523f31" + "12" * 4 + "00" * 498],
        ["78" * 512],
        ["4f574849535045523f31" + NONCE],
        [""],
        ["4f574849535045523f32" + NONCE + "00" * 486],
    ],
    "encode_reply": [
        [NONCE, _INFO],
        [NONCE, _info(name="x" * 200)],
        [NONCE, _info(engine=dict(_ENGINE, label="L" * 300))],
        [NONCE, _info(name="\u6f22" * 60, engine=dict(_ENGINE, label="L" * 300))],
        [NONCE, _info(name="\u6f22" * 60, fingerprint="F" * 600)],
        [NONCE, _info(name="\u00e9" * 70, engine=dict(_ENGINE, label="\u00e9" * 60))],
        [NONCE, _info(port="47821")],
        [NONCE, _info(port=None)],
        [NONCE, _info(port=True)],
        [NONCE, _info(port=3.9)],
        [NONCE, _info(port="abc")],
        [NONCE, _info(port=[1])],
        [NONCE, _info(engine=["not", "a", "dict"])],
        [NONCE, _info(engine={})],
        [NONCE, _info(approval="yes")],
        [NONCE, _info(approval=1)],
        [NONCE, _info(name=42)],
        [NONCE, _info(name=None, fingerprint=None)],
        [NONCE, {}],
        ["", _INFO],
    ],
    "parse_reply": [
        [_answer(), NONCE, "192.168.1.20"],
        [_answer(), OTHER, "192.168.1.20"],
        [_answer(fp="not hex"), NONCE, "x"],
        [_answer(fp="ab" * 32), NONCE, "x"],
        [_answer(fp="AB" * 31 + "AG"), NONCE, "x"],
        [_answer(fp="AB" * 33), NONCE, "x"],
        [_answer(fp=12), NONCE, "x"],
        [_answer(port=0), NONCE, "x"],
        [_answer(port=65536), NONCE, "x"],
        [_answer(port=65535), NONCE, "x"],
        [_answer(port=True), NONCE, "x"],
        [_answer(port=47821.0), NONCE, "x"],
        [_answer(port="47821"), NONCE, "x"],
        [_answer(protocol="2"), NONCE, "x"],
        [_answer(protocol=None), NONCE, "x"],
        [_answer(protocol="x"), NONCE, "x"],
        [_answer(protocol=1.9), NONCE, "x"],
        [_answer(protocol=True), NONCE, "x"],
        [_reply('{"n":"%s","fp":"%s","port":1,"protocol":NaN}' % (NONCE, FP)), NONCE, "x"],
        [_reply('{"n":"%s","fp":"%s","port":1,"protocol":Infinity}' % (NONCE, FP)), NONCE, "x"],
        [_answer(engine=["x"]), NONCE, "x"],
        [_answer(engine={"available": False, "extra": [1, 2]}), NONCE, "x"],
        [_answer(name=""), NONCE, "10.0.0.7"],
        [_answer(name=None), NONCE, "10.0.0.7"],
        [_answer(name="  dev\u0000box\u200b  "), NONCE, "x"],
        [_answer(name="\u00e9" * 70), NONCE, "x"],
        [_answer(name=7), NONCE, "x"],
        [_answer(approval="true"), NONCE, "x"],
        [_answer(approval=False), NONCE, "x"],
        [_reply(b'{"n":"\xff"}'), NONCE, "x"],
        [_reply("[1, 2]"), NONCE, "x"],
        [_reply("not json"), NONCE, "x"],
        [_reply(""), NONCE, "x"],
        ["67617262616765", NONCE, "x"],
        [_reply('{"n":"%s","fp":"%s","port":47821}' % (NONCE, FP)), NONCE, "fe80::1"],
        [_reply(' {"n":"%s","fp":"%s","port":2} ' % (NONCE, FP)), NONCE, "x"],
    ],
    "allowed_sender": [
        [address] for address in [
            "192.168.1.20", "10.0.0.5", "172.16.0.1", "127.0.0.1", "169.254.1.1", "100.64.0.5",
            "8.8.8.8", "224.0.0.1", "nonsense", "fe80::1", "fd00::1", "::1", "2001:db8::1",
            "2607:f8b0::1", "::ffff:192.168.1.1", "::ffff:8.8.8.8", "::ffff:224.0.0.1",
            "::ffff:0.0.0.0", "0.0.0.0", "::", "ff02::1", "100.127.255.255", "100.128.0.1",
            "100.63.255.255", "::ffff:100.64.0.5", "fe80::1%eth0", "", "1.2.3",
            "255.255.255.255", "239.255.255.250", "198.18.0.1", "192.0.0.8", "fc00::1",
            "192.168.1.20 ", "::ffff:127.0.0.1", "64:ff9b::1", "fec0::1",
        ]
    ],
    "broadcast_targets": [
        [[]],
        [[["192.168.1.20", "lan", 24]]],
        [[["10.1.2.3", "lan", 8], ["192.168.1.20", "lan", 24], ["192.168.1.21", "lan", 24]]],
        [[["172.16.5.4", "lan", 31], ["172.16.5.4", "lan", 30], ["172.16.5.4", "lan", 7]]],
        [[["192.168.1.20", "vpn", 24], ["192.168.2.20", "virtual", 24]]],
        [[["8.8.8.8", "lan", 24], ["100.64.0.2", "lan", 10]]],
        [[["192.168.1.20", "lan", 0]]],
        [[["10.0.0.5", "lan", 16], ["192.168.1.255", "lan", 24]]],
    ],
    "sweep_addresses": [
        [[]],
        [[["192.168.1.20", "lan", 24]]],
        [[["10.0.0.5", "lan", 16]]],
        [[["192.168.1.20", "lan", 0]]],
        [[["192.168.1.20", "lan", 30]]],
        [[["192.168.1.20", "lan", 31]]],
        [[["192.168.1.20", "lan", 32]]],
        [[["192.168.1.20", "lan", 24], ["192.168.1.21", "lan", 24], ["10.0.0.5", "lan", 28]]],
        [[["10.0.0.5", "lan", 28], ["172.16.0.9", "lan", 29], ["192.168.7.1", "lan", 30]]],
        [[["8.8.8.8", "lan", 24], ["192.168.1.20", "vpn", 30], ["10.9.9.9", "lan", 30]]],
    ],
}

WRAPPERS = {
    "constants": _constants,
    "make_query": _make_query,
    "parse_query": _parse_query,
    "encode_reply": _encode_reply,
    "parse_reply": _parse_reply,
    "broadcast_targets": _broadcast_targets,
    "sweep_addresses": _sweep_addresses,
}
