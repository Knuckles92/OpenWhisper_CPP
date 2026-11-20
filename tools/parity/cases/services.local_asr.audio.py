"""Curated inputs for services/local_asr/audio.py (fv_dsp split_point and WindowSplitter).

Signals are 16 kHz int16 made with integer arithmetic that tests/dsp/test_window_splitter.cpp
repeats exactly, so a case names a signal instead of holding it:

    silence   zeros
    constant  every sample = seed
    noise     64-bit LCG (top 16 bits as int16), scaled per 1600-sample block by
              amp = (block * 7919 + seed) % 64 + 1 as (v * amp) >> 6
    gaps      noise with every block where block % 23 == 17 silent
    sine      a 100 Hz sine (Bhaskara I's rational approximation, period 160, peak 32000)
              under the same per-block amp as noise

split_point gets the signal as float32 (int16 / 32768). WindowSplitter gets it as int16 frames
of the given size, one push per frame as windows() does per decoded frame; each window is
recorded as [frame index of the push that returned it (-1: finish), start_s, sample count,
SHA-256 of its float32 little-endian bytes].

windows() reads files: the fixtures of tests/fixtures/audio by name, and WAVs this file writes
into out/parity/speech-windows from the signals above (["wav", kind, rate, channels, frames,
seed]: the signal of frames * channels samples, interleaved, as 16-bit PCM through Python's
wave module), which tests/speech/test_file_windows.cpp writes again byte for byte. Each window
is recorded as [start_s, sample count, SHA-256 of its float32 little-endian bytes].
"""

import hashlib
import wave
from pathlib import Path

import numpy as np

_REPO = Path(__file__).resolve().parents[3]
_FIXTURES = _REPO / "tests" / "fixtures" / "audio"
_SCRATCH = _REPO / "out" / "parity" / "speech-windows"

_MASK64 = (1 << 64) - 1
_BLOCK = 1600
_CACHE = {}


def _amp(blocks, seed):
    return (blocks * 7919 + seed) % 64 + 1


def signal(kind, length, seed):
    key = (kind, length, seed)
    if key in _CACHE:
        return _CACHE[key]
    index = np.arange(length, dtype=np.int64)
    blocks = index // _BLOCK
    if kind == "silence":
        values = np.zeros(length, dtype=np.int64)
    elif kind == "constant":
        values = np.full(length, seed, dtype=np.int64)
    elif kind in ("noise", "gaps"):
        state = seed & _MASK64
        raw = []
        for _ in range(length):
            state = (state * 6364136223846793005 + 1442695040888963407) & _MASK64
            raw.append((state >> 48) - 32768)
        values = (np.asarray(raw, dtype=np.int64) * _amp(blocks, seed)) >> 6
        if kind == "gaps":
            values[blocks % 23 == 17] = 0
    elif kind == "sine":
        phase = index % 160
        q = phase % 80
        base = (16 * q * (80 - q) * 32000) // (5 * 80 * 80 - 4 * q * (80 - q))
        base = np.where(phase < 80, base, -base)
        values = (base * _amp(blocks, seed)) >> 6
    else:
        raise ValueError(kind)
    assert values.min(initial=0) >= -32768 and values.max(initial=0) <= 32767
    result = values.astype(np.int16)
    _CACHE[key] = result
    return result


class _Frame:
    def __init__(self, samples):
        self._samples = samples

    def to_ndarray(self):
        # PyAV's packed s16 mono frame: shape (1, samples).
        return self._samples.reshape(1, -1)


def _digest(window):
    return hashlib.sha256(np.asarray(window, dtype="<f4").tobytes()).hexdigest()


def _split_point(module, kind, length, seed):
    audio = signal(kind, length, seed).astype(np.float32) / 32768.0
    assert audio.dtype == np.float32
    return module.split_point(audio)


def _window_splitter(module, kind, length, seed, frame_size):
    samples = signal(kind, length, seed)
    splitter = module.WindowSplitter()
    windows = []
    for number, start in enumerate(range(0, length, frame_size)):
        for start_s, window in splitter.push([_Frame(samples[start:start + frame_size])]):
            assert window.dtype == np.float32
            windows.append([number, start_s, len(window), _digest(window)])
    for start_s, window in splitter.finish():
        windows.append([-1, start_s, len(window), _digest(window)])
    return {"windows": windows, "total": splitter.total, "offset": splitter.offset}


def _wav_path(kind, rate, channels, frames, seed):
    path = _SCRATCH / f"{kind}-{rate}-{channels}ch-{frames}-{seed}.wav"
    if not path.is_file():
        path.parent.mkdir(parents=True, exist_ok=True)
        samples = signal(kind, frames * channels, seed)
        with wave.open(str(path), "wb") as handle:
            handle.setnchannels(channels)
            handle.setsampwidth(2)
            handle.setframerate(rate)
            handle.writeframes(samples.astype("<i2").tobytes())
    return path


def _windows(module, source, *spec):
    path = _wav_path(*spec) if source == "wav" else _FIXTURES / source
    windows = []
    for start_s, window in module.windows(str(path)):
        assert window.dtype == np.float32
        windows.append([start_s, len(window), _digest(window)])
    return windows


WRAPPERS = {"split_point": _split_point, "WindowSplitter": _window_splitter, "windows": _windows}

PINS = {"MAX_SAMPLES": 480000, "MIN_SAMPLES": 384000, "SAMPLE_RATE": 16000}

_KINDS = [("silence", 0), ("constant", 1000), ("noise", 7), ("gaps", 11), ("sine", 5)]
_SPLIT_LENGTHS = [0, 1, 479999, 480000, 480001, 600000, 959999, 1000000]
_STREAM_LENGTHS = [0, 1, 1600, 479999, 480000, 480001, 1000000, 2000003]
_FRAMES = [160, 1024, 4800, 16000]

CASES = {
    "split_point": [[kind, length, seed] for kind, seed in _KINDS for length in _SPLIT_LENGTHS]
    + [["noise", 480000, 12345], ["sine", 520000, 63], ["gaps", 700000, 3]],
    "WindowSplitter": [[kind, length, seed, frame] for kind, seed in _KINDS
                       for length in _STREAM_LENGTHS for frame in _FRAMES],
    # The fixtures (one window each), then WAVs long enough to be cut: stereo 44.1 kHz past two
    # cuts, mono 16 kHz (the probesize path, no resampling) with quiet blocks to cut in, and
    # mono 48 kHz just over 30 s.
    "windows": [[name] for name in (
        "tone-44k1-mono.wav", "tone-48k-stereo.wav", "tone-16k-mono.wav", "tone-44k1-stereo.mp3",
        "tone-44k1-stereo.m4a", "tone-44k1-stereo.ogg", "tone-48k-mono.flac",
        "tone-44k1-stereo.wma")]
    + [["wav", "sine", 44100, 2, 65 * 44100, 5], ["wav", "gaps", 16000, 1, 70 * 16000, 11],
       ["wav", "sine", 48000, 1, 31 * 48000, 63], ["wav", "constant", 22050, 1, 61 * 22050, 3277]],
}
