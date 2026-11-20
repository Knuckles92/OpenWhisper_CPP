"""Curated inputs for meeting/diarize/fbank.py (fv_dsp compute_fbank, apply_cmn).

A case is [kind, mode]. The wrapper builds the kind's int16 signal and returns it with the
features, so the C++ replay (tests/dsp/test_fbank.cpp) reads the very samples Python used:
"pcm" is base64 of little-endian int16. Mode "float" passes the samples as float32 value / 32768
(what the embedder hands over), "int16" passes the int16 array itself (compute_fbank normalises
it). "fbank" and "cmn" are the [frames, 80] features before and after apply_cmn, row-major, each
value the shortest repr of its float32.

Kinds: tones at 440 Hz and 7800 Hz (near Nyquist), LCG noise, a synthetic voice (a gliding
harmonic series with formant weights and a syllable envelope), digital silence, and the framing
edges (100, 400, 559 and 560 samples: no frame, one, one, two).
"""

import base64

import numpy as np

FLOAT_TOLERANCE = 1e-4

_RATE = 16000
_MASK64 = (1 << 64) - 1


def _lcg_int16(count, seed):
    state = seed & _MASK64
    out = []
    for _ in range(count):
        state = (state * 6364136223846793005 + 1442695040888963407) & _MASK64
        out.append((state >> 48) - 32768)
    return np.asarray(out, dtype=np.int16)


def _to_int16(samples):
    return np.clip(np.round(samples * 32767.0), -32768, 32767).astype(np.int16)


def _voice(count, f0_start, f0_end, formants):
    t = np.arange(count) / _RATE
    f0 = np.linspace(f0_start, f0_end, count)
    phase = 2 * np.pi * np.cumsum(f0) / _RATE
    signal = np.zeros(count)
    for harmonic in range(1, 40):
        freq = f0 * harmonic
        weight = sum(gain * np.exp(-((freq - centre) / width) ** 2)
                     for centre, width, gain in formants) / harmonic ** 0.5
        weight = np.where(freq < _RATE / 2 - 200, weight, 0.0)
        signal += weight * np.sin(harmonic * phase)
    envelope = 0.55 + 0.45 * np.sin(2 * np.pi * 4.0 * t - np.pi / 2)
    signal *= envelope
    return 0.6 * signal / np.max(np.abs(signal))


def signal(kind):
    if kind == "tone_440":
        t = np.arange(4000)
        return _to_int16(0.5 * np.sin(2 * np.pi * 440.0 * t / _RATE))
    if kind == "tone_7800":
        t = np.arange(3200)
        return _to_int16(0.5 * np.sin(2 * np.pi * 7800.0 * t / _RATE))
    if kind == "noise":
        return _lcg_int16(4000, 31)
    if kind == "voice":
        return _to_int16(_voice(6400, 118.0, 152.0, [(700, 250, 1.0), (1200, 300, 0.6),
                                                     (2600, 400, 0.3)]))
    if kind == "silence":
        return np.zeros(3200, dtype=np.int16)
    if kind.startswith("samples_"):
        return _lcg_int16(int(kind.split("_")[1]), 77)
    raise ValueError(kind)


def _features(module, kind, mode):
    pcm = signal(kind)
    audio = pcm if mode == "int16" else pcm.astype(np.float32) / np.float32(32768.0)
    feats = module.compute_fbank(audio)
    assert feats.dtype == np.float32 and feats.shape[1] == module.NUM_MEL_BINS
    cmn = module.apply_cmn(feats)
    return {
        "pcm": base64.b64encode(pcm.astype("<i2").tobytes()).decode("ascii"),
        "frames": int(feats.shape[0]),
        "fbank": [float(str(v)) for v in feats.reshape(-1)],
        "cmn": [float(str(v)) for v in cmn.reshape(-1)],
    }


def _mel_filters(module):
    """Per filter: first and last bin with weight, and the weights' float32 sum."""
    filters = module._build_mel_filters()
    out = []
    for row in filters:
        nonzero = np.nonzero(row)[0]
        out.append([int(nonzero[0]), int(nonzero[-1]), float(str(row.sum(dtype=np.float32)))])
    return out


def _window(module):
    return [float(str(v)) for v in module._build_window()]


WRAPPERS = {"compute_fbank": _features, "mel_filters": _mel_filters, "window": _window}

CASES = {
    "compute_fbank": [
        ["tone_440", "float"],
        ["tone_7800", "float"],
        ["noise", "float"],
        ["noise", "int16"],
        ["voice", "float"],
        ["silence", "float"],
        ["samples_100", "float"],
        ["samples_400", "float"],
        ["samples_559", "float"],
        ["samples_560", "int16"],
    ],
    "mel_filters": [[]],
    "window": [[]],
}
