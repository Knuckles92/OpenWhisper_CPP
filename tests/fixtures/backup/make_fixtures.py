"""Writes tests/fixtures/backup: a bundle made by OpenWhisper's own create_backup, and what
OpenWhisper reports for it.

    $env:OPENWHISPER_CPP_OW_ROOT = 'D:\\coding\\whisper_local'
    & $env:OPENWHISPER_CPP_OW_PYTHON -I -B tools/parity/check_isolation.py -- `
        $env:OPENWHISPER_CPP_OW_PYTHON -I -B tests/fixtures/backup/make_fixtures.py

OpenWhisper is imported through tools/parity/ow_isolation.py; its data root and the source data
root built here live under out/parity/ (git-ignored), and only the two files below are written
into this folder.

- openwhisper-sample.owbackup: services.backup.create_backup of a data root holding a v18
  database (DatabaseManager) with three history entries (one with archived audio, one whose
  audio file is missing, one without audio) and an ended meeting with a chunk and a segment,
  recordings, captures, scratchpad.txt, recorded_audio.wav, and settings that keep records on a
  paired host. Python's zipfile writes every member with Zip64 extra fields (force_zip64), which
  the C++ reader must accept on small members.
- openwhisper-sample.json: the BackupInfo inspect_backup reports for it (path left out).
"""
from __future__ import annotations

import json
import shutil
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
REPO = HERE.parents[2]
sys.path.insert(0, str(REPO / "tools" / "parity"))

import ow_isolation  # noqa: E402

ARCHIVE = HERE / "openwhisper-sample.owbackup"
REPORT = HERE / "openwhisper-sample.json"


def build_source(source: Path) -> None:
    from meeting.persist.repository import SqlMeetingRepository
    from services.database import DatabaseManager
    from services.models import MeetingAudioChunk, MeetingSegment

    (source / "recordings").mkdir(parents=True)
    (source / "recordings" / "dictation.wav").write_bytes(b"RIFFhistory" + bytes(64))
    spool = source / "meetings" / "meeting-ended"
    spool.mkdir(parents=True)
    chunk_path = spool / "mic-000.wav"
    chunk_path.write_bytes(b"RIFFmeeting" + bytes(32))
    capture = source / "captures" / "2026-10-21_14-32-05"
    capture.mkdir(parents=True)
    (capture / "shot-1.png").write_bytes(b"\x89PNG fixture")
    (capture / "walkthrough.json").write_text('{"title": "Fixture"}', encoding="utf-8")
    (source / "scratchpad.txt").write_text("Groceries:\n- oat milk\n", encoding="utf-8")
    (source / "recorded_audio.wav").write_bytes(b"RIFFrecovery")
    (source / "openwhisper_settings.json").write_text(json.dumps({
        "ui_theme": "dark",
        "remote_records_location": "both",
        "mcp_enabled": True,
        "transcript_cleanup_base_url": "https://user:pw@api.example.com/v1?key=abc",
    }), encoding="utf-8")

    database = DatabaseManager(db_path=str(source / "openwhisper.db"))
    try:
        database.add_history_entry("history-audio", "dictation text", "2026-10-04T00:00:00Z",
                                   "local", audio_file="dictation.wav")
        database.add_history_entry("history-missing", "lost audio", "2026-10-04T00:01:00Z",
                                   "local", audio_file="gone.wav")
        database.add_history_entry("history-text", "no audio", "2026-10-04T00:02:00Z", "local")
        repository = SqlMeetingRepository(database)
        repository.create_meeting(id="meeting-ended", title="Ended meeting", status="ended",
                                  started_at="2026-10-04T00:00:00Z",
                                  ended_at="2026-10-04T00:01:00Z", host_token="old-host",
                                  guest_token="old-guest", spool_dir=str(spool))
        with database.get_session() as session:
            chunk = MeetingAudioChunk(meeting_id="meeting-ended", channel="mic", seq=0,
                                      file_path=str(chunk_path), start_s=0.0, duration_s=1.0,
                                      sample_rate=16000)
            session.add(chunk)
            session.flush()
            session.add(MeetingSegment(id="segment-ended", meeting_id="meeting-ended",
                                       chunk_id=chunk.id, channel="mic", start_s=0.0, end_s=1.0,
                                       text="hello", created_at="2026-10-04T00:00:00Z"))
    finally:
        database.close()


def main() -> None:
    checkout = ow_isolation.checkout()
    ow_isolation.check_reference_commit(checkout)
    data = ow_isolation.isolate()
    ow_isolation.after_import_guards(data)
    from services import backup

    work = ow_isolation.SCRATCH / "backup-fixture"
    shutil.rmtree(work, ignore_errors=True)
    source = work / "source"
    build_source(source)
    archive = work / ARCHIVE.name
    backup.create_backup(archive, data_dir=source)
    info = backup.inspect_backup(archive)
    shutil.copyfile(archive, ARCHIVE)
    report = {
        "created_at": info.created_at,
        "app_version": info.app_version,
        "schema_version": info.schema_version,
        "include_recordings": info.include_recordings,
        "file_count": info.file_count,
        "total_bytes": info.total_bytes,
        "archive_bytes": info.archive_bytes,
        "warnings": list(info.warnings),
    }
    REPORT.write_bytes((json.dumps(report, indent=1) + "\n").encode("utf-8"))
    print(json.dumps(report, indent=1))


if __name__ == "__main__":
    main()
