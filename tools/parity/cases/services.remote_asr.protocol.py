"""Curated inputs for services/remote_asr/protocol.py (M9.2 remote-wire): the remote engine's
frames, PCM16 audio, fingerprints, pairing numbers and addresses.

Bytes travel as lowercase hex, since the vector format holds JSON only. Functions whose refusals
carry user-facing text go through _catching, which records {"result": ...} or {"error": <Python
class>, "message": str(exc)}, so the replay compares the message as well as the class. Frames
near the 64 KiB header limit are built by the *_sized wrappers from a few numbers and recorded
as their length and SHA-256, so the vector file stays small. The file is ASCII: every other
character is written as an escape.
"""

import hashlib
import random

_RNG = random.Random(9021)
_RANDOM_SAMPLES = [round(_RNG.uniform(-1.0, 1.0), 9) for _ in range(48)]


def _catching(fn):
    def run(*args):
        try:
            return {"result": fn(*args)}
        except Exception as exc:  # the class and the text are both the contract
            return {"error": type(exc).__name__, "message": str(exc)}
    return run


def _encode_audio(module, samples):
    return module.encode_audio(samples).hex()


def _decode_audio(module, data):
    return _catching(lambda: module.decode_audio(bytes.fromhex(data)).tolist())()


def _pack_request(module, header, audio=None, payload=None):
    def run():
        if payload is not None:
            return module.pack_request(header, payload=bytes.fromhex(payload)).hex()
        return module.pack_request(header, audio).hex()
    return _catching(run)()


def _pack_request_sized(module, char, count):
    """pack_request({"phrases": [char * count]}): the frame's length and SHA-256, or the refusal."""
    def run():
        frame = module.pack_request({"phrases": [char * count]})
        return [len(frame), hashlib.sha256(frame).hexdigest()]
    return _catching(run)()


def _unpack_frame(module, data, binary=True):
    def run():
        frame = bytes.fromhex(data) if binary else data
        header, payload = module.unpack_frame(frame)
        return [header, payload.hex()]
    return _catching(run)()


def _unpack_frame_sized(module, length, spaces, tail):
    """unpack_frame of a length field `length`, then "{", `spaces` spaces, "}", then `tail`
    zero bytes: the header and the payload's length, or the refusal."""
    def run():
        body = b"{" + b" " * spaces + b"}"
        header, payload = module.unpack_frame(length.to_bytes(4, "big") + body + b"\0" * tail)
        return [header, len(payload)]
    return _catching(run)()


def _payload_audio(module, data):
    return _catching(lambda: module.payload_audio(bytes.fromhex(data)).tolist())()


def _payload_audio_size(module, size):
    """payload_audio of `size` zero bytes: how many samples come back, or the refusal."""
    return _catching(lambda: len(module.payload_audio(b"\0" * size)))()


def _certificate_fingerprint(module, der):
    return module.certificate_fingerprint(bytes.fromhex(der))


def _pairing_commitment(module, nonce):
    return module.pairing_commitment(bytes.fromhex(nonce))


def _pairing_sas(module, host_nonce, client_nonce, fingerprint):
    return module.pairing_sas(bytes.fromhex(host_nonce), bytes.fromhex(client_nonce), fingerprint)


def _parse_address(module, text, *default_port):
    return _catching(lambda: list(module.parse_address(text, *default_port)))()


def _constants(module):
    names = ("PROTOCOL_VERSION", "DEFAULT_PORT", "PATH", "SAMPLE_RATE", "MAX_AUDIO_SECONDS",
             "MAX_HEADER_BYTES", "MAX_REQUEST_BYTES", "MAX_REPLY_BYTES", "RECORD_CHUNK_BYTES",
             "CLOSE_UNAUTHORIZED", "CLOSE_BAD_REQUEST", "CLOSE_TIMEOUT", "CLOSE_ENGINE_CHANGED",
             "ENGINE_RELOADING", "CLOSE_BUSY", "MAX_NAME", "MAX_HINT_PHRASES", "MAX_HINT_CHARS",
             "PAIRING_NONCE_BYTES", "SAS_DIGITS")
    return {name: getattr(module, name) for name in names}


_HALF_TIES = [0.5 / 32768, 1.5 / 32768, 2.5 / 32768, -0.5 / 32768, -1.5 / 32768, -2.5 / 32768,
              32766.5 / 32768, -32767.5 / 32768, 32767.5 / 32768]


def _frame(header_text, payload=b""):
    body = header_text.encode("utf-8") if isinstance(header_text, str) else header_text
    return (len(body).to_bytes(4, "big") + body + payload).hex()


CASES = {
    "constants": [[]],
    "encode_audio": [
        [[]],
        [[0.0, 0.0, 0.0, -0.0]],
        [[1.0, -1.0, 1.5, -1.5, 0.99999994, -0.99999994, 2.0, -2.0]],
        [_HALF_TIES],
        [[1e-10, -1e-10, 3.0517578125e-05, -3.0517578125e-05, 0.1, -0.1, 0.25, 0.75]],
        [[float("inf"), float("-inf"), 1e30, -1e30]],
        [_RANDOM_SAMPLES],
    ],
    "decode_audio": [
        [""], ["0000"], ["0080"], ["ff7f"], ["0100feff3930"], ["00"], ["000000"],
    ],
    "pack_request": [
        [{"id": 7, "op": "transcribe", "language": "en"}, [-1.0, -3.0517578125e-05, 0.0,
                                                          3.0517578125e-05, 0.376739501953125,
                                                          0.999969482421875]],
        [{"id": 1, "op": "describe"}],
        [{}],
        [{"z": 1, "a": 2, "m": [3, 2, 1]}],
        [{"device_name": "Zo\u00eb's \U0001F4BB laptop", "op": "x",
          "text": "\u6f22\u5b57 e\u0301 \u00e9"}],
        [{"a": {"b": [1, 2.5, None, True, False, 0.1, 1e16, 1e-05, -0.0], "c": {}}, "d": []}],
        [{"s": "quote\" back\\ slash/ tab\t nl\n cr\r ctl\u0001 del\u007f nul\u0000"}],
        [{"big": 12345678901234567890, "neg": -9007199254740993, "f": 123456789.125}],
        [{"op": "records_put", "record_id": "r1", "name": "audio.wav"}, None, "010203"],
        [{"op": "records_put"}, None, ""],
        [{"op": "transcribe"}, [0.0, 0.5, -0.5]],
    ],
    "pack_request_sized": [
        ["x", 65519], ["x", 65520], ["x", 65521], ["\u00e9", 10920], ["\u00e9", 10921],
        ["\U0001F4BB", 5460], ["\U0001F4BB", 5461],
    ],
    "unpack_frame": [
        [_frame('{"id":7,"op":"transcribe","language":"en"}', b"\x01\x00\xff\x7f")],
        [_frame("{}")],
        [_frame('{"z":1,"a":{"b":[1,2.5,null,true]}}')],
        [_frame('{"a":"\\u00e9\\ud83d\\udcbb"}')],
        [_frame('{"a":"\u00e9"}', b"\x00")],
        [_frame(' {"a" : 1} ')],
        [_frame('{"a":NaN,"b":Infinity,"c":-Infinity}')],
        [_frame('{"a":1,"a":2}')],
        [""], ["00"], ["000000"], ["00000000"],
        ["000000096e6f7065"],
        [_frame("nope")],
        [_frame("[1]")],
        [_frame("null")],
        [_frame("NaN")],
        [_frame('"text"')],
        [_frame('{"a":1')],
        [_frame(b'{"a":"\xff"}')],
        [_frame(b'{"a":"\xc0\xaf"}')],
        [_frame(b'\xef\xbb\xbf{"bom":1}')],
        [_frame('{"utf16":1}'.encode("utf-16-le"))],
        [_frame('{"utf16":1}'.encode("utf-16-be"))],
        [_frame('{"utf16":1}'.encode("utf-16"))],
        [_frame('{"utf32":1}'.encode("utf-32-le"))],
        [_frame('{"utf32":1}'.encode("utf-32-be"))],
        [_frame('{"utf32":1}'.encode("utf-32"))],
        [_frame('{"t":"\u00e9\U0001F4BB"}'.encode("utf-16-le"))],
        [_frame(b"\x7b\x00")],
        [_frame(b"\x00\x7b")],
        [_frame(b'{"a":"\x00"}')],
        [_frame(b'{"a":1}\x00')],
        ["text frame", False],
        ["", False],
    ],
    "unpack_frame_sized": [
        [65536, 65534, 3], [65536, 65534, 0], [65537, 65535, 0], [65537, 65534, 1],
        [4294967295, 2, 0], [100, 2, 50],
    ],
    "payload_audio": [
        [""], ["0000"], ["00800180"], ["00"], ["010203"],
    ],
    "payload_audio_size": [
        [0], [2], [3], [3_840_000], [3_840_001], [3_840_002], [3_839_999],
    ],
    "header_phrases": [
        [{"phrases": ["Ksenia", "  Kuber   netes "]}],
        [{"phrases": None}], [{"phrases": "Ksenia"}], [{"phrases": {"a": 1}}], [{"phrases": 5}],
        [{"phrases": []}], [{"phrases": [None, 3, "", "  ", True, ["x"], {"y": 1}]}], [{}],
        [{"phrases": ["x" * 64, "y" * 65, "\u00e9" * 64, "\u00e9" * 65, "\U0001F4BB" * 64]}],
        [{"phrases": ["x" * 65] + [f"w{i}" for i in range(60)]}],
        [{"phrases": [f"w{i}" for i in range(50)] + ["extra"]}],
        [{"phrases": ["a\tb\nc\u00a0d\u2003e\u3000f", "\u200b", " \x1c\x1d\x1e\x1f "]}],
        [{"phrases": ["  " + "x" * 64 + "  ", "a" + " " * 100 + "b"]}],
    ],
    "certificate_fingerprint": [
        [""], ["00"], [bytes(range(256)).hex() * 2],
    ],
    "short_fingerprint": [
        ["ab" * 32], ["AB" * 32], ["ab-cd:ef 01"], [""], ["short"], ["abcd"], ["abcde"],
        ["\u00e4" * 30], ["\u00df" * 15], ["12 34:56/78\u0663\u0664"], ["ab" * 32, 0],
        ["ab" * 32, 1], ["ab" * 32, 2], ["ab" * 32, -1], ["ab" * 32, 20], ["a-b-c", 5],
    ],
    "token_digest": [
        [""], ["abc"], ["tok\u00e9\U0001D11E"], ["x" * 43],
    ],
    "clean_name": [
        [None], [""], ["  devbox  "], ["dev\x00box\x1b"], ["Zo\u00eb \U0001F4BB laptop"],
        ["a" * 70], ["\u00e9" * 61], ["\u200b x \u2028"], [42], [0], [True], [False],
        [3.5], ["\t\n"], ["x\u00a0y"], ["\u00a0x\u00a0"], ["  \u3000name\u3000 "],
        ["line\nbreak"], ["\U0001F4BB" * 61],
    ],
    "pairing_commitment": [
        ["68" * 32], [""], ["00" * 32], ["78" * 32],
    ],
    "pairing_sas": [
        ["68" * 32, "63" * 32, "AB" * 32],
        ["68" * 32, "63" * 32, "ab" * 32],
        ["68" * 32, "63" * 32, "CD" * 32],
        ["68" * 32, "64" * 32, "AB" * 32],
        ["68" * 32, "63" * 32, ""],
        ["68" * 32, "63" * 32, "ab\u00e9" + "AB" * 30],
        ["68" * 32, "63" * 32, "\u00df"],
        ["68" * 32, "63" * 32, "\u0131"],
        ["68" * 32, "63" * 32, "\U0001F4BB"],
        ["", "", ""],
        ["00" * 32, "ff" * 32, "AB" * 32],
    ],
    "format_sas": [
        ["123456"], ["12345"], ["1234567"], [""], ["ab cde"],
        ["\uff11\uff12\uff13\uff14\uff15\uff16"], ["000000"],
    ],
    "parse_address": [
        ["devbox"], ["192.168.1.20:5000"], ["[fe80::1]:6000"], ["fe80::1"],
        ["https://devbox.local:7000/whatever"], [""], ["   "], ["host:notaport"], ["host:0"],
        ["[fe80::1"], ["[::1]:5"], ["wss://host/path"], ["WSS://Host:80"],
        ["ws://[fe80::1%3]:9/x"], ["host:"], [":80"], ["[::1]x"], ["[::1]:"], ["[]:80"],
        ["[ ]"], ["host :80"], ["a b"], ["host:65535"], ["host:65536"], ["host:-1"],
        ["host:+5"], ["host: 7 "], ["host:1_000"], ["host:\u0663"], ["  https://x  "],
        ["http://"], ["devbox/path"], ["/path"], ["HTTP://Mixed.Case:1/p"], ["ftp://x:1"],
        ["host:99999999999999999999999"], ["\u00e9t\u00e9.local:8"], ["a:b:c"],
        ["[fe80::1]:6000/p"], ["devbox", 8000], ["devbox:9", 8000], ["\thost\t"],
        ["http:// spaced"], ["host\u00a0x"], ["\u0130://x"], ["host:\u00b2"],
        ["host:1" + "0" * 5000],
    ],
    "format_address": [
        ["devbox", 47821], ["fe80::1", 1], ["::1", 47821], ["", 5], ["192.168.1.20", 65535],
    ],
}

WRAPPERS = {
    "constants": _constants,
    "encode_audio": _encode_audio,
    "decode_audio": _decode_audio,
    "pack_request": _pack_request,
    "pack_request_sized": _pack_request_sized,
    "unpack_frame": _unpack_frame,
    "unpack_frame_sized": _unpack_frame_sized,
    "payload_audio": _payload_audio,
    "payload_audio_size": _payload_audio_size,
    "certificate_fingerprint": _certificate_fingerprint,
    "pairing_commitment": _pairing_commitment,
    "pairing_sas": _pairing_sas,
    "parse_address": _parse_address,
}
