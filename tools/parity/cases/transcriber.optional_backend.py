"""Curated inputs for transcriber/optional_backend.py (fv_speech ParakeetBackend).

join_texts and _audible take literal values (_audible's as float32, as a window is).

_transcribe_audio runs LocalSpeechBackend._transcribe_audio over a named signal with a fake
_recognize that tests/speech/test_parakeet_backend.cpp repeats: call n (from 1) returns the text
"w<n>" and one segment {"text": "w<n>", "start": 0.0, "end": <window samples> / 16000}.
Signals, float32:

    constant  every sample = value
    zeros     silence
    ramp      sample i = ((i % 3200) - 1600) * value / 1600, a sawtooth whose peak is value
    burst     zeros, with value in the 0.1 s blocks whose index is a multiple of 7

decode is a real decode, when OPENWHISPER_CPP_TEST_RUNTIME_DIR and OPENWHISPER_CPP_TEST_MODEL name an
installed NeMo-Speech.cpp runtime and Parakeet model (read-only): LocalSpeechBackend.transcribe
of a fixture WAV of tests/fixtures/audio, its _recognize doing what the worker does for
Parakeet on the CPU (NvidiaRecognizer(runtime, model, "cpu").transcribe(samples, language),
language as _request_audio and native_language_code choose it). Without the variables there
are no decode cases.
"""
from __future__ import annotations

import array
import os
from pathlib import Path

import numpy as np

_REPO = Path(__file__).resolve().parents[3]
_FIXTURES = _REPO / "tests" / "fixtures" / "audio"
_BLOCK = 1600


def _signal(kind, length, value):
    index = np.arange(length, dtype=np.int64)
    if kind == "constant":
        return np.full(length, value, dtype=np.float32)
    if kind == "zeros":
        return np.zeros(length, dtype=np.float32)
    if kind == "ramp":
        return (((index % 3200) - 1600).astype(np.float32) * np.float32(value)
                / np.float32(1600)).astype(np.float32)
    if kind == "burst":
        audio = np.zeros(length, dtype=np.float32)
        audio[(index // _BLOCK) % 7 == 0] = np.float32(value)
        return audio
    raise ValueError(kind)


def _backend_class(module):
    class Backend(module.LocalSpeechBackend):
        _settings = staticmethod(lambda: {})

    return Backend


def _transcribe_audio(module, kind, length, value):
    backend = _backend_class(module)("parakeet")
    calls = []

    def recognize(window, language=None, **_options):
        calls.append(len(window))
        text = f"w{len(calls)}"
        return dict(text=text, segments=[dict(text=text, start=0.0, end=len(window) / 16000)])

    backend._recognize = recognize
    result = backend._transcribe_audio(_signal(kind, length, value))
    return {"text": result["text"], "segments": result["segments"], "windows": calls}


def _join_texts(module, texts):
    return module.LocalSpeechBackend.join_texts(texts)


def _audible(module, values):
    # bool(): a numpy bool would be recorded as its repr.
    return bool(module.LocalSpeechBackend._audible(np.asarray(values, dtype=np.float32)))


def _decode(module, name):
    from services.local_asr import worker
    from services.local_asr.nvidia import NvidiaRecognizer

    runtime = os.environ["OPENWHISPER_CPP_TEST_RUNTIME_DIR"]
    model = os.environ["OPENWHISPER_CPP_TEST_MODEL"]
    recognizer = NvidiaRecognizer(runtime, model, "cpu")
    backend = _backend_class(module)("parakeet")

    def recognize(window, language=None, **_options):
        language = language or backend.request_language()
        samples = array.array("f", np.asarray(window, dtype=np.float32).tobytes())
        return recognizer.transcribe(samples, worker.native_language_code("parakeet", language))

    backend._recognize = recognize
    backend.is_available = lambda: True
    try:
        return backend.transcribe(str(_FIXTURES / name))
    finally:
        recognizer.close()


WRAPPERS = {
    "_transcribe_audio": _transcribe_audio,
    "join_texts": _join_texts,
    "_audible": _audible,
    "decode": _decode,
}

PINS = {
    "request_language": "en (local_asr_language's default) for every decode case",
    "decode_device": "cpu",
}

CASES = {
    "_transcribe_audio": [
        ["constant", 61 * 16000, 0.1],
        ["zeros", 48000, 0.0],
        ["zeros", 0, 0.0],
        ["constant", 16000, 0.00025],
        ["constant", 16000, 0.00026],
        ["constant", 30 * 16000 - 1, 0.5],
        ["constant", 30 * 16000, 0.5],
        ["constant", 30 * 16000 + 1, 0.5],
        ["ramp", 95 * 16000 + 123, 0.3],
        ["burst", 75 * 16000, 0.2],
        ["burst", 100 * 16000, 0.0002],
    ],
    "join_texts": [
        [[]],
        [["one", "two"]],
        [[" one ", "", "  ", "two\n"]],
        [["", ""]],
        [["　wide　", " nbsp", "tab\t"]],
        [["a b", " c  d "]],
        [["\x1c", "x\x85"]],
    ],
    "_audible": [
        [[]],
        [[0.0]],
        [[0.0, 0.0, 0.0]],
        [[0.00025]],
        [[-0.00025]],
        [[0.00026]],
        [[-0.00026, 0.0]],
        [[0.000250001]],
        [[0.0, 1.0]],
        [[-1.0]],
    ],
}

if os.environ.get("OPENWHISPER_CPP_TEST_RUNTIME_DIR") and os.environ.get("OPENWHISPER_CPP_TEST_MODEL"):
    CASES["decode"] = [[name] for name in (
        "tone-44k1-mono.wav", "tone-48k-stereo.wav", "tone-16k-mono.wav")]
