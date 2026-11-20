"""Curated inputs for meeting/asr/engine.py (M7.12 meeting-asr).

Signals are described as in meeting.capture.spool's cases: [kind, frames, amp, step] segments,
"saw" being ((i * step) % 65536) * 2 * amp // 65536 - amp with i counted from the start.

    _stable_segment_id(meeting_id, chunk_id, ordinal)      on a stub engine with meeting_id
    _is_digital_silence(frames)                            frames as an int16 list
    prepare_for_whisper_signal(signal, rate)               meeting.asr.audio's conversion, as
                                                           the engine imports it: float32 list
    chunk_segments(meeting_id, chunk_id, channel, start_s, signal, rate, segments)
        _transcribe_chunk on a stub engine: the signal written as the chunk's 16-bit mono WAV,
        a model answering `segments` ([start, end, text]); the result as [segment_id, chunk_id,
        channel, start_s, end_s, text] rows, and whether the model was called.
"""

import tempfile
import wave
from pathlib import Path
from types import SimpleNamespace

import numpy as np

import ow_isolation
from meeting.interfaces import SpooledChunk


def _signal(segments):
    parts = []
    offset = 0
    for kind, frames, amp, step in segments:
        index = np.arange(offset, offset + frames, dtype=np.int64)
        if kind == "saw":
            values = ((index * step) % 65536) * 2 * amp // 65536 - amp
        elif kind == "const":
            values = np.full(frames, amp, dtype=np.int64)
        elif kind == "zero":
            values = np.zeros(frames, dtype=np.int64)
        else:
            raise ValueError(f"unknown segment {kind}")
        parts.append(values.astype(np.int16))
        offset += frames
    return np.concatenate(parts) if parts else np.zeros(0, dtype=np.int16)


def _stable_id(module, meeting_id, chunk_id, ordinal):
    stub = SimpleNamespace(meeting_id=meeting_id)
    return module.MeetingAsrEngine._stable_segment_id(stub, chunk_id, ordinal)


def _silence(module, frames):
    return module.MeetingAsrEngine._is_digital_silence(np.asarray(frames, dtype=np.int16))


def _prepare(module, signal, rate):
    return module.prepare_for_whisper(_signal(signal), rate).tolist()


class _Model:
    def __init__(self, segments):
        self.segments = segments
        self.calls = 0

    def transcribe(self, audio, **_options):
        self.calls += 1
        return iter([SimpleNamespace(start=s, end=e, text=t) for s, e, t in self.segments]), \
            SimpleNamespace()


def _chunk_segments(module, meeting_id, chunk_id, channel, start_s, signal, rate, segments):
    scratch = ow_isolation.SCRATCH / "asr-chunks"
    scratch.mkdir(parents=True, exist_ok=True)
    folder = Path(tempfile.mkdtemp(dir=scratch))
    path = folder / "chunk.wav"
    frames = _signal(signal)
    with wave.open(str(path), "wb") as handle:
        handle.setnchannels(1)
        handle.setsampwidth(2)
        handle.setframerate(rate)
        handle.writeframes(frames.tobytes())
    model = _Model(segments)
    engine = module.MeetingAsrEngine.__new__(module.MeetingAsrEngine)
    engine.meeting_id = meeting_id
    engine.language = None
    engine._language_votes = {}
    engine._backend = SimpleNamespace(model=model)
    chunk = SpooledChunk(chunk_id=chunk_id, meeting_id=meeting_id, channel=channel, seq=0,
                         file_path=str(path), start_s=start_s,
                         duration_s=len(frames) / float(rate), sample_rate=rate)
    try:
        result = engine._transcribe_chunk(chunk)
    finally:
        path.unlink()
        folder.rmdir()
    return {
        "segments": [[s.segment_id, s.chunk_id, s.channel, s.start_s, s.end_s, s.text]
                     for s in result],
        "decoded": model.calls > 0,
    }


_TONE = ["saw", 8000, 6000, 641]

CASES = {
    "_stable_segment_id": [
        ["m_test", 1, 0],
        ["m_test", 1, 1],
        ["m_test", 2, 0],
        ["m", 0, 0],
        ["mé ting", 123456789, 41],
        ["", -1, 0],
    ],
    "_is_digital_silence": [
        [[]],
        [[0]],
        [[8, -8, 0]],
        [[9]],
        [[-9]],
        [[-32768]],
        [[32767]],
        [[0] * 100 + [8]],
        [[3] * 1000 + [-9]],
    ],
    "prepare_for_whisper_signal": [
        [[["saw", 400, 9000, 997]], 16000],
        [[["const", 3, -32768, 0], ["const", 3, 32767, 0]], 16000],
        [[["saw", 441, 9000, 997]], 44100],
        [[["saw", 480, 30000, 3001]], 48000],
        [[["saw", 160, 8000, 13]], 8000],
        [[["saw", 7, 8000, 13]], 22050],
        [[["const", 1, 1000, 0]], 44100],
        [[], 44100],
        [[], 0],
        [[["const", 5, 1000, 0]], 0],
        [[["const", 5, 1000, 0]], -16000],
    ],
    "chunk_segments": [
        ["m_test", 1, "mic", 10.0, [_TONE], 16000,
         [[0.5, 1.5, "  hello world  "], [2.0, 3.0, ""]]],
        ["m_test", 7, "loopback", 3.25, [_TONE], 16000,
         [[0.0, 0.2, ""], [0.2, 0.3, " "], [0.3, 0.4, "kept"], [0.4, 0.45, "　also\n"]]],
        ["m_test", 2, "mic", 0.0, [["const", 8000, 8, 0]], 16000, [[0.0, 0.5, "never"]]],
        ["m_test", 3, "mic", 0.0, [["const", 8000, -9, 0]], 16000, [[0.0, 0.5, "heard"]]],
        ["m_test", 4, "mic", 1.5, [["saw", 22050, 6000, 641]], 44100, [[0.1, 0.2, "resampled"]]],
        ["m_test", 5, "mic", 0.0, [], 16000, [[0.0, 0.5, "empty"]]],
        ["m_test", 6, "mic", 0.0, [_TONE], 16000, []],
    ],
}

WRAPPERS = {
    "_stable_segment_id": _stable_id,
    "_is_digital_silence": _silence,
    "prepare_for_whisper_signal": _prepare,
    "chunk_segments": _chunk_segments,
}

PINS = {
    "signal_note": "segments [kind, frames, amp, step]; saw = ((i * step) % 65536) * 2 * amp "
                   "// 65536 - amp with i counted from the signal's first sample",
}

# float32 resampling (dsp::fft_resample) matches numpy to 1e-6.
FLOAT_TOLERANCE = 1e-6
