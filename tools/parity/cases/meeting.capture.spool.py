"""Curated inputs for meeting/capture/spool.py (M7.8 meeting-capture).

Audio is described, never listed, so both sides build the same int16 samples with integer
arithmetic only. A signal is a list of segments, each [kind, frames, amp, step]:

    "saw"    ((i * step) % 65536) * 2 * amp // 65536 - amp, i counting from the signal's start
    "const"  amp in every sample
    "zero"   silence

    resample_to_16k(samples, rate)                     the samples as a list
    find_cut_point_signal(signal, rate, target, max, quiet_rms, quiet_window_s)
    cut_scanner_pushes(signal, rate, target, max, quiet_window_s, push_sizes)
        cut_index() after every push of the next push_sizes[i] samples
    gap_fill_frame_count(expected, actual, rate, tolerance)
    spool_run(script, options)
        One SpoolWriter fed by a script, flushed, and its spool folder read back. Steps:
          ["stream", rate, t0, start_index, blocks, block_frames, signal]
              blocks of block_frames samples from signal[start_index + k * block_frames ...],
              stamped t0 + (start_index + k * block_frames) / rate
          ["burst", rate, t, start_index, blocks, block_frames, signal]  every block stamped t
          ["pause", t] | ["resume", t]   the meeting clock pauses or resumes at monotonic t
        Options: target_sec, max_sec, quiet_window_s, initial_seq, register_failures (the first
        n register_chunk calls raise). The clock starts at monotonic 1000.0. The queue holds
        every block (the replay waits for room instead), so nothing is dropped.
        Result: registered chunks (file names, not paths), the chunks on_chunk saw, the final
        chunk flush returned, the folder's files with sizes and SHA-256, the session JSON, the
        session WAV's frame count and SHA-256, health_error and the timeline-fix count.
"""

import hashlib
import json
import os
import shutil
import tempfile
import threading
import wave
from pathlib import Path
from types import SimpleNamespace

import numpy as np

import ow_isolation
from meeting.interfaces import CaptureBlock

CLOCK_START = 1000.0


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


def _resample(module, samples, rate):
    return module.resample_to_16k(np.asarray(samples, dtype=np.int16), rate).tolist()


def _find_cut(module, signal, rate, target, max_sec, quiet_rms, window):
    return module.find_cut_point(_signal(signal), rate, target, max_sec, quiet_rms, window)


def _pushes(module, signal, rate, target, max_sec, window, sizes):
    data = _signal(signal)
    scanner = module._CutScanner(rate, target, max_sec, quiet_window_s=window)
    out, at = [], 0
    for size in sizes:
        scanner.push(data[at:at + size])
        at += size
        out.append(scanner.cut_index())
    return out


class _Repo:
    def __init__(self, failures):
        self.failures = failures
        self.rows = []

    def register_chunk(self, **fields):
        if self.failures > 0:
            self.failures -= 1
            raise RuntimeError("database is locked")
        self.rows.append(dict(fields))
        return len(self.rows)


def _sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def _chunk(chunk):
    if chunk is None:
        return None
    return {"chunk_id": chunk.chunk_id, "channel": chunk.channel, "seq": chunk.seq,
            "file": os.path.basename(chunk.file_path), "start_s": chunk.start_s,
            "duration_s": chunk.duration_s, "sample_rate": chunk.sample_rate}


def _spool_run(module, script, options):
    import meeting.clock as clock_module

    scratch = ow_isolation.SCRATCH / "spool-runs"
    scratch.mkdir(parents=True, exist_ok=True)
    folder = Path(tempfile.mkdtemp(dir=scratch))
    ticks = SimpleNamespace(value=CLOCK_START)
    saved = clock_module.time
    clock_module.time = SimpleNamespace(monotonic=lambda: ticks.value)
    try:
        clock = clock_module.MeetingClock()
        clock.start()
        repo = _Repo(options.get("register_failures", 0))
        seen = []
        lock = threading.Lock()

        def on_chunk(chunk):
            with lock:
                seen.append(_chunk(chunk))

        kwargs = {key: options[key] for key in ("target_sec", "max_sec", "quiet_window_s",
                                                 "initial_seq") if key in options}
        writer = module.SpoolWriter("m_golden", "mic", str(folder), clock, repo,
                                    on_chunk=on_chunk, queue_size=1_000_000, **kwargs)
        for step in script:
            name = step[0]
            if name in ("pause", "resume"):
                ticks.value = step[1]
                getattr(clock, name)()
                continue
            _, rate, t, start, blocks, frames, segments = step
            data = _signal(segments)
            for k in range(blocks):
                index = start + k * frames
                block = data[index:index + frames]
                t_mono = t if name == "burst" else t + index / float(rate)
                writer.feed(CaptureBlock(channel="mic", frames=block, sample_rate=rate,
                                         t_mono=t_mono))
        final = writer.flush()
        files = {}
        for path in sorted(folder.iterdir()):
            files[path.name] = {"size": path.stat().st_size, "sha256": _sha(path)}
        session_json = folder / "mic_session.json"
        session_wav = folder / "mic_session.wav"
        frames = None
        if session_wav.is_file():
            with wave.open(str(session_wav), "rb") as handle:
                frames = handle.getnframes()
        registered = []
        for row in repo.rows:
            row = dict(row)
            row["file_path"] = os.path.basename(row["file_path"])
            registered.append(row)
        return {
            "registered": registered,
            "on_chunk": seen,
            "final": _chunk(final),
            "files": files,
            "session_json": (json.loads(session_json.read_text("utf-8"))
                             if session_json.is_file() else None),
            "session_wav_frames": frames,
            "health_error": writer.health_error,
            "gap_logs": writer._gap_logs,
        }
    finally:
        clock_module.time = saved
        shutil.rmtree(folder, ignore_errors=True)


def _saw(frames, amp=6000, step=641):
    return ["saw", frames, amp, step]


def _speech(seconds, rate, quiet_every_s=None, quiet_len_s=0.6, amp=6000, step=641):
    """Saw-wave "speech" with quiet stretches, as the spool tests' _signal() makes."""
    total = int(seconds * rate)
    if not quiet_every_s:
        return [_saw(total, amp, step)]
    segments, at = [], 0
    every, span = int(quiet_every_s * rate), int(quiet_len_s * rate)
    for start in range(every, total, every):
        segments.append(_saw(start - at, amp, step))
        quiet = min(span, total - start)
        segments.append(["zero", quiet, 0, 0])
        at = start + quiet
    if at < total:
        segments.append(_saw(total - at, amp, step))
    return segments


def _stream(rate, seconds, start=0, t0=CLOCK_START, frames=1024, signal=None, **kw):
    signal = signal or _speech(seconds, rate, **kw)
    total = sum(segment[1] for segment in signal)
    return ["stream", rate, t0, start, (total - start) // frames, frames, signal]


_RATES = [44100, 48000]

CASES = {
    "resample_to_16k": [
        [_signal([_saw(441, 9000, 997)]).tolist(), 44100],
        [_signal([_saw(1024, 12000, 2311)]).tolist(), 44100],
        [_signal([_saw(1023, 20000, 517)]).tolist(), 44100],
        [_signal([_saw(4410, 6000, 641)]).tolist(), 44100],
        [_signal([_saw(480, 9000, 997)]).tolist(), 48000],
        [_signal([_saw(4800, 30000, 3001)]).tolist(), 48000],
        [_signal([_saw(1000, 32767, 7919)]).tolist(), 48000],
        [_signal([["const", 700, -32768, 0], ["const", 700, 32767, 0]]).tolist(), 48000],
        [_signal([_saw(1600, 6000, 641)]).tolist(), 16000],
        [_signal([_saw(2205, 8000, 13)]).tolist(), 22050],
        [_signal([_saw(800, 8000, 13)]).tolist(), 8000],
        [[1000], 44100],
        [[1000, -1000, 3], 44100],
        [[], 44100],
        [[5, 6, 7], 0],
    ],
    "find_cut_point_signal": [
        [[["const", 3 * 16000, 5000, 0], ["zero", int(1.9 * 16000), 0, 0]], 16000, 5.0, 20.0,
         300.0, 0.4],
    ] + [
        [[["const", int(5.1 * r), 5000, 0], ["zero", int(1.9 * r), 0, 0]], r, 5.0, 20.0, 300.0,
         0.4] for r in (16000, 44100, 48000)
    ] + [
        [[["const", 21 * 16000, 5000, 0]], 16000, 5.0, 20.0, 300.0, 0.4],
        [[["const", int(12.5 * 16000), 5000, 0]], 16000, 5.0, 20.0, 300.0, 0.4],
        [[["const", 20 * 16000, 5000, 0]], 16000, 5.0, 20.0, 300.0, 0.4],
        [[["const", 20 * 16000 - 1, 5000, 0]], 16000, 5.0, 20.0, 300.0, 0.4],
        [[["const", 5 * 44100, 299, 0], ["const", 44100, 301, 0]], 44100, 5.0, 20.0, 300.0,
         0.4],
        [[["const", 5 * 44100, 301, 0], ["const", 44100, 299, 0]], 44100, 5.0, 20.0, 300.0,
         0.4],
        [[_saw(6 * 48000, 6000, 641), ["zero", 48000, 0, 0]], 48000, 5.0, 20.0, 300.0, 0.4],
        [[_saw(16 * 16000, 6000, 641), ["zero", 16000, 0, 0]], 16000, 15.0, 25.0, 300.0,
         0.7],
        [[["zero", 6 * 16000, 0, 0]], 16000, 5.0, 20.0, 300.0, 0.4],
        [[["const", 1000, 5000, 0]], 16000, 0.0, 20.0, 300.0, 0.4],
        [[["const", 1000, 5000, 0]], 16000, 0.0, 0.0, 300.0, 0.4],
        [[["zero", 100, 0, 0]], 1000, 0.05, 1.0, 300.0, 0.05],
        [[], 16000, 5.0, 20.0, 300.0, 0.4],
    ],
    "cut_scanner_pushes": [
        [[["const", int(5.1 * 44100), 5000, 0], ["zero", int(1.9 * 44100), 0, 0]], 44100, 5.0,
         20.0, 0.4, [1024] * 301],
        [[_saw(21 * 16000, 6000, 641)], 16000, 5.0, 20.0, 0.4, [1600, 799, 1, 333] * 100
         + [400000]],
        [[_saw(6 * 48000, 6000, 641), ["zero", 48000, 0, 0]], 48000, 5.0, 20.0, 0.4,
         [7, 13, 2400, 1024] * 80],
        [[["const", 1000, 5000, 0]], 16000, 0.0, 20.0, 0.4, [0, 1, 999]],
    ],
    "gap_fill_frame_count": [
        [None, 1.0, 16000, 0.12],
        [0.0, 0.12, 16000, 0.12],
        [0.0, 0.1, 16000, 0.12],
        [2.5, 2.55, 16000, 0.12],
        [5.0, 5.25, 16000, 0.12],
        [0.0, 0.121, 16000, 0.12],
        [0.0, 0.12000001, 16000, 0.12],
        [10.0, 9.0, 16000, 0.12],
        [1.0, 31.5, 44100, 0.12],
        [0.0, 0.5000312, 16000, 0.12],
        [0.0, 0.00003125 * 3, 16000, 0.0],
        [0.0, 1.5, 48000, 1.0],
        [100.0, 100.130001, 48000, 0.12],
    ],
    "spool_run": [
        # 60 s at 44.1 kHz with quiet stretches every 7 s: no drift, contiguous chunks.
        [[_stream(44100, 60.0, quiet_every_s=7.0)], {}],
        # A stall and a burst of 20 same-stamped blocks at 48 kHz.
        [[["stream", 48000, CLOCK_START, 0, 235, 1024, _speech(30.0, 48000, 7.0)],
          ["burst", 48000, CLOCK_START + 240000 / 48000.0, 235 * 1024, 20, 1024,
           _speech(30.0, 48000, 7.0)],
          ["stream", 48000, CLOCK_START, 255 * 1024, 1406 - 255, 1024,
           _speech(30.0, 48000, 7.0)]], {}],
        # A dropout of 0.5 s at 16 kHz is filled with silence.
        [[_stream(16000, 1.0), ["stream", 16000, CLOCK_START, 24000, 15, 1024,
                                [["zero", 24000, 0, 0], _saw(16000)]]], {}],
        # Paused before the first block: nothing is written.
        [[["pause", CLOCK_START], _stream(16000, 3.0)], {}],
        # Paused in the middle: meeting time stands still across the pause.
        [[["stream", 16000, CLOCK_START, 0, 40, 1024, _speech(10.0, 16000)],
          ["pause", CLOCK_START + 2.56], ["stream", 16000, CLOCK_START, 40 * 1024, 20, 1024,
                                          _speech(10.0, 16000)],
          ["resume", CLOCK_START + 6.0],
          ["stream", 16000, CLOCK_START + 6.0 - 3.84, 60 * 1024, 40, 1024,
           _speech(10.0, 16000)]], {}],
        # A rate change from 44.1 to 48 kHz after a one-second outage.
        [[_stream(44100, 0.5), ["stream", 48000, CLOCK_START + (21 * 1024) / 44100.0 + 1.0,
                                0, 23, 1024, [_saw(24000, 6000, 641)]]], {}],
        # Registration fails twice: the WAV is orphaned with its sidecar.
        [[_stream(16000, 1.0)], {"register_failures": 2}],
        # Registration fails once: the retry registers it.
        [[_stream(16000, 1.0)], {"register_failures": 1}],
        # A short remainder is kept; one under 0.25 s is dropped.
        [[_stream(16000, 0.5)], {}],
        [[_stream(16000, 0.2)], {}],
        # A 30 s gap is filled in bounded pieces with short chunks; the sequence starts at 7.
        [[_stream(16000, 0.25), ["stream", 16000, CLOCK_START + 3 * 1024 / 16000.0 + 30.0, 0,
                                 3, 1024, [_saw(4000)]]],
         {"target_sec": 1.0, "max_sec": 2.0, "initial_seq": 7}],
        # Overlap and small jitter: blocks stamped early by 0.2 s and late by 0.1 s.
        [[["stream", 48000, CLOCK_START, 0, 30, 1024, [_saw(48000 * 2)]],
          ["stream", 48000, CLOCK_START - 0.2, 30 * 1024, 30, 1024, [_saw(48000 * 2)]],
          ["stream", 48000, CLOCK_START - 0.1, 60 * 1024, 30, 1024, [_saw(48000 * 2)]]], {}],
        # Mono speech with long pauses over 25 s at 48 kHz, 960-frame blocks.
        [[_stream(48000, 25.0, frames=960, quiet_every_s=3.0, quiet_len_s=1.0)],
         {"quiet_window_s": 0.5}],
    ],
}

WRAPPERS = {
    "resample_to_16k": _resample,
    "find_cut_point_signal": _find_cut,
    "cut_scanner_pushes": _pushes,
    "spool_run": _spool_run,
}

PINS = {
    "clock_start": CLOCK_START,
    "signal_note": "segments [kind, frames, amp, step]; saw = ((i * step) % 65536) * 2 * amp "
                   "// 65536 - amp with i counted from the signal's first sample",
}

# Chunk offsets and durations are sums of exact sample counts divided by the rate.
FLOAT_TOLERANCE = 1e-9
