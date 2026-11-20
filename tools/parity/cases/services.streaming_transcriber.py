"""Curated inputs for services/streaming_transcriber.py (fv_dsp fft_resample).

A case is [kind, n_in, num_samples, seed]: the wrapper builds the float32 input from it with
integer arithmetic that tests/dsp/test_fft_resample.cpp repeats exactly, so the vector file
holds only the outputs. Each output sample is written as the shortest repr of its float32
value, which reads back to the same float32.

Kinds: "noise" (64-bit LCG, top 16 bits as int16 / 32768), "dc" (0.25), "alternating"
(+0.5, -0.5, ...: all energy at Nyquist), "impulse" (1.0 at seed % n_in).
"""

import numpy as np

FLOAT_TOLERANCE = 1e-6

_MASK64 = (1 << 64) - 1


def _lcg_int16(count, seed):
    state = seed & _MASK64
    out = []
    for _ in range(count):
        state = (state * 6364136223846793005 + 1442695040888963407) & _MASK64
        out.append((state >> 48) - 32768)
    return out


def signal(kind, n_in, seed):
    if kind == "noise":
        return np.asarray(_lcg_int16(n_in, seed), dtype=np.int16).astype(np.float32) / 32768.0
    if kind == "dc":
        return np.full(n_in, 0.25, dtype=np.float32)
    if kind == "alternating":
        return np.asarray([0.5 if i % 2 == 0 else -0.5 for i in range(n_in)], dtype=np.float32)
    if kind == "impulse":
        samples = np.zeros(n_in, dtype=np.float32)
        if n_in:
            samples[seed % n_in] = 1.0
        return samples
    raise ValueError(kind)


def _fft_resample(module, kind, n_in, num_samples, seed):
    samples = signal(kind, n_in, seed)
    assert samples.dtype == np.float32
    out = module.fft_resample(samples, num_samples)
    assert out.dtype == np.float32 and len(out) == num_samples
    return [float(str(value)) for value in out]


def _append_preview_text(module, existing, chunk_text, max_overlap_words=0):
    # max_overlap_words is keyword-only.
    return module.append_preview_text(existing, chunk_text, max_overlap_words=max_overlap_words)


def _prepare_preview_audio(module, dtype, channels, sample_rate, frames, seed):
    """[dtype, channels, sample_rate, frames, seed]: LCG samples as int16, or as float32 / 32768
    (tests/speech/test_window_preview.cpp builds the same input), interleaved when channels > 1."""
    values = np.asarray(_lcg_int16(frames * channels, seed), dtype=np.int16)
    if dtype == "float32":
        values = values.astype(np.float32) / np.float32(32768.0)
    if channels > 1:
        values = values.reshape(frames, channels)
    out = module.prepare_preview_audio(values, sample_rate)
    if out is None:
        return None
    assert out.dtype == np.float32
    return [float(str(value)) for value in out]


WRAPPERS = {
    "fft_resample": _fft_resample,
    "append_preview_text": _append_preview_text,
    "prepare_preview_audio": _prepare_preview_audio,
}

# The seam table of test_overlap_join_is_bounded_and_preserves_ambiguous_repetitions, then
# casefolding, curly apostrophes, the bounds and the 512-code-point look-back on both sides.
_LONG_TAIL = "filler " * 80 + "alpha beta"
_FAR_TAIL = "alpha beta " + "q" * 520
_LONG_HEAD = "alpha beta " + "words " * 90 + "end"
_APPEND_CASES = [
    ["Meet the Alpha team.", "alpha TEAM, tomorrow morning.", 6],
    ["one two three", "two three four", 6],
    ["very", "very good", 6],
    ["yes yes", "yes yes", 6],
    ["one two", "one two three", 0],
    ["we don't know", "don’t know yet", 6],
    ["one two three", "one two three four", 2],
    ["one two", "", 6],
    ["", "  hello there  ", 6],
    ["", "", 6],
    ["abc", "   ", 3],
    [" a b ", " b c ", 6],
    ["one two", "one two three", 1],
    ["one two three", "two three", 6],
    ["Straße frei", "STRASSE FREI jetzt", 6],
    ["it's fine", "it’s fine now", 6],
    ["it’s fine", "IT'S FINE now", 6],
    ["perché no", "Perché no davvero", 6],
    ["a b c d e f g h", "c d e f g h i", 6],
    ["a b c d e f g h", "b c d e f g h i", 6],
    ["a b c d e f g h", "b c d e f g h i", 12],
    ["Hello, world.", "world. Again", 6],
    ["the end, the end", "the end, the end. Fin", 6],
    [_LONG_TAIL, "alpha beta gamma", 6],
    [_FAR_TAIL, "alpha beta gamma", 6],
    ["one alpha beta", _LONG_HEAD, 6],
    ["Été chaud", "étÉ CHAUD encore", 6],
    ["x_y z9", "X_Y Z9 more", 6],
]

_PREPARE_CASES = [
    ["int16", 1, 44100, 1024, 11],
    ["int16", 1, 44100, 4410, 12],
    ["int16", 1, 44100, 2205, 13],
    ["int16", 1, 44100, 2, 14],
    ["int16", 1, 44100, 3, 15],
    ["int16", 1, 16000, 100, 16],
    ["int16", 1, 16000, 0, 17],
    ["int16", 1, 48000, 960, 18],
    ["int16", 1, 8000, 300, 19],
    ["int16", 2, 48000, 960, 20],
    ["int16", 2, 44100, 1024, 21],
    ["int16", 3, 22050, 500, 22],
    ["float32", 1, 44100, 1000, 23],
    ["float32", 2, 8000, 300, 24],
    ["float32", 1, 16000, 64, 25],
]

# (n_in, num_samples): equal, up and down, odd and even on both sides, lengths with large prime
# factors (pocketfft's Bluestein path: 4093 and 2039 are prime) and the 1..4096 range ends.
_PAIRS = [
    (1, 1), (1, 2), (1, 5), (2, 1), (2, 2), (2, 3), (3, 2), (3, 7), (4, 4), (4, 5), (4, 8),
    (5, 4), (5, 2), (7, 3), (8, 4), (8, 5), (8, 13), (15, 16), (16, 15), (16, 32), (31, 64),
    (64, 31), (100, 37), (101, 256), (127, 128), (128, 127), (255, 256), (256, 255),
    (441, 160), (160, 441), (999, 1000), (1000, 999), (1001, 1000), (1024, 1023), (1023, 4096),
    (2039, 2048), (2047, 2048), (2048, 743), (2048, 2048), (4093, 1489), (4095, 4096),
    (4096, 4095), (4096, 1486), (4096, 4096), (2205, 800),
]

CASES = {
    "fft_resample": (
        [["noise", n_in, n_out, 1000 + i] for i, (n_in, n_out) in enumerate(_PAIRS)]
        + [["alternating", n_in, n_out, 0] for n_in, n_out in
           [(2, 3), (4, 5), (4, 2), (8, 4), (8, 6), (8, 7), (16, 9), (64, 128), (4096, 4095),
            (4096, 2048), (7, 4), (9, 12)]]
        + [["dc", n_in, n_out, 0] for n_in, n_out in [(1, 3), (6, 4), (11, 16), (1000, 441)]]
        + [["impulse", n_in, n_out, seed] for n_in, n_out, seed in
           [(10, 10, 3), (10, 4, 0), (10, 25, 9), (33, 32, 16), (512, 160, 7)]]
        # numpy raises ValueError for a zero-length input or output.
        + [["noise", 0, 5, 1], ["noise", 5, 0, 1], ["dc", 0, 0, 0]]
    ),
    "append_preview_text": _APPEND_CASES,
    "prepare_preview_audio": _PREPARE_CASES,
}
