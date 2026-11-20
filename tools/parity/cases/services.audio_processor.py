"""Curated inputs for services/audio_processor.py (fv_speech speech/upload_splitter.h).

Signals are made with integer arithmetic that tests/speech/test_upload_splitter.cpp repeats
exactly, so a case names a signal instead of holding it. A 32-bit LCG,
state = (state * 1664525 + 1013904223) mod 2^32 from the seed, gives v = (state >> 16) - 32768
per sample:

    silence   0
    noise     (v * 6) // 32
    gaps      noise, but 0 in every 0.1 s block (rate // 10 samples) whose index % 37 >= 29
    quiet     v // 512
    loud      v
    uniform   float32 (state >> 8) / 2^24 (moving-average cases only)
    abs       float32 |noise| / 32768 (moving-average cases only)

A stereo signal interleaves the signal of `seed` (left) with that of `seed + 1` (right).

Limits name the config values the wrapper sets for the call (and restores):
{"max_file_size_mb", "overlap_s", "min_chunk_s", "silence_s"} -> MAX_FILE_SIZE_MB,
OVERLAP_DURATION_SEC, MIN_CHUNK_DURATION_SEC, SILENCE_DURATION_SEC.

_moving_average is recorded as its float32 values; convolve is np.convolve(x, ones(w)/w,
"same") in float64, the reference the C++ moving_average must meet within FLOAT_TOLERANCE.
split_audio_file writes the signal as a 16-bit PCM WAV with Python's wave module into
out/parity/audio-processor, splits it with a fresh AudioProcessor and records each chunk as
[sample count, SHA-256 of its little-endian PCM].
"""
import hashlib
import sys
import wave
from pathlib import Path

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import ow_isolation  # noqa: E402

WORK = ow_isolation.SCRATCH / "audio-processor"
FLOAT_TOLERANCE = 3e-8

_CONFIG = {"max_file_size_mb": "MAX_FILE_SIZE_MB", "overlap_s": "OVERLAP_DURATION_SEC",
           "min_chunk_s": "MIN_CHUNK_DURATION_SEC", "silence_s": "SILENCE_DURATION_SEC"}


def _lcg(length, seed):
    state = seed & 0xFFFFFFFF
    for _ in range(length):
        state = (state * 1664525 + 1013904223) & 0xFFFFFFFF
        yield state


def pcm(kind, length, rate, seed):
    tenth = max(1, rate // 10)
    out = []
    for i, state in enumerate(_lcg(length, seed)):
        v = (state >> 16) - 32768
        if kind == "silence":
            s = 0
        elif kind == "noise":
            s = (v * 6) // 32
        elif kind == "gaps":
            s = 0 if (i // tenth) % 37 >= 29 else (v * 6) // 32
        elif kind == "quiet":
            s = v // 512
        elif kind == "loud":
            s = v
        else:
            raise ValueError(kind)
        out.append(s)
    return np.array(out, dtype=np.int16)


def floats(kind, length, seed):
    if kind == "uniform":
        return np.array([(state >> 8) / 16777216.0 for state in _lcg(length, seed)],
                        dtype=np.float32)
    if kind == "abs":
        return (np.abs(pcm("noise", length, 16000, seed).astype(np.float32)) / 32768.0).astype(
            np.float32)
    raise ValueError(kind)


class _Limits:
    def __init__(self, limits):
        self.limits = limits

    def __enter__(self):
        from config import config

        self.saved = {name: getattr(config, name) for name in _CONFIG.values()}
        for key, value in self.limits.items():
            setattr(config, _CONFIG[key], value)

    def __exit__(self, *_):
        from config import config

        for name, value in self.saved.items():
            setattr(config, name, value)


def _moving_average(module, kind, length, seed, window):
    return [float(x) for x in module._moving_average(floats(kind, length, seed), window)]


def _convolve(module, kind, length, seed, window):
    samples = floats(kind, length, seed)
    return [float(x) for x in np.convolve(samples, np.ones(window) / window, mode="same")]


def _chunk_limits(module, rate, limits):
    with _Limits(limits):
        return list(module.AudioProcessor()._chunk_limits(rate))


def _generate_time_based_splits(module, total, rate, limits):
    with _Limits(limits):
        return module.AudioProcessor()._generate_time_based_splits(total, rate)


def _find_split_points(module, kind, seconds, rate, seed, limits):
    with _Limits(limits):
        audio = pcm(kind, int(seconds * rate), rate, seed)
        return [int(p) for p in module.AudioProcessor()._find_split_points(audio, rate)]


def _chunk_boundary(module, kind, length, rate, seed, prefix, overlap, limits):
    with _Limits(limits):
        samples = pcm(kind, length, rate, seed)
        return int(module.AudioProcessor()._chunk_boundary(samples, rate, prefix, overlap))


def _find_best_silence(module, kind, length, rate, seed, start, end, silence, limits):
    with _Limits(limits):
        samples = pcm(kind, length, rate, seed)
        absolute = np.abs(samples.astype(np.float32)) / 32768.0
        smooth = module._moving_average(absolute, max(1, int(0.1 * rate)))
        found = module.AudioProcessor()._find_best_silence(smooth, start, end, silence, rate)
        return None if found is None else int(found)


def _split_audio_file(module, kind, seconds, rate, seed, channels, limits):
    WORK.mkdir(parents=True, exist_ok=True)
    frames = int(seconds * rate)
    if channels == 1:
        data = pcm(kind, frames, rate, seed)
    else:
        data = np.stack([pcm(kind, frames, rate, seed), pcm(kind, frames, rate, seed + 1)],
                        axis=1).reshape(-1)
    path = WORK / f"{kind}-{seconds}-{rate}-{seed}-{channels}.wav"
    with wave.open(str(path), "wb") as handle:
        handle.setnchannels(channels)
        handle.setsampwidth(2)
        handle.setframerate(rate)
        handle.writeframes(data.astype("<i2").tobytes())
    processor = module.AudioProcessor()
    with _Limits(limits):
        try:
            chunks = processor.split_audio_file(str(path))
            out = []
            for chunk in chunks:
                with wave.open(chunk, "rb") as handle:
                    pcm_bytes = handle.readframes(handle.getnframes())
                out.append([len(pcm_bytes) // 2, hashlib.sha256(pcm_bytes).hexdigest()])
            return out
        finally:
            processor.cleanup_temp_files()
            path.unlink()


def _combine_transcriptions(module, texts):
    return module.AudioProcessor().combine_transcriptions(texts)


WRAPPERS = {
    "_moving_average": _moving_average,
    "convolve": _convolve,
    "_chunk_limits": _chunk_limits,
    "_generate_time_based_splits": _generate_time_based_splits,
    "_find_split_points": _find_split_points,
    "_chunk_boundary": _chunk_boundary,
    "_find_best_silence": _find_best_silence,
    "split_audio_file": _split_audio_file,
    "combine_transcriptions": _combine_transcriptions,
}

SMALL = {"max_file_size_mb": 0.2, "overlap_s": 1.0, "min_chunk_s": 5, "silence_s": 0.5}
TINY = {"max_file_size_mb": 0.02, "overlap_s": 0.05, "min_chunk_s": 0.1, "silence_s": 0.1}

AVERAGES = [
    ["uniform", 1, 1, 1], ["uniform", 2, 2, 2], ["uniform", 5, 3, 3], ["uniform", 17, 4, 4],
    ["uniform", 17, 5, 7], ["uniform", 100, 6, 50], ["uniform", 1000, 7, 101],
    ["uniform", 1000, 8, 999], ["uniform", 4096, 9, 50], ["uniform", 3000, 10, 1000],
    ["abs", 5000, 11, 1600], ["abs", 4410, 12, 4410], ["uniform", 0, 13, 4410],
    ["uniform", 3, 14, 1], ["uniform", 3, 15, 0],
]

CASES = {
    "_moving_average": AVERAGES,
    "convolve": [case for case in AVERAGES if 0 < case[3] <= case[1]],
    "_chunk_limits": [
        [44100, {}], [16000, {}], [8000, {"max_file_size_mb": 0.02}],
        [8000, {"max_file_size_mb": 0.01, "overlap_s": 2}], [1000, {"max_file_size_mb": 0.01}],
        [8000, {"max_file_size_mb": 0.00005}], [8000, {"max_file_size_mb": 0.0000495}],
        [8000, {"overlap_s": 0}], [192000, {"overlap_s": 30}], [44100, {"max_file_size_mb": 5}],
    ],
    "_generate_time_based_splits": [
        [0, 44100, {}], [12058602, 44100, {}], [12058603, 44100, {}], [44100 * 600, 44100, {}],
        [8000 * 30, 8000, {"max_file_size_mb": 0.05}], [100, 8000, TINY],
        [50000, 8000, TINY], [16000 * 3600, 16000, {}], [48000 * 7200, 48000, {}],
    ],
    "_find_split_points": [
        ["gaps", 60, 8000, 1, SMALL], ["noise", 60, 8000, 2, SMALL],
        ["silence", 60, 8000, 3, SMALL], ["quiet", 40, 8000, 4, SMALL],
        ["gaps", 30, 16000, 5, SMALL], ["loud", 20, 8000, 6, SMALL],
        ["gaps", 120, 4000, 7, {"max_file_size_mb": 0.5, "overlap_s": 2.0, "min_chunk_s": 30,
                                "silence_s": 0.5}],
        ["gaps", 10, 8000, 8, TINY],
    ],
    "_chunk_boundary": [
        ["gaps", 104835, 8000, 1, 0, 8000, SMALL], ["gaps", 104835, 8000, 1, 8000, 8000, SMALL],
        ["noise", 104835, 8000, 2, 0, 8000, SMALL], ["silence", 104835, 8000, 3, 0, 8000, SMALL],
        ["quiet", 104835, 8000, 4, 8000, 8000, SMALL], ["gaps", 104835, 8000, 9, 0, 0, SMALL],
        ["gaps", 10463, 8000, 5, 0, 400, TINY], ["silence", 50, 1000, 6, 0, 10, TINY],
    ],
    "_find_best_silence": [
        ["gaps", 104835, 8000, 1, 40001, 96835, 4000, SMALL],
        ["gaps", 104835, 8000, 1, 96835, 40001, 4000, SMALL],
        ["quiet", 20000, 8000, 2, 1, 19000, 4000, SMALL],
        ["silence", 20000, 8000, 3, 1, 20000, 800, SMALL],
        ["noise", 20000, 8000, 4, 1, 19000, 4000, SMALL],
        ["gaps", 30000, 8000, 5, 1000, 29000, 1, SMALL],
    ],
    "split_audio_file": [
        ["gaps", 60, 8000, 1, 1, SMALL], ["noise", 30, 8000, 2, 1, SMALL],
        ["gaps", 40, 8000, 3, 2, SMALL], ["silence", 25, 8000, 4, 1, SMALL],
        ["gaps", 5, 8000, 5, 1, SMALL], ["quiet", 20, 16000, 6, 1, SMALL],
    ],
    "combine_transcriptions": [
        [[]], [["a"]], [["  a  ", "b"]], [["a", "", "  ", "b"]], [["a  b", "c   d"]],
        [["", ""]], [["　wide　", "x "]], [["a\n", "\nb"]], [["a ", " b ", " c"]],
    ],
}
