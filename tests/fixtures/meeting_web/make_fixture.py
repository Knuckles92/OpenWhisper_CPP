"""Write tests/fixtures/meeting_web/meetings.db through OpenWhisper's own meeting repository.

    $env:OPENWHISPER_CPP_OW_ROOT = 'D:\\coding\\whisper_local'
    & $env:OPENWHISPER_CPP_OW_PYTHON -I -B tools/parity/check_isolation.py -- `
        $env:OPENWHISPER_CPP_OW_PYTHON -I -B tests/fixtures/meeting_web/make_fixture.py

The dashboard API reads this database: tools/parity/transcripts/meeting_api.py records
OpenWhisper's answers over a copy of it and tests/meeting_web replays them against the C++
routes. Rows go in through SqlMeetingRepository (OpenWhisper's schema and column types); the
current meeting's audit events come from a write-through state store, with ids and time pinned as
the transcript pins them (new_id counts from 1, now is NOW_UTC). Every time is naive local text,
so exports print the same dates whatever the zone of the computer that replays them.
"""
from __future__ import annotations

import json
import shutil
import sqlite3
import sys
from datetime import datetime, timezone
from pathlib import Path

HERE = Path(__file__).resolve().parent
REPO = HERE.parents[2]
sys.path.insert(0, str(REPO / "tools" / "parity"))
import ow_isolation  # noqa: E402

OUTPUT = HERE / "meetings.db"
NOW_UTC = datetime(2026, 9, 19, 9, 30, tzinfo=timezone.utc)

HOST_TOKEN = "host-api-token-eeeeeeeeeeeeeeeeeeee"
GUEST_TOKEN = "guest-api-token-ffffffffffffffffffff"
CURRENT = "m_current"


class _Pinned(datetime):
    @classmethod
    def now(cls, tz=None):
        return NOW_UTC if tz is not None else NOW_UTC.replace(tzinfo=None)


class _Uuid:
    def __init__(self, n):
        self.hex = f"{n:012x}" + "0" * 20


class _FixedUuid:
    def __init__(self):
        self.next = 1

    def uuid4(self):
        value = _Uuid(self.next)
        self.next += 1
        return value


def _participants(*people):
    return {pid: {"id": pid, "display_name": name, "kind": kind,
                  "created_at": f"2026-09-18T10:0{i}:00Z"}
            for i, (pid, name, kind) in enumerate(people)}


def past_state() -> dict:
    return {
        "meeting_id": "m_past", "title": "Vendor renewal", "status": "ended",
        "cloud_enabled": True,
        "topic": {"current": "Renewal terms", "history": []},
        "rolling_summary": "We hold at twelve percent.",
        "participants": _participants(("p_guest", "Priya", "guest"), ("p_me", "Me", "me")),
        "cards": {
            "decisions": [
                {"id": "it_d1", "card": "decisions", "text": "Hold at twelve percent",
                 "status": "confirmed", "evidence": ["sg_p2"]},
                {"id": "it_d2", "card": "decisions", "text": "Dropped idea", "status": "removed"},
            ],
            "action_items": [
                {"id": "it_a1", "card": "action_items", "text": "Draft the renewal note",
                 "status": "proposed", "data": {"owner": "Priya", "deadline": "Friday"}},
            ],
            "risks": [{"id": "it_r1", "card": "risks", "text": "Vendor walks away",
                       "status": "proposed", "data": {"severity": "high"}}],
        },
        "questions": [
            {"id": "q_1", "text": "Who signs?", "status": "open"},
            {"id": "q_2", "text": "When?", "status": "resolved", "answer": "Friday"},
        ],
        "finalization": {"status": "completed", "message": "Final insights are ready."},
        "custom_reports": [
            {"id": "r_ready", "request": "the commitments", "status": "ready",
             "title": "Brief", "markdown": "# Brief\n\nHeld at twelve.", "run_id": "run_1",
             "sources": {"transcript_lines": 4}, "requested_by": None,
             "created_at": "2026-09-18T11:00:00Z", "updated_at": "2026-09-18T11:01:00Z"},
            {"id": "r_spare", "request": "the risks", "status": "ready", "title": "Risks",
             "markdown": "# Risks\n\nThe vendor may walk.", "run_id": "run_2",
             "sources": {}, "requested_by": None,
             "created_at": "2026-09-18T11:02:00Z", "updated_at": "2026-09-18T11:03:00Z"},
            {"id": "r_dead", "request": "x", "status": "running", "title": "", "markdown": "",
             "message": "", "run_id": "run_3", "sources": {}, "requested_by": None,
             "created_at": "2026-09-18T11:04:00Z", "updated_at": "2026-09-18T11:04:00Z"},
        ],
    }


def meetings() -> list[dict]:
    """Every row but the current meeting, newest first."""
    ended = {"status": "ended", "host_token": "old-host", "guest_token": "old-guest",
             "spool_dir": "", "cloud_enabled": False}
    return [
        dict(ended, id="m_past", title="Vendor renewal", started_at="2026-09-18T10:00:00",
             ended_at="2026-09-18T10:50:00", paused_total_s=180.0, cloud_enabled=True,
             asr_model="parakeet-tdt-0.6b-v3", agent_provider="openrouter",
             agent_model="test-model", state_json=json.dumps(past_state()), state_seq=9),
        dict(ended, id="m_digest", title="Decisions only", started_at="2026-09-17T10:00:00",
             ended_at="2026-09-17T10:30:00",
             state_json=json.dumps({"cards": {"decisions": [
                 {"id": "d", "card": "decisions", "text": "Ship", "status": "confirmed"}]}})),
        dict(ended, id="m_literal", title="Literal 100%_exact", started_at="2026-09-16T10:00:00",
             ended_at="2026-09-16T10:10:00", state_json="{}"),
        dict(ended, id="m_like", title="Literal 100ZZexact", started_at="2026-09-15T10:00:00",
             ended_at="2026-09-15T10:10:00", state_json="{}"),
        dict(ended, id="m_attention", title="Saved for later", started_at="2026-09-14T10:00:00",
             ended_at="2026-09-14T11:00:00", cloud_enabled=True,
             state_json=json.dumps({"meeting_id": "m_attention", "status": "ended",
                                    "cloud_enabled": True,
                                    "finalization": {"status": "failed", "message": "interrupted",
                                                     "card_deferred": True}})),
        dict(ended, id="m_untitled", title="", started_at="2026-09-13T10:00:00",
             ended_at="2026-09-13T10:05:00",
             state_json=json.dumps({"topic": {"current": "Quarterly roadmap"}})),
        dict(ended, id="m_corrupt", title="Broken", started_at="not a date",
             state_json="{not json"),
        dict(ended, id="m_busy", title="Busy elsewhere", started_at="2026-09-12T10:00:00",
             ended_at="2026-09-12T10:30:00", state_json="{}"),
        dict(ended, id="m_novalue", title="No transcript", started_at="2026-09-11T10:00:00",
             ended_at="2026-09-11T10:30:00", state_json="{}"),
        dict(ended, id="m_delete", title="Delete me", started_at="2026-09-10T10:00:00",
             ended_at="2026-09-10T10:30:00", state_json="{}"),
        dict(ended, id="m_old", title="Older \u00e9t\u00e9 \u65e5", started_at="2026-08-01T10:00:00",
             ended_at="2026-08-01T10:30:00", state_json="{}"),
    ]


def build(path: Path) -> None:
    import meeting.state.schema as schema
    import meeting.time_utils as time_utils
    from meeting.interfaces import TranscriptSegment
    from meeting.persist.repository import SqlMeetingRepository
    from meeting.stored import open_store
    from services.database import DatabaseManager

    def segment(meeting_id, seg_id, start, text, channel="mic", chunk_id=None):
        return TranscriptSegment(segment_id=seg_id, meeting_id=meeting_id, chunk_id=chunk_id,
                                 channel=channel, start_s=start, end_s=start + 2.0, text=text)

    saved = schema.uuid, time_utils.datetime
    schema.uuid, time_utils.datetime = _FixedUuid(), _Pinned
    db = DatabaseManager(str(path))
    try:
        repo = SqlMeetingRepository(db=db)
        for row in meetings():
            repo.create_meeting(**row)
        repo.create_meeting(id=CURRENT, title="Weekly sync", status="active",
                            started_at="2026-09-19T09:00:00", host_token=HOST_TOKEN,
                            guest_token=GUEST_TOKEN, cloud_enabled=False, spool_dir="",
                            asr_model="parakeet-tdt-0.6b-v3",
                            state_json=json.dumps({"meeting_id": CURRENT, "title": "Weekly sync",
                                                   "status": "active"}))
        chunk = repo.register_chunk(meeting_id="m_past", channel="loopback", seq=0,
                                    file_path="missing-loopback.wav", start_s=0.0,
                                    duration_s=30.0, sample_rate=16000)
        repo.add_segments([
            segment("m_past", "sg_p1", 0.0, "Lets talk about the vendor renewal."),
            segment("m_past", "sg_p2", 6.0, "We hold at twelve percent.", "loopback", chunk),
            segment("m_past", "sg_p3", 14.0, "Priya drafts the renewal note by Friday."),
            segment(CURRENT, "sg_c1", 1.0, "Good morning"),
            segment(CURRENT, "sg_c2", 1.0, "Same start, later id"),
            segment(CURRENT, "sg_c3", 2.5, "caf\u00e9 \u65e5\u672c renewal", "loopback"),
            segment(CURRENT, "sg_c4", 4.0, "Budget review"),
            segment(CURRENT, "sg_c5", 6.0, "Wrap up"),
        ])
        store = open_store(repo, CURRENT, repo.get_meeting(CURRENT), historical=False)
        store.apply("system", None, [{"op": "upsert_participant", "display_name": "Me",
                                      "kind": "me", "is_provisional": False}])
        store.apply("host", None, [
            {"op": "add_item", "card": "decisions", "text": "Keep the weekly slot"},
            {"op": "set_topic", "text": "Weekly planning"},
        ])
        store.apply("agent", "agent", [
            {"op": "add_item", "card": "action_items", "text": "Send the agenda",
             "evidence": ["sg_c4"]},
        ])
    finally:
        db.close()
        schema.uuid, time_utils.datetime = saved


def main() -> int:
    checkout = ow_isolation.checkout()
    ow_isolation.check_reference_commit(checkout)
    data = ow_isolation.isolate()
    ow_isolation.after_import_guards(data)
    work = ow_isolation.SCRATCH / "meeting-web-fixture"
    shutil.rmtree(work, ignore_errors=True)
    work.mkdir(parents=True)
    path = work / "meetings.db"
    build(path)
    # One file: fold any write-ahead log into the database before copying it.
    with sqlite3.connect(path) as conn:
        conn.execute("PRAGMA wal_checkpoint(TRUNCATE)")
    shutil.copyfile(path, OUTPUT)
    print(f"{OUTPUT.relative_to(REPO)}: {OUTPUT.stat().st_size} bytes")
    return 0


if __name__ == "__main__":
    sys.exit(main())
