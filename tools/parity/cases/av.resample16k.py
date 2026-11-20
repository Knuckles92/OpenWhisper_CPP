"""PyAV's 16 kHz decode path (services/local_asr/audio.py) over the fixtures of tests/fixtures/audio.

"av.resample16k" is no OpenWhisper module: it names what local_asr/audio.py does with PyAV
(_open with the mono PCM probesize, container.decode(audio=0), frame.pts = None,
AudioResampler(format="s16", layout="mono", rate=16000), flushed with resample(None)). This
file registers it so golden_vectors.py can import it; every step calls OpenWhisper's own
_open and resampler(). The C++ replay is AudioFileDecoder + Resampler16k.

The PCM goes into the vector as base64 of little-endian int16 so the file stays small.
"""
from __future__ import annotations

import base64
import sys
import types
from pathlib import Path

REPO = Path(__file__).resolve().parents[3]
FIXTURES = REPO / "tests" / "fixtures" / "audio"

FILES = [
    "tone-44k1-mono.wav",
    "tone-48k-stereo.wav",
    "tone-16k-mono.wav",
    "tone-44k1-stereo.mp3",
    "tone-44k1-stereo.m4a",
    "tone-44k1-stereo.ogg",
    "tone-48k-mono.flac",
    "tone-44k1-stereo.wma",
]


def resample16k(name: str) -> dict:
    """The 16 kHz mono s16 stream windows() cuts, for the fixture `name`."""
    import numpy as np

    from services.local_asr import audio

    path = str(FIXTURES / name)
    pieces = []
    decoded = 0
    convert = audio.resampler()
    with audio._open(path) as container:
        stream = container.streams.audio[0]
        rate, channels = stream.codec_context.sample_rate, stream.codec_context.channels
        for frame in container.decode(audio=0):
            decoded += frame.samples
            frame.pts = None
            pieces.extend(f.to_ndarray().reshape(-1) for f in convert.resample(frame))
        pieces.extend(f.to_ndarray().reshape(-1) for f in convert.resample(None))
    pcm = np.concatenate(pieces).astype("<i2")
    return {
        "rate": rate,
        "channels": channels,
        "decoded_frames": decoded,
        "samples": int(len(pcm)),
        "pcm_s16le_base64": base64.b64encode(pcm.tobytes()).decode("ascii"),
    }


_module = types.ModuleType("av.resample16k")
_module.resample16k = resample16k
sys.modules["av.resample16k"] = _module

CASES = {"resample16k": [[name] for name in FILES]}

import av as _av  # noqa: E402

PINS = {
    "fixtures": "tests/fixtures/audio, made by tests/fixtures/audio/make_fixtures.py",
    "pyav": _av.__version__,
    "ffmpeg": {name: ".".join(str(part) for part in version)
               for name, version in sorted(_av.library_versions.items())
               if name in ("libavcodec", "libavformat", "libavutil", "libswresample")},
    # Measured against FFmpeg 9.0.2 (vcpkg; libavcodec 63.1), which OpenWhisper_CPP links. Every other
    # fixture matches exactly. Keys: decoded_frames and length (how far the native frame count
    # and the 16 kHz length may be apart), differing_samples and max_delta (within the common
    # length), differing_tail (differences only within that many samples of the common end).
    "tolerance": {
        # One sample, in the middle, one step apart: float-to-int16 rounding of the mp3float
        # output differs between the two FFmpeg builds.
        "tone-44k1-stereo.mp3": {"differing_samples": 1, "max_delta": 1,
                                 "differing_tail": 12800},
        # FFmpeg 9's mov demuxer trims the AAC end padding to the stream's stated duration
        # (36304 frames, which both builds report); FFmpeg 8 decodes all 36864. The resampler's
        # flush then sees a different tail: the last 14 samples before the cut differ.
        "tone-44k1-stereo.m4a": {"decoded_frames": 560, "length": 203, "differing_samples": 14,
                                 "max_delta": 452, "differing_tail": 15},
    },
    # Added to "tolerance" off Windows. The same FFmpeg built for Linux (GCC, static) rounds one
    # sample of the wmav2 float output one step differently from the Windows build (measured
    # with GCC 16: 1 of 12632). The count depends on the compiler's floating-point code, so a
    # few are allowed; the step is not.
    "tolerance_non_windows": {
        "tone-44k1-stereo.wma": {"differing_samples": 4, "max_delta": 1,
                                 "differing_tail": 12800},
    },
}
