"""Curated inputs for meeting/recovery.py (M7.14 meeting-recovery).

Audio is described, never listed, as in meeting.capture.spool's cases: a signal is a list of
segments [kind, frames, amp, step] ("saw", "const", "zero"), built with integer arithmetic so both
sides write the same int16 samples.

    recover_pcm_tails_run(spec)
        _recover_pcm_tails over a spool folder built from `spec`:
          "chunks"    the meeting's registered chunk rows (channel, seq, start_s, duration_s);
                      they are handed in, not registered
          "sessions"  {channel: {"meta": the <channel>_session.json object or null,
                                 "prefix": signal of <channel>_session.16k.pcm or null,
                                 "raw": signal of <channel>_session.pcm or null,
                                 "wav": frames of a complete <channel>_session.wav (a 16 kHz
                                        saw) or null}}
          "register_failures"  the first n registrations raise RuntimeError
        Registration is register_chunk_if_missing on (channel, seq). Result: the count it
        returns, the rows it appended (file names, not paths; the WAV's frame count and
        SHA-256), and the files left in the folder.
    mark_ended_run(meeting)
        _mark_ended with now_iso() pinned to NOW_ISO: the fields update_meeting received.
    is_session_dead_at(meeting, alive)
        is_session_dead with "now" pinned to NOW_UTC in a UTC zone (seconds_since reads a naive
        value as UTC wall time) and _pid_alive answering True for the pids in `alive` (after its
        own falsy and <= 0 checks).
"""

import hashlib
import json
import os
import shutil
import tempfile
import wave
from datetime import datetime, timezone
from pathlib import Path

import numpy as np

import ow_isolation

NOW_ISO = "2026-01-01T12:00:00.123456Z"
NOW_UTC = datetime(2026, 1, 1, 12, 0, 0, 123456, tzinfo=timezone.utc)


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


class _Repo:
    def __init__(self, failures):
        self.failures = failures
        self.rows = {}

    def register_chunk_if_missing(self, **fields):
        if self.failures > 0:
            self.failures -= 1
            raise RuntimeError("database is locked")
        key = (fields["channel"], fields["seq"])
        if key in self.rows:
            return self.rows[key], False
        self.rows[key] = len(self.rows) + 1
        return self.rows[key], True


def _sha(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def _recover_pcm_tails_run(module, spec):
    scratch = ow_isolation.SCRATCH / "recovery-runs"
    scratch.mkdir(parents=True, exist_ok=True)
    folder = Path(tempfile.mkdtemp(dir=scratch))
    try:
        for channel, session in spec.get("sessions", {}).items():
            if session.get("meta") is not None:
                (folder / f"{channel}_session.json").write_text(
                    json.dumps(session["meta"]), encoding="utf-8")
            if session.get("prefix") is not None:
                (folder / f"{channel}_session.16k.pcm").write_bytes(
                    _signal(session["prefix"]).tobytes())
            if session.get("raw") is not None:
                (folder / f"{channel}_session.pcm").write_bytes(_signal(session["raw"]).tobytes())
            if session.get("wav") is not None:
                with wave.open(str(folder / f"{channel}_session.wav"), "wb") as handle:
                    handle.setnchannels(1)
                    handle.setsampwidth(2)
                    handle.setframerate(16000)
                    handle.writeframes(_signal([["saw", session["wav"], 3000, 641]]).tobytes())
        chunks = [dict(row) for row in spec.get("chunks", [])]
        given = len(chunks)
        repo = _Repo(spec.get("register_failures", 0))
        meeting = {"id": "m_gold", "spool_dir": str(folder)}
        recovered = module._recover_pcm_tails(repo, meeting, chunks)
        rows = []
        for row in chunks[given:]:
            path = row["file_path"]
            with wave.open(path, "rb") as handle:
                frames = handle.getnframes()
            rows.append({
                "channel": row["channel"], "seq": row["seq"],
                "file": os.path.basename(path), "start_s": row["start_s"],
                "duration_s": row["duration_s"], "sample_rate": row["sample_rate"],
                "asr_status": row["asr_status"], "frames": frames, "sha256": _sha(path),
            })
        return {
            "recovered": recovered,
            "rows": rows,
            "files": sorted(path.name for path in folder.iterdir()),
        }
    finally:
        shutil.rmtree(folder, ignore_errors=True)


class _UpdateRepo:
    def __init__(self):
        self.updates = []

    def update_meeting(self, meeting_id, **fields):
        self.updates.append([meeting_id, fields])


def _mark_ended_run(module, meeting):
    repo = _UpdateRepo()
    saved = module.now_iso
    module.now_iso = lambda: NOW_ISO
    try:
        module._mark_ended(repo, meeting)
    finally:
        module.now_iso = saved
    return repo.updates


def _is_session_dead_at(module, meeting, alive):
    from meeting import time_utils

    def seconds_since(value):
        parsed = time_utils.parse_meeting_time(value)
        if parsed is None:
            return None
        if parsed.tzinfo is None:
            return (NOW_UTC.replace(tzinfo=None) - parsed).total_seconds()
        return (NOW_UTC - parsed).total_seconds()

    def pid_alive(pid):
        if not pid or int(pid) <= 0:
            return False
        return int(pid) in alive

    saved = (module.seconds_since, module._pid_alive)
    module.seconds_since = seconds_since
    module._pid_alive = pid_alive
    try:
        return module.is_session_dead(meeting)
    finally:
        module.seconds_since, module._pid_alive = saved


def _saw(frames, amp=6000, step=641):
    return ["saw", frames, amp, step]


def _meta(rate, count, origin, **extra):
    meta = {"sample_rate": rate, "sample_count": count, "origin_s": origin}
    meta.update(extra)
    return meta


def _row(channel, seq, start, duration):
    return {"channel": channel, "seq": seq, "start_s": start, "duration_s": duration}


_TAILS = [
    # A native tail after the last chunk, at each capture rate.
    {"chunks": [_row("mic", 0, 0.0, 1.0)],
     "sessions": {"mic": {"meta": _meta(rate, 2 * rate, 0.0, pcm_origin_s=0.0),
                          "raw": [_saw(2 * rate)]}}}
    for rate in (16000, 44100, 48000)
] + [
    # Longer than RECOVERED_CHUNK_MAX_S: cut into 20 s pieces.
    {"sessions": {"loopback": {"meta": _meta(16000, 0, 3.25, pcm_origin_s=3.25),
                               "raw": [_saw(16000 * 45 + 7, 9000, 997)]}}},
    {"sessions": {"mic": {"meta": _meta(22050, 0, 0.5),
                          "raw": [_saw(22050 * 21, 9000, 997)]}}},
    # Coverage already reaches the end (within half a sample).
    {"chunks": [_row("mic", 4, 0.0, 2.0)],
     "sessions": {"mic": {"meta": _meta(16000, 0, 0.0, pcm_origin_s=0.0),
                          "raw": [_saw(32000)]}}},
    {"chunks": [_row("mic", 0, 0.0, 1.99997)],
     "sessions": {"mic": {"meta": _meta(16000, 0, 0.0, pcm_origin_s=0.0),
                          "raw": [_saw(32000)]}}},
    {"chunks": [_row("mic", 0, 0.0, 1.9999)],
     "sessions": {"mic": {"meta": _meta(16000, 0, 0.0, pcm_origin_s=0.0),
                          "raw": [_saw(32000)]}}},
    # The cursor sample rounds half up: floor(x * rate + 0.5).
    {"chunks": [_row("mic", 0, 0.0, 1.00003125)],
     "sessions": {"mic": {"meta": _meta(16000, 0, 0.0, pcm_origin_s=0.0),
                          "raw": [_saw(32000)]}}},
    {"chunks": [_row("mic", 2, 0.25, 0.3), _row("mic", 1, 0.0, 0.2)],
     "sessions": {"mic": {"meta": _meta(48000, 0, 0.1, pcm_origin_s=0.1),
                          "raw": [_saw(48000)]}}},
    # A prefix and an active native suffix placed by pcm_origin_s; the converted raw left
    # inactive; a legacy meta with a prefix leaves the raw file alone; one without uses origin_s.
    {"sessions": {"mic": {"meta": _meta(48000, 0, 1.0, pcm_origin_s=2.0, prefix_16k_samples=16000),
                          "prefix": [_saw(16000)], "raw": [_saw(48000 * 2)]}}},
    {"sessions": {"mic": {"meta": _meta(16000, 0, 0.0, pcm_origin_s=0.0,
                                        prefix_16k_samples=16000, pcm_active=False),
                          "prefix": [_saw(16000)], "raw": [_saw(16000)]}}},
    {"sessions": {"mic": {"meta": _meta(44100, 0, 0.0), "prefix": [_saw(8000)],
                          "raw": [_saw(44100)]}}},
    {"sessions": {"mic": {"meta": _meta(44100, 0, 2.5), "raw": [_saw(44100)]}}},
    # A complete session WAV means the PCM is not a tail; no meta or no rate means nothing.
    {"sessions": {"mic": {"meta": _meta(16000, 0, 0.0, pcm_origin_s=0.0), "raw": [_saw(16000)],
                          "wav": 16000}}},
    {"sessions": {"mic": {"meta": _meta(16000, 0, 0.0, pcm_origin_s=0.0), "raw": [_saw(16000)],
                          "wav": 0}}},
    {"sessions": {"mic": {"meta": None, "raw": [_saw(16000)]}}},
    {"sessions": {"mic": {"meta": _meta(0, 0, 0.0), "raw": [_saw(16000)]}}},
    {"sessions": {"mic": {"meta": {"sample_rate": "x"}, "raw": [_saw(16000)]}}},
    # A chunk channel outside CHANNELS joins the scan; an empty channel does not.
    {"chunks": [_row("extra", 0, 0.0, 0.5), _row("", 3, 0.0, 9.0)],
     "sessions": {"extra": {"meta": _meta(16000, 0, 0.0, pcm_origin_s=0.0),
                            "raw": [_saw(16000)]},
                  "loopback": {"meta": _meta(16000, 0, 0.0, pcm_origin_s=0.0),
                               "raw": [_saw(8000)]}}},
    # A failed registration stops that channel only.
    {"register_failures": 1,
     "sessions": {"mic": {"meta": _meta(16000, 0, 0.0, pcm_origin_s=0.0),
                          "raw": [_saw(16000 * 25)]},
                  "loopback": {"meta": _meta(16000, 0, 0.0, pcm_origin_s=0.0),
                               "raw": [_saw(16000)]}}},
    {"register_failures": 2,
     "sessions": {"loopback": {"meta": _meta(16000, 0, 0.0, pcm_origin_s=0.0),
                               "raw": [_saw(16000 * 45)]}}},
    # A one-sample tail is still a chunk.
    {"sessions": {"mic": {"meta": _meta(16000, 0, 0.0, pcm_origin_s=0.0),
                          "raw": [_saw(1)]}}},
]


def _state(**fields):
    state = {"meeting_id": "m_gold", "seq": 3, "status": "active", "cloud_enabled": True,
             "title": "Café sync", "unknown_key": {"kept": [1, 2]},
             "capture": {"mic_available": True, "loopback_available": True, "message": "",
                         "audio_incomplete": False, "integrity_error": ""}}
    state.update(fields)
    return json.dumps(state, ensure_ascii=False)


_MEETINGS = [
    {"id": "m_gold"},
    {"id": "m_gold", "ended_at": "2026-01-01T10:00:00Z"},
    {"id": "m_gold", "ended_at": ""},
    {"id": "m_gold", "state_json": ""},
    {"id": "m_gold", "state_json": "not json"},
    {"id": "m_gold", "state_json": "[1, 2]"},
    {"id": "m_gold", "cloud_enabled": True,
     "state_json": _state(finalization={"status": "running", "message": "Preparing…"})},
    {"id": "m_gold", "cloud_enabled": False,
     "state_json": _state(finalization={"status": "running", "message": "Preparing…"})},
    {"id": "m_gold", "state_json": _state(finalization={"status": "pending"})},
    {"id": "m_gold", "state_json": _state(cloud_enabled=False,
                                          finalization={"status": "pending"})},
    {"id": "m_gold", "cloud_enabled": None, "state_json": _state()},
    {"id": "m_gold", "state_json": _state(finalization={"status": "completed",
                                                        "message": "Final insights are ready.",
                                                        "steps": [{"id": "finalize"}]})},
    {"id": "m_gold", "state_json": _state(finalization={"status": "running",
                                                        "steps": [{"id": "polish",
                                                                   "status": "running"}],
                                                        "card_deferred": True})},
    {"id": "m_gold", "status": "needs_recovery",
     "state_json": _state(capture={"audio_incomplete": True,
                                   "integrity_error": "mic audio blocks were dropped"})},
    {"id": "m_gold", "state_json": _state(capture={"audio_incomplete": 0})},
    {"id": "m_gold", "state_json": _state(capture="broken")},
    {"id": "m_gold", "ended_at": "2026-01-01T10:00:00Z",
     "state_json": _state(status="paused", finalization="nonsense")},
]

_NOW_NAIVE = "2026-01-01T12:00:00"
_HEARTBEATS = [
    {},
    {"app_heartbeat_at": None, "app_pid": 10},
    {"app_heartbeat_at": "", "app_pid": 10},
    {"app_heartbeat_at": "not-a-timestamp", "app_pid": 10},
    {"app_heartbeat_at": "2026-01-01T11:59:55", "app_pid": 10},
    {"app_heartbeat_at": "2026-01-01T11:59:55", "app_pid": 11},
    {"app_heartbeat_at": "2026-01-01T11:59:55", "app_pid": None},
    {"app_heartbeat_at": "2026-01-01T11:59:55", "app_pid": 0},
    {"app_heartbeat_at": "2026-01-01T11:59:55", "app_pid": -10},
    {"app_heartbeat_at": "2026-01-01T11:59:55", "app_pid": "10"},
    {"app_heartbeat_at": "2026-01-01T11:59:55Z", "app_pid": 10},
    {"app_heartbeat_at": "2026-01-01T11:59:00.123457", "app_pid": 10},
    {"app_heartbeat_at": "2026-01-01T11:59:00.123456", "app_pid": 10},
    {"app_heartbeat_at": "2026-01-01T11:58:00Z", "app_pid": 10},
    {"app_heartbeat_at": "2026-01-01T13:00:00+01:00", "app_pid": 10},
    {"app_heartbeat_at": "2026-01-01T12:05:00Z", "app_pid": 10},
    {"app_heartbeat_at": 5, "app_pid": 10},
]

CASES = {
    "recover_pcm_tails_run": [[spec] for spec in _TAILS],
    "mark_ended_run": [[meeting] for meeting in _MEETINGS],
    "is_session_dead_at": [[meeting, [10]] for meeting in _HEARTBEATS],
}

WRAPPERS = {
    "recover_pcm_tails_run": _recover_pcm_tails_run,
    "mark_ended_run": _mark_ended_run,
    "is_session_dead_at": _is_session_dead_at,
}

PINS = {
    "now_iso": NOW_ISO,
    "now_utc": NOW_UTC.isoformat(),
    "zone": "UTC (seconds_since reads naive values as UTC wall time)",
    "alive_pids": "_pid_alive(pid) is True only for the listed pids",
}
