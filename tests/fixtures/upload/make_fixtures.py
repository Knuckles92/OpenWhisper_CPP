"""Writes the small fixtures of tests/fixtures/upload, which test the file preview's edges.

    python -I -B tests/fixtures/upload/make_fixtures.py

Standard library only; writes nothing outside this folder. The stereo WAV and the 25 MB WAV
are written by the tests at run time, as OpenWhisper's tests write theirs.

- header-only.wav: a mono 16-bit 44.1 kHz WAV header announcing no data and holding none, so
  FFmpeg finds an audio stream without a duration and decoding it yields no samples.
- no-duration.flac: ../audio/tone-48k-mono.flac with STREAMINFO's total-samples field zeroed
  ("unknown" in the FLAC format), so the header has no duration and the 0.8 s are only found by
  decoding.
- no-audio.srt: a subtitle file, a container FFmpeg opens that has no audio stream.
- empty.wav: no bytes at all.
"""
from __future__ import annotations

import struct
from pathlib import Path

HERE = Path(__file__).resolve().parent


def header_only_wav() -> bytes:
    rate, channels, bits = 44100, 1, 16
    block = channels * bits // 8
    fmt = struct.pack("<HHIIHH", 1, channels, rate, rate * block, block, bits)
    return (b"RIFF" + struct.pack("<I", 36) + b"WAVE" + b"fmt " + struct.pack("<I", 16) + fmt
            + b"data" + struct.pack("<I", 0))


def no_duration_flac() -> bytes:
    data = bytearray((HERE.parent / "audio" / "tone-48k-mono.flac").read_bytes())
    assert data[:4] == b"fLaC" and data[4] & 0x7F == 0, "STREAMINFO must be the first block"
    # STREAMINFO starts at byte 8; its 36-bit total-samples field is the low nibble of byte 21
    # and bytes 22 to 25.
    data[21] &= 0xF0
    data[22:26] = b"\0\0\0\0"
    return bytes(data)


def no_audio_srt() -> bytes:
    # LF line ends: the repository stores text with LF and would rewrite CRLF.
    return b"1\n00:00:00,000 --> 00:00:01,000\nNo audio here.\n"


def main() -> None:
    (HERE / "header-only.wav").write_bytes(header_only_wav())
    (HERE / "no-duration.flac").write_bytes(no_duration_flac())
    (HERE / "no-audio.srt").write_bytes(no_audio_srt())
    (HERE / "empty.wav").write_bytes(b"")


if __name__ == "__main__":
    main()
