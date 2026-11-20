"""Speaker embeddings of three synthetic voice clips through meeting/diarize/embedder.py, for the
[decode] parity test of tests/diarize/test_embedder.cpp (cosine >= 0.999).

Needs the WeSpeaker model the embedder runs, named by FIORIVOCE_TEST_SPEAKER_MODEL (read only;
on the reference machine the owner's %LOCALAPPDATA%\\OpenWhisper\\models\\speaker-id\\
voxceleb_resnet34_LM.onnx), and OpenWhisper's interpreter's onnxruntime. Without the model the
committed vectors are written back unchanged, so a run of every case file gives no diff on a
machine that has no model.

A case is [clip]; the result carries the clip's samples ("pcm": base64 of little-endian int16,
16 kHz mono) so the C++ test embeds exactly what Python embedded, and the embedding as float32
reprs. The clips are synthetic voices (gliding harmonic series with formant weights, a syllable
envelope and a little LCG noise), not recordings of anybody.
"""

import base64
import hashlib
import json
import os
from pathlib import Path

import numpy as np

_RATE = 16000
_MASK64 = (1 << 64) - 1
_REPO = Path(__file__).resolve().parents[3]
_COMMITTED = _REPO / "tests" / "golden" / "meeting.diarize.embedder.json"
_MODEL = os.environ.get("FIORIVOCE_TEST_SPEAKER_MODEL", "")


def _committed():
    if not _COMMITTED.is_file():
        raise SystemExit("meeting.diarize.embedder: set FIORIVOCE_TEST_SPEAKER_MODEL to the "
                         "speaker model; there are no committed vectors to keep.")
    return json.loads(_COMMITTED.read_text(encoding="utf-8"))


def _lcg_noise(count, seed):
    state = seed & _MASK64
    out = np.empty(count)
    for i in range(count):
        state = (state * 6364136223846793005 + 1442695040888963407) & _MASK64
        out[i] = ((state >> 48) - 32768) / 32768.0
    return out


def _voice(seconds, f0_start, f0_end, formants, seed):
    count = int(seconds * _RATE)
    t = np.arange(count) / _RATE
    f0 = np.linspace(f0_start, f0_end, count) * (1.0 + 0.02 * np.sin(2 * np.pi * 5.5 * t))
    phase = 2 * np.pi * np.cumsum(f0) / _RATE
    signal = np.zeros(count)
    for harmonic in range(1, 60):
        freq = f0 * harmonic
        weight = sum(gain * np.exp(-((freq - centre) / width) ** 2)
                     for centre, width, gain in formants) / harmonic ** 0.5
        weight = np.where(freq < _RATE / 2 - 200, weight, 0.0)
        signal += weight * np.sin(harmonic * phase)
    signal *= 0.55 + 0.45 * np.sin(2 * np.pi * 3.7 * t - np.pi / 2)
    signal = 0.5 * signal / np.max(np.abs(signal)) + 0.01 * _lcg_noise(count, seed)
    return np.clip(np.round(signal * 32767.0), -32768, 32767).astype(np.int16)


CLIPS = {
    "low_voice": (1.2, 105.0, 125.0, [(600, 220, 1.0), (1000, 260, 0.7), (2400, 380, 0.25)], 1),
    "mid_voice": (1.2, 170.0, 150.0, [(800, 250, 1.0), (1400, 300, 0.5), (2800, 400, 0.3)], 2),
    "high_voice": (1.2, 235.0, 260.0, [(950, 260, 1.0), (1900, 320, 0.6), (3200, 450, 0.35)], 3),
}

_embedder = None


def _embed(module, clip):
    if not _MODEL:
        for case in _committed()["functions"]["embed"]:
            if case["args"] == [clip]:
                return case["result"]
        raise SystemExit(f"meeting.diarize.embedder: no committed vector for {clip}")
    global _embedder
    if _embedder is None:
        _embedder = module.SpeakerEmbedder(_MODEL)
    pcm = _voice(*CLIPS[clip])
    embedding = _embedder.embed(pcm.astype(np.float32) / np.float32(32768.0))
    return {
        "pcm": base64.b64encode(pcm.astype("<i2").tobytes()).decode("ascii"),
        "samples": int(pcm.size),
        "embedding": [float(str(v)) for v in embedding],
    }


def _pins():
    if not _MODEL:
        pins = _committed()["pins"]
        return {key: pins[key] for key in ("model", "model_sha256", "onnxruntime")}
    import onnxruntime

    digest = hashlib.sha256(Path(_MODEL).read_bytes()).hexdigest()
    return {"model": Path(_MODEL).name, "model_sha256": digest,
            "onnxruntime": onnxruntime.__version__}


PINS = _pins()
WRAPPERS = {"embed": _embed}
CASES = {"embed": [[name] for name in CLIPS]}
