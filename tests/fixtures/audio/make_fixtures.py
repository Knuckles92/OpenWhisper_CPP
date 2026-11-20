"""Writes the audio fixtures of tests/fixtures/audio with PyAV, the library OpenWhisper decodes
with, so FFmpeg's own encoders make every container the decoder tests read.

    & $env:FIORIVOCE_OW_PYTHON -I -B tests/fixtures/audio/make_fixtures.py

Needs only numpy and av from OpenWhisper's interpreter; imports no OpenWhisper module and writes
nothing outside this folder. The signal is deterministic (a seeded generator), so a rerun with the
same encoder versions gives the same samples; the committed files are the reference either way,
because tools/parity/cases/av.resample16k.py decodes the committed bytes.
"""
from __future__ import annotations

import wave
from pathlib import Path

import av
import numpy as np

HERE = Path(__file__).resolve().parent
SECONDS = 0.8


def signal(rate: int, channels: int, seed: int) -> np.ndarray:
    """(frames, channels) float64 in [-1, 1]: two tones, noise, a fade-in and a quiet gap."""
    frames = int(rate * SECONDS)
    t = np.arange(frames) / rate
    rng = np.random.RandomState(seed)
    columns = []
    for channel in range(channels):
        tone = (0.30 * np.sin(2 * np.pi * 220.0 * t + channel * 0.7)
                + 0.20 * np.sin(2 * np.pi * 1375.5 * t)
                + 0.08 * np.sin(2 * np.pi * 7300.0 * t)
                + 0.04 * rng.standard_normal(frames))
        envelope = np.minimum(1.0, t / 0.05)
        envelope[(t > 0.40) & (t < 0.48)] = 0.02
        columns.append(tone * envelope)
    return np.stack(columns, axis=1)


def to_int16(samples: np.ndarray) -> np.ndarray:
    return np.clip(np.round(samples * 32767.0), -32768, 32767).astype("<i2")


def write_wav(name: str, rate: int, channels: int, seed: int) -> None:
    pcm = to_int16(signal(rate, channels, seed))
    with wave.open(str(HERE / name), "wb") as handle:
        handle.setnchannels(channels)
        handle.setsampwidth(2)
        handle.setframerate(rate)
        handle.writeframes(pcm.tobytes())


def write_encoded(name: str, container: str, codec: str, rate: int, channels: int, seed: int,
                  options: dict | None = None) -> None:
    layout = "mono" if channels == 1 else "stereo"
    pcm = to_int16(signal(rate, channels, seed))
    with av.open(str(HERE / name), "w", format=container) as output:
        stream = output.add_stream(codec, rate=rate, layout=layout)
        if options:
            stream.codec_context.options = options
        stream.codec_context.bit_rate = 128000 if codec != "flac" else 0
        # Python's wave and PyAV agree on interleaved s16; the encoder takes its own format in
        # fixed-size frames.
        frame = av.AudioFrame.from_ndarray(pcm.reshape(1, -1), format="s16", layout=layout)
        frame.sample_rate = rate
        convert = av.AudioResampler(format=stream.codec_context.format, layout=layout, rate=rate,
                                    frame_size=stream.codec_context.frame_size or None)
        for converted in convert.resample(frame) + convert.resample(None):
            for packet in stream.encode(converted):
                output.mux(packet)
        for packet in stream.encode(None):
            output.mux(packet)


def main() -> None:
    write_wav("tone-44k1-mono.wav", 44100, 1, 1)
    write_wav("tone-48k-stereo.wav", 48000, 2, 2)
    write_wav("tone-16k-mono.wav", 16000, 1, 3)
    write_encoded("tone-44k1-stereo.mp3", "mp3", "libmp3lame", 44100, 2, 4)
    write_encoded("tone-44k1-stereo.m4a", "ipod", "aac", 44100, 2, 5)
    write_encoded("tone-44k1-stereo.ogg", "ogg", "vorbis", 44100, 2, 6,
                  {"strict": "experimental"})
    write_encoded("tone-48k-mono.flac", "flac", "flac", 48000, 1, 7)
    write_encoded("tone-44k1-stereo.wma", "asf", "wmav2", 44100, 2, 8)


if __name__ == "__main__":
    main()
