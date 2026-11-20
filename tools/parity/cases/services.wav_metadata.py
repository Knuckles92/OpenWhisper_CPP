"""Curated inputs for services/wav_metadata.py (fv_audio's wav_metadata.h).

append_origination writes into an open file, so the wrapper builds the WAV Python's wave module
writes for the given PCM, tags it, and returns the whole file as hex. ORIGINATOR and SOFTWARE are
set to FioriVoce's product name while it runs (identity().product), which changes the LIST chunk
size; a byte-for-byte rebrand of OpenWhisper's output could not.

is_mono_pcm16 and the read_* helpers take a path, so their wrappers take the file's bytes as hex
and write them to a scratch file first.
"""
from __future__ import annotations

import os
import struct
import tempfile
import wave
from datetime import datetime
from pathlib import Path

REPO = Path(__file__).resolve().parents[3]
SCRATCH = REPO / "out" / "parity" / "wav_metadata"
PRODUCT = "FioriVoce"


def _scratch_file(data: bytes) -> str:
    SCRATCH.mkdir(parents=True, exist_ok=True)
    fd, path = tempfile.mkstemp(suffix=".wav", dir=SCRATCH)
    with os.fdopen(fd, "wb") as handle:
        handle.write(data)
    return path


def _append_origination(module, pcm_hex: str, width: int, when: str) -> str:
    frames = bytes.fromhex(pcm_hex)
    SCRATCH.mkdir(parents=True, exist_ok=True)
    fd, path = tempfile.mkstemp(suffix=".wav", dir=SCRATCH)
    saved = module.ORIGINATOR, module.SOFTWARE
    module.ORIGINATOR = module.SOFTWARE = PRODUCT
    try:
        with os.fdopen(fd, "wb") as handle:
            with wave.open(handle, "wb") as wav:
                wav.setnchannels(1)
                wav.setsampwidth(width)
                wav.setframerate(16000)
                wav.writeframesraw(frames)
            module.append_origination(handle, datetime.fromisoformat(when))
        return Path(path).read_bytes().hex()
    finally:
        module.ORIGINATOR, module.SOFTWARE = saved
        os.remove(path)


def _on_file(function_name: str):
    def wrapper(module, data_hex: str):
        path = _scratch_file(bytes.fromhex(data_hex))
        try:
            return getattr(module, function_name)(path)
        finally:
            os.remove(path)
    return wrapper


def _riff(format_fields: bytes, before_format: bytes = b"", data: bytes = b"\x00\x00" * 8) -> bytes:
    body = (b"WAVE" + before_format + b"fmt " + struct.pack("<I", len(format_fields))
            + format_fields + b"data" + struct.pack("<I", len(data)) + data)
    return b"RIFF" + struct.pack("<I", len(body)) + body


def _format(tag: int, channels: int, bits: int) -> bytes:
    block = channels * bits // 8
    return struct.pack("<HHIIHH", tag, channels, 44100, 44100 * block, block, bits)


def _tagged(pcm: bytes, when: str) -> str:
    import services.wav_metadata as module

    return _append_origination(module, pcm.hex(), 2, when)


WRAPPERS = {
    "append_origination": _append_origination,
    "is_mono_pcm16": _on_file("is_mono_pcm16"),
    "read_info_icrd": _on_file("read_info_icrd"),
    "read_info_isft": _on_file("read_info_isft"),
    "read_bext_origination": _on_file("read_bext_origination"),
}

_SHORT = bytes(range(64))
_ODD_8BIT = b"\x80\x81\x7f"

CASES = {
    "append_origination": [
        [_SHORT.hex(), 2, "2026-09-19T13:05:07"],
        [_ODD_8BIT.hex(), 1, "2026-09-19T13:05:07"],
        [b"".hex(), 2, "1999-12-31T23:59:59"],
        [(b"\x01\x02" * 1000).hex(), 2, "2026-01-02T03:04:05"],
    ],
    "is_mono_pcm16": [
        [_riff(_format(1, 1, 16)).hex()],
        [_riff(_format(1, 1, 16), before_format=b"JUNK" + struct.pack("<I", 3) + b"abc\x00").hex()],
        [_riff(_format(1, 2, 16)).hex()],
        [_riff(_format(1, 1, 8)).hex()],
        [_riff(_format(1, 1, 24)).hex()],
        [_riff(_format(3, 1, 32)).hex()],
        [_riff(_format(0xFFFE, 1, 16) + bytes(24)).hex()],
        [_riff(_format(0x55, 1, 16)).hex()],
        [_riff(_format(1, 1, 16))[:30].hex()],
        [(b"RIFF" + struct.pack("<I", 4) + b"WAVE").hex()],
        [(b"OggS" + bytes(60)).hex()],
        [b"".hex()],
        # data before fmt: not one.
        [(b"RIFF" + struct.pack("<I", 20) + b"WAVEdata" + struct.pack("<I", 0)).hex()],
    ],
}


# The read_* helpers over tagged files (wav_metadata imports nothing that resolves data paths, so
# importing it while the cases load is safe), an untagged one and a file that is no WAV.
_TAGGED = [_tagged(_SHORT, "2026-09-19T13:05:07"), _tagged(b"\x00\x00" * 3, "2001-02-03T04:05:06")]
for _name in ("read_info_icrd", "read_info_isft", "read_bext_origination"):
    CASES[_name] = [[_TAGGED[0]], [_TAGGED[1]], [_riff(_format(1, 1, 16)).hex()],
                    [(b"OggS" + bytes(60)).hex()]]

PINS = {"originator": PRODUCT, "software": PRODUCT}
