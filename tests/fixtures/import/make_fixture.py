"""Writes tests/fixtures/import: two OpenWhisper data folders made by OpenWhisper's own
services.database, services.settings and services.backup_startup, for the import tests.

    $env:OPENWHISPER_CPP_OW_ROOT = 'D:\\coding\\whisper_local'
    & $env:OPENWHISPER_CPP_OW_PYTHON -I -B tools/parity/check_isolation.py -- `
        $env:OPENWHISPER_CPP_OW_PYTHON -I -B tests/fixtures/import/make_fixture.py

OpenWhisper is imported through tools/parity/ow_isolation.py; the folders are built under
out/parity/import-fixture/ (git-ignored) and then copied here, replacing what was here.

- openwhisper/: a v18 database (DatabaseManager) with three history entries (audio below
  recordings/ by a relative name, audio by an absolute path, no audio) and an ended meeting with
  a chunk and a segment; settings (SettingsManager) with a dropped engine as selected_model, a
  key OpenWhisper_CPP does not know, MCP turned on, a saved action, a webhook, a custom text endpoint
  with its own key variable and a URL with credentials; recordings, meetings, captures,
  scratchpad.txt, recorded_audio.wav, a remote_records folder and a log file (neither is
  imported), and the data lease's lock file (DataLease).
- legacy/: a database whose history table is empty and a transcription_history.json with two
  entries beside it (merged by the import, never staged), no settings, and the lock file.
- fixture.json: the absolute path the databases' meeting and history paths start with. The
  folders were not built there: a test copies a folder into a temporary one and rewrites that
  prefix to the copy's path, as if OpenWhisper had run there.
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

PLACEHOLDER = "C:\\OpenWhisperFixture"


def placeholder(*parts: str) -> str:
    return "\\".join((PLACEHOLDER, *parts))


def take_lease(root: Path) -> None:
    from services.backup_startup import DataLease

    DataLease.acquire(str(root)).close()


def build_full(root: Path) -> None:
    from meeting.persist.repository import SqlMeetingRepository
    from services.database import DatabaseManager
    from services.models import MeetingAudioChunk, MeetingSegment
    from services.settings import SettingsManager

    (root / "recordings").mkdir(parents=True)
    (root / "recordings" / "dictation.wav").write_bytes(b"RIFFrelative" + bytes(48))
    (root / "recordings" / "absolute.wav").write_bytes(b"RIFFabsolute" + bytes(40))
    spool = root / "meetings" / "meeting-ended"
    spool.mkdir(parents=True)
    (spool / "mic-000.wav").write_bytes(b"RIFFmeeting" + bytes(32))
    capture = root / "captures" / "2026-10-21_14-32-05"
    capture.mkdir(parents=True)
    (capture / "shot-1.png").write_bytes(b"\x89PNG fixture")
    (root / "scratchpad.txt").write_text("Groceries:\n- oat milk\n", encoding="utf-8")
    (root / "recorded_audio.wav").write_bytes(b"RIFFlast-recording")
    (root / "remote_records" / "host-1").mkdir(parents=True)
    (root / "remote_records" / "host-1" / "record.json").write_text("{}", encoding="utf-8")
    (root / "openwhisper.log").write_text("log line\n", encoding="utf-8")

    SettingsManager(settings_file=str(root / "openwhisper_settings.json")).update_settings({
        "selected_model": "local_whisper",
        "ui_theme": "dark",
        "fixture_future_key": {"kept": True},
        "mcp_enabled": True,
        "actions": [{
            "id": "a1b2c3d4e5f6", "name": "Post it", "kind": "webhook",
            "params": {"url": "https://hooks.example.com/post", "output": "none"},
            "hotkey": "", "shaping": "raw", "spoken_names": [], "description": "",
            "enabled": True,
        }],
        "automation_webhooks": [{
            "id": " 0f1e2d3c4b5a ", "name": "Notify", "url": "https://hooks.example.com/notify",
            "events": ["transcription.completed"], "enabled": True,
        }],
        "text_llm_profiles": [{
            "id": "custom-1234abcd", "name": "Mine", "kind": "custom",
            "base_url": "http://localhost:9000/v1", "api_key_env": "MY_LLM_KEY",
        }],
        "transcript_cleanup_base_url": "https://user:pw@api.example.com/v1?key=abc",
    })

    database = DatabaseManager(db_path=str(root / "openwhisper.db"))
    try:
        database.add_history_entry("history-relative", "relative audio", "2026-10-04T00:00:00Z",
                                   "local", audio_file="dictation.wav")
        database.add_history_entry("history-absolute", "absolute audio", "2026-10-04T00:01:00Z",
                                   "local",
                                   audio_file=placeholder("recordings", "absolute.wav"))
        database.add_history_entry("history-text", "no audio", "2026-10-04T00:02:00Z", "local")
        repository = SqlMeetingRepository(database)
        repository.create_meeting(id="meeting-ended", title="Ended meeting", status="ended",
                                  started_at="2026-10-04T00:00:00Z",
                                  ended_at="2026-10-04T00:01:00Z", host_token="old-host",
                                  guest_token="old-guest",
                                  spool_dir=placeholder("meetings", "meeting-ended"))
        with database.get_session() as session:
            chunk = MeetingAudioChunk(meeting_id="meeting-ended", channel="mic", seq=0,
                                      file_path=placeholder("meetings", "meeting-ended",
                                                            "mic-000.wav"),
                                      start_s=0.0, duration_s=1.0, sample_rate=16000)
            session.add(chunk)
            session.flush()
            session.add(MeetingSegment(id="segment-ended", meeting_id="meeting-ended",
                                       chunk_id=chunk.id, channel="mic", start_s=0.0, end_s=1.0,
                                       text="hello", created_at="2026-10-04T00:00:00Z"))
    finally:
        database.close()
    take_lease(root)


def build_legacy(root: Path) -> None:
    from services.database import DatabaseManager

    root.mkdir(parents=True)
    DatabaseManager(db_path=str(root / "openwhisper.db")).close()
    (root / "transcription_history.json").write_text(json.dumps({"entries": [
        {"id": "legacy-1", "text": "from the JSON file", "timestamp": "2024-01-02T03:04:05",
         "model": "whisper"},
        {"id": "legacy-2", "text": "second", "timestamp": "2024-01-02T03:05:00",
         "model": "whisper", "transcription_time": 1.5},
    ]}), encoding="utf-8")
    take_lease(root)


def main() -> None:
    checkout = ow_isolation.checkout()
    ow_isolation.check_reference_commit(checkout)
    data = ow_isolation.isolate()
    ow_isolation.after_import_guards(data)

    work = ow_isolation.SCRATCH / "import-fixture"
    shutil.rmtree(work, ignore_errors=True)
    build_full(work / "openwhisper")
    build_legacy(work / "legacy")
    for name in ("openwhisper", "legacy"):
        target = HERE / name
        shutil.rmtree(target, ignore_errors=True)
        shutil.copytree(work / name, target)
    (HERE / "fixture.json").write_bytes(
        (json.dumps({"placeholder": PLACEHOLDER}, indent=1) + "\n").encode("utf-8"))
    print("Wrote", HERE)


if __name__ == "__main__":
    main()
