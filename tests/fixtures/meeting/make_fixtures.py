"""Writes tests/fixtures/meeting: a crashed meeting's spool written with OpenWhisper's own spool
helpers, and what OpenWhisper's recovery makes of it.

    $env:FIORIVOCE_OW_ROOT = 'D:\\coding\\whisper_local'
    & $env:FIORIVOCE_OW_PYTHON -I -B tools/parity/check_isolation.py -- `
        $env:FIORIVOCE_OW_PYTHON -I -B tests/fixtures/meeting/make_fixtures.py

OpenWhisper is imported through tools/parity/ow_isolation.py; the database and the working copy
of the spool live under out/parity/ (git-ignored), and only the files below are written here.

- crash_spool/: the spool as the crash left it. Mic: a done chunk, a pending chunk, an orphaned
  chunk (its registration failed, so the WAV is "*.orphan" beside its recovery sidecar) and 48 kHz
  native session PCM past the last chunk. Loopback: a chunk WAV whose row never committed (its
  sidecar claims a longer duration than the WAV header), a converted 16 kHz prefix and a 44.1 kHz
  native suffix placed by pcm_origin_s. Every chunk and session holds a constant level, so a
  decoder can tell the chunks apart by their first sample.
- crash.json: the meeting row and the chunk rows registered before the crash; the recoverable
  meetings the startup scan finds and the chunk rows after it; and after a headless finalize
  whose decoder answers one segment per chunk ("level <first sample>", the chunk's whole span),
  its result, the chunk rows, the segments, the meeting's status and finalization, and the
  SHA-256 of every file in the spool.
"""
from __future__ import annotations

import hashlib
import json
import shutil
import sys
import wave
from pathlib import Path
from types import SimpleNamespace

import numpy as np

HERE = Path(__file__).resolve().parent
REPO = HERE.parents[2]
sys.path.insert(0, str(REPO / "tools" / "parity"))

import ow_isolation  # noqa: E402

SPOOL = HERE / "crash_spool"
REPORT = HERE / "crash.json"
MEETING = "m_fixture_crash"


def _const(count: int, level: int) -> bytes:
    return np.full(count, level, dtype=np.int16).tobytes()


def _wav(path: Path, seconds: float, level: int, rate: int = 16000) -> None:
    with wave.open(str(path), "wb") as handle:
        handle.setnchannels(1)
        handle.setsampwidth(2)
        handle.setframerate(rate)
        handle.writeframes(_const(int(seconds * rate), level))


def write_spool(folder: Path) -> None:
    from meeting.capture.spool import write_chunk_recovery_meta, write_session_meta

    folder.mkdir(parents=True)
    _wav(folder / "mic_00000.wav", 1.0, 500)
    _wav(folder / "mic_00001.wav", 1.0, 600)
    _wav(folder / "mic_00002.wav", 1.0, 700)
    (folder / "mic_00002.wav").replace(folder / "mic_00002.wav.orphan")
    write_chunk_recovery_meta(str(folder / "mic_00002.wav.orphan"), MEETING, "mic", 2, 2.0, 1.0)
    (folder / "mic_session.pcm").write_bytes(_const(int(3.5 * 48000), 800))
    write_session_meta(str(folder / "mic_session.json"), 48000, int(3.5 * 48000), 0.0,
                       pcm_origin_s=0.0)

    _wav(folder / "loopback_00000.wav", 0.75, 900)
    write_chunk_recovery_meta(str(folder / "loopback_00000.wav"), MEETING, "loopback", 0, 0.25,
                              0.9)
    (folder / "loopback_session.16k.pcm").write_bytes(_const(16000, 1000))
    (folder / "loopback_session.pcm").write_bytes(_const(int(1.5 * 44100), 1100))
    write_session_meta(str(folder / "loopback_session.json"), 44100, int(1.5 * 44100), 0.0,
                       pcm_origin_s=1.0, prefix_16k_samples=16000)


MEETING_ROW = {
    "id": MEETING, "title": "Crashed sync", "status": "active",
    "started_at": "2026-10-04T09:00:00Z", "host_token": "host", "guest_token": "guest",
    "cloud_enabled": False, "asr_model": "parakeet-v3",
}
INITIAL_CHUNKS = [
    {"channel": "mic", "seq": 0, "file": "mic_00000.wav", "start_s": 0.0, "duration_s": 1.0,
     "sample_rate": 16000, "asr_status": "done"},
    {"channel": "mic", "seq": 1, "file": "mic_00001.wav", "start_s": 1.0, "duration_s": 1.0,
     "sample_rate": 16000, "asr_status": "pending"},
]


def _state_json() -> str:
    from meeting.state.schema import FinalizationState, MeetingState

    state = MeetingState(meeting_id=MEETING, title="Crashed sync", status="active",
                         cloud_enabled=False,
                         finalization=FinalizationState(status="pending"))
    return json.dumps(state.to_dict(), ensure_ascii=False)


def _chunk_rows(repo) -> list:
    rows = []
    for row in repo.get_audio_chunks(MEETING):
        rows.append({"channel": row["channel"], "seq": row["seq"],
                     "file": Path(row["file_path"]).name, "start_s": row["start_s"],
                     "duration_s": row["duration_s"], "sample_rate": row["sample_rate"],
                     "asr_status": row["asr_status"]})
    return rows


def _first_sample(path: str) -> int:
    with wave.open(path, "rb") as handle:
        return int(np.frombuffer(handle.readframes(1), dtype="<i2")[0])


def build(scratch: Path) -> dict:
    import meeting.asr.engine as engine_module
    from meeting.interfaces import TranscriptSegment
    from meeting.persist.repository import SqlMeetingRepository
    from meeting.recovery import finalize_meeting, find_recoverable_meetings
    from services.database import DatabaseManager

    spool = scratch / "spool"
    write_spool(spool)
    shutil.copytree(spool, SPOOL)

    database = DatabaseManager(db_path=str(scratch / "openwhisper.db"))
    real_engine = engine_module.MeetingAsrEngine
    try:
        repo = SqlMeetingRepository(database)
        repo.create_meeting(**MEETING_ROW, spool_dir=str(spool), state_json=_state_json())
        for row in INITIAL_CHUNKS:
            chunk_id = repo.register_chunk(
                meeting_id=MEETING, channel=row["channel"], seq=row["seq"],
                file_path=str(spool / row["file"]), start_s=row["start_s"],
                duration_s=row["duration_s"], sample_rate=row["sample_rate"])
            if row["asr_status"] != "pending":
                repo.set_chunk_status(chunk_id, row["asr_status"])

        recoverable = [row["id"] for row in find_recoverable_meetings(repo)]
        after_scan = _chunk_rows(repo)

        class FakeEngine:
            def __init__(self, model_name, meeting_id, repository, language=None,
                         term_rules=None, **_kwargs):
                self.meeting_id = meeting_id
                self.is_available = True
                self._cb = None

            def start(self, cb):
                self._cb = cb

            def enqueue(self, chunk):
                segment_id = real_engine._stable_segment_id(
                    SimpleNamespace(meeting_id=self.meeting_id), chunk.chunk_id, 0)
                self._cb(chunk, [TranscriptSegment(
                    segment_id=segment_id, meeting_id=self.meeting_id, chunk_id=chunk.chunk_id,
                    channel=chunk.channel, start_s=chunk.start_s,
                    end_s=chunk.start_s + chunk.duration_s,
                    text=f"level {_first_sample(chunk.file_path)}")])

            def drain(self, timeout):
                return True

            def stop(self):
                pass

        engine_module.MeetingAsrEngine = FakeEngine
        progress = []
        meeting = repo.get_meeting(MEETING)
        finalized = finalize_meeting(repo, meeting, asr_language="auto",
                                     on_progress=lambda done, total: progress.append([done,
                                                                                       total]))
        by_id = {row["id"]: row for row in repo.get_audio_chunks(MEETING)}
        segments = []
        for row in repo.get_segments(MEETING):
            chunk = by_id[row["chunk_id"]]
            segments.append({"id": row["id"], "chunk": [chunk["channel"], chunk["seq"]],
                             "channel": row["channel"], "start_s": row["start_s"],
                             "end_s": row["end_s"], "text": row["text"]})
        ended = repo.get_meeting(MEETING)
        state = json.loads(ended["state_json"])
        files = {path.name: hashlib.sha256(path.read_bytes()).hexdigest()
                 for path in sorted(spool.iterdir())}
        return {
            "meeting": MEETING_ROW,
            "state_json": _state_json(),
            "initial_chunks": INITIAL_CHUNKS,
            "recoverable": recoverable,
            "after_scan": after_scan,
            "finalized": finalized,
            "progress": progress[-1],
            "chunks": _chunk_rows(repo),
            "segments": segments,
            "status": ended["status"],
            "has_ended_at": bool(ended.get("ended_at")),
            "finalization": state["finalization"],
            "files": files,
        }
    finally:
        engine_module.MeetingAsrEngine = real_engine
        database.close()


def main() -> int:
    checkout = ow_isolation.checkout()
    ow_isolation.check_reference_commit(checkout)
    data = ow_isolation.isolate()
    ow_isolation.after_import_guards(data)
    scratch = ow_isolation.SCRATCH / "meeting-recovery-fixture"
    shutil.rmtree(scratch, ignore_errors=True)
    scratch.mkdir(parents=True)
    shutil.rmtree(SPOOL, ignore_errors=True)
    report = build(scratch)
    REPORT.write_text(json.dumps(report, indent=2, ensure_ascii=False) + "\n", encoding="utf-8",
                      newline="\n")
    print(f"wrote {SPOOL.relative_to(REPO)} and {REPORT.relative_to(REPO)}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
