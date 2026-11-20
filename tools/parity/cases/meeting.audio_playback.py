"""Curated inputs for meeting/audio_playback.py (M7.12 meeting-asr).

    render_mono_chunks(chunks)
        _render_mono over chunk WAVs written to a scratch folder. Each chunk is [id, start_s,
        duration_s, signal, file_rate, channels]: the signal (segments [kind, frames, amp, step]
        as in meeting.capture.spool's cases) is written as a 16-bit WAV at file_rate, every
        channel carrying the same samples. The result is the rendered WAV's frame count, the
        SHA-256 of its frames and its first 16 samples; a chunk the render cannot read raises.
"""

import hashlib
import shutil
import tempfile
import wave
from pathlib import Path

import numpy as np

import ow_isolation


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


def _render(module, chunks):
    scratch = ow_isolation.SCRATCH / "playback-runs"
    scratch.mkdir(parents=True, exist_ok=True)
    folder = Path(tempfile.mkdtemp(dir=scratch))
    try:
        rows = []
        for index, (chunk_id, start_s, duration_s, signal, rate, channels) in enumerate(chunks):
            path = folder / f"chunk{index}.wav"
            mono = _signal(signal)
            frames = np.repeat(mono, channels) if channels > 1 else mono
            with wave.open(str(path), "wb") as handle:
                handle.setnchannels(channels)
                handle.setsampwidth(2)
                handle.setframerate(rate)
                handle.writeframes(frames.astype("<i2").tobytes())
            rows.append({"id": chunk_id, "file_path": str(path), "start_s": start_s,
                         "duration_s": duration_s})
        output = folder / "playback.wav"
        module._render_mono(rows, str(output))
        with wave.open(str(output), "rb") as handle:
            count = handle.getnframes()
            raw = handle.readframes(count)
        samples = np.frombuffer(raw, dtype="<i2")
        return {"frames": count, "sha256": hashlib.sha256(raw).hexdigest(),
                "head": samples[:16].tolist()}
    finally:
        shutil.rmtree(folder, ignore_errors=True)


def _saw(frames, amp=6000, step=641):
    return ["saw", frames, amp, step]


CASES = {
    "render_mono_chunks": [
        # tests/test_meeting_data_lifecycle.py's layout: two channels overlapping, then silence.
        [[[1, 0.0, 0.25, [["const", 8000, 1000, 0]], 16000, 1],
          [2, 0.0, 0.25, [["const", 8000, 3000, 0]], 16000, 1],
          [3, 1.0, 0.25, [["const", 8000, 4000, 0]], 16000, 1]]],
        # Overlapping saws: negative sums floor when averaged.
        [[[1, 0.0, 1.0, [_saw(16000, 9000, 997)], 16000, 1],
          [2, 0.5, 1.0, [_saw(16000, 7000, 2311)], 16000, 1]]],
        # Chunks crossing the 10 s render block, and three contributors at once.
        [[[1, 9.5, 1.0, [_saw(16000)], 16000, 1],
          [2, 9.75, 0.5, [_saw(8000, 30000, 3001)], 16000, 1],
          [3, 9.9, 0.2, [["const", 3200, -32768, 0]], 16000, 1]]],
        # A WAV shorter than its row's duration leaves silence.
        [[[1, 0.0, 1.0, [_saw(4000)], 16000, 1]]],
        # Incompatible chunks are skipped: stereo, 44.1 kHz.
        [[[1, 0.0, 0.5, [_saw(8000)], 16000, 2],
          [2, 0.0, 0.5, [_saw(22050)], 44100, 1],
          [3, 0.25, 0.5, [["const", 8000, -5, 0]], 16000, 1]]],
        # Start times between samples round half to even.
        [[[1, 0.00003125, 0.1, [["const", 1600, 100, 0]], 16000, 1],
          [2, 0.00009375, 0.1, [["const", 1600, 200, 0]], 16000, 1]]],
        # A chunk whose file ends before a later render block: setpos fails.
        [[[1, 0.0, 12.0, [_saw(16000)], 16000, 1]]],
        # Nothing but a zero-length chunk.
        [[[1, 0.0, 0.0, [_saw(160)], 16000, 1]]],
    ],
}

WRAPPERS = {
    "render_mono_chunks": _render,
}

PINS = {
    "signal_note": "segments [kind, frames, amp, step]; saw = ((i * step) % 65536) * 2 * amp "
                   "// 65536 - amp with i counted from the signal's first sample",
}
