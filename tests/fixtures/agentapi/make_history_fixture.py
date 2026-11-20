"""Write tests/fixtures/agentapi/history.db through OpenWhisper's own database code.

    $env:OPENWHISPER_CPP_OW_ROOT = 'D:\\coding\\whisper_local'
    & $env:OPENWHISPER_CPP_OW_PYTHON -I -B tests/fixtures/agentapi/make_history_fixture.py

The History API reads this database (tools/parity/transcripts/agent_api.py records OpenWhisper's
answers over it, tests/agentapi replays them). Rows are inserted with OpenWhisper's
DatabaseManager and its ORM models, so the file has OpenWhisper's schema v18 and its column
types. Private columns (audio_file, host_token, guest_token, spool_dir, embedding,
agent_endpoint_json, cleaned_text, app_name) hold "private-marker", which no answer may show.
"""
from __future__ import annotations

import json
import shutil
import sqlite3
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
REPO = HERE.parents[2]
sys.path.insert(0, str(REPO / "tools" / "parity"))
import ow_isolation  # noqa: E402

OUTPUT = HERE / "history.db"
DATE = "2026-09-01T12:00:00+00:00"
PRIVATE = "private-marker"


def snapshot() -> dict:
    """m_a's saved state: removed and dismissed items, keys and cards the API must not show."""
    return {
        "title": "Launch review",
        "rolling_summary": "Launch plan agreed.",
        "rolling_summary_evidence": ["sg_a"],
        "participants": {
            "p_a": {"id": "p_a", "display_name": "Alex", "kind": "me", "token": PRIVATE},
            "p_b": {"id": "p_b", "display_name": "Bea"},
        },
        "cards": {
            "action_items": [
                {"id": "it_a", "text": "Ship the launch", "status": "confirmed",
                 "evidence": ["sg_a"], "pinned": True,
                 "data": {"owner_participant_id": "p_a", "due_date": "2026-09-04",
                          "private_path": PRIVATE}},
                {"id": "it_removed", "text": "Removed note", "status": "removed"},
            ],
            "decisions": [{"id": "it_d", "text": "Friday it is", "data": {"start_s": 3.5}}],
            "risks": [{"id": "it_r", "text": "Rain", "data": {"severity": "low", "heading": "Weather"}}],
            "secret_cards": [{"id": "it_secret", "text": PRIVATE}],
        },
        "questions": [
            {"id": "q_a", "text": "When?", "answer": "Friday", "status": "resolved",
             "answer_source": "transcript", "evidence": ["sg_b"]},
            {"id": "q_dismissed", "text": "Why?", "status": "dismissed"},
            {"id": "q_open", "text": "Who brings cake?"},
        ],
        "custom_reports": [
            {"id": "r_a", "title": "Plan", "markdown": "Launch Friday", "status": "ready",
             "sources": {"path": PRIVATE}},
            {"id": "r_b", "request": "Summarize risks"},
        ],
        "capture": {"path": PRIVATE},
    }


def odd_snapshot() -> dict:
    """Values pydantic's lax mode coerces: a str float, a "yes" bool, a 1 bool, a tuple-free list."""
    return {
        "rolling_summary": "Odd but readable",
        "participants": {"p_x": {"id": "p_x", "display_name": "Xan", "kind": "others_cluster"}},
        "cards": {
            "key_points": [
                {"id": "k1", "text": "Twelve", "pinned": "yes", "data": {"start_s": "12"}},
                {"id": "k2", "pinned": 1, "data": {"start_s": 7, "heading": "Seven"}},
                {"id": "k3", "pinned": "off", "data": {"start_s": True}},
            ],
            "timeline": [],
            "live_notes": [{"id": "n1", "text": "A note", "status": "proposed"}],
        },
        "questions": [],
    }


def dictations() -> list[dict]:
    rows = [
        dict(entry_id="h_a", text="The launch is Friday", raw_text="raw phrase only",
             timestamp=DATE, model="test", audio_file=PRIVATE, source_name="plan.wav",
             audio_duration=12.5, cleanup_provider="openai", cleanup_model="gpt-test"),
        dict(entry_id="h_b", text="Literal 50%_quote ' OR 1=1 -- path/like", timestamp=DATE,
             model="test"),
        dict(entry_id="h_c", text="x" * 1000 + " Needle near end",
             timestamp="2026-09-01T09:00:00-07:00", model="test"),
        dict(entry_id="h_old", text="launch before", timestamp="2026-08-31T23:00:00", model="test"),
        dict(entry_id="h_remote", text="launch remote", timestamp=DATE, model="test",
             origin_device_id="device_a", origin_device_name="Laptop"),
        dict(entry_id="h_unicode", text="x" * 900 + " ÉTÉ 東京", timestamp=DATE, model="test"),
        dict(entry_id="h_cleaned", text="hello world um", raw_text="hello world um",
             cleaned_text="Hello, world.", app_name=PRIVATE, timestamp="2026-07-01T08:00:00Z",
             model="base"),
        dict(entry_id="h_titled", text="Notes for the roadmap", timestamp="2026-07-02T08:00:00Z",
             model="base", source_name="roadmap.m4a"),
        dict(entry_id="h_source_only", text="nothing here", timestamp="2026-07-03T08:00:00Z",
             model="base", source_name="Launch memo.wav"),
        dict(entry_id="h_baddate", text="launch with no date", timestamp="not a date",
             model="base"),
        dict(entry_id="h_frac", text="fractional seconds", timestamp="2026-09-01T12:00:00.5Z",
             model="base"),
        dict(entry_id="h space é", text="an id that needs quoting", timestamp="2026-06-01T00:00:00Z",
             model="base"),
        dict(entry_id="h_long", text="é" * 330 + " launch", timestamp="2026-05-01T00:00:00Z",
             model="base", audio_duration=3.0),
        dict(entry_id="h_lower", text="été et tōkyō", timestamp="2026-05-02T00:00:00Z",
             model="base"),
        dict(entry_id="h_offset", text="offset launch", timestamp="2026-09-01T14:00:00+02:00",
             model="base"),
    ]
    # Ties: ten dictations at one instant, written in three spellings of it.
    for i in range(10):
        spelling = ("2026-08-15T10:00:00Z", "2026-08-15T10:00:00+00:00",
                    "2026-08-15T12:00:00+02:00")[i % 3]
        rows.append(dict(entry_id=f"h_tie_{i:02d}", text=f"tie number {i}", timestamp=spelling,
                         model="tie"))
    return rows


def meetings() -> list[dict]:
    return [
        dict(id="m_a", title="Launch review", status="ended", started_at=DATE, ended_at=DATE,
             state_json=json.dumps(snapshot())),
        dict(id="m_b", title="Budget sync", status="ended",
             started_at="2026-08-20T09:00:00+00:00", ended_at="2026-08-20T10:00:00+00:00",
             state_json=json.dumps({"rolling_summary": "Budget holds."})),
        dict(id="m_remote", title="Launch review", status="ended", started_at=DATE, ended_at=DATE,
             state_json=json.dumps(snapshot()), origin_device_id="device_a",
             origin_device_name="Laptop"),
        dict(id="m_empty", title="Launch review", status="ended", started_at=DATE, ended_at=DATE,
             state_json=None),
        dict(id="m_corrupt", title="Corrupt notes", status="failed",
             started_at="2026-08-01T00:00:00Z", ended_at=None, state_json="not json"),
        dict(id="m_odd", title="Odd snapshot", status="ended",
             started_at="2026-07-01T00:00:00", ended_at=None, state_json=json.dumps(odd_snapshot())),
        dict(id="m_badtype", title="Bad types", status="ended",
             started_at="2026-07-02T00:00:00Z", ended_at=None,
             state_json=json.dumps({"cards": {"decisions": [{"id": "d", "text": 5}]}})),
        dict(id="m_list", title="List state", status="ended",
             started_at="2026-07-03T00:00:00Z", ended_at=None, state_json="[]"),
        dict(id="m_active", title="Ongoing ÉTÉ meeting", status="active",
             started_at="2026-09-02T08:00:00+00:00", ended_at=None, state_json="{}"),
    ]


SEGMENTS = [
    ("sg_a", "m_a", 0, "We launch Friday"),
    ("sg_b", "m_a", 0, "We launch Friday"),
    ("sg_c", "m_a", 10, "We launch Friday"),
    ("sg_d", "m_a", 3.5, "Weather risk for the launch: 東京 rain"),
    ("sg_e", "m_b", 1.25, "Budget holds through Q4"),
    ("sg_f", "m_b", 20, "The 100% figure _ is final"),
    ("sg_remote", "m_remote", 0, "We launch Friday"),
]


def build(path: Path) -> None:
    from services.database import DatabaseManager
    from services.models import MeetingSegment, MeetingSession

    db = DatabaseManager(str(path))
    try:
        for row in dictations():
            origin = {k: row.pop(k) for k in ("origin_device_id", "origin_device_name") if k in row}
            db.add_history_entry(**row)
            if origin:
                from services.models import TranscriptionHistory

                with db.get_session() as session:
                    entry = session.get(TranscriptionHistory, row["entry_id"])
                    for name, value in origin.items():
                        setattr(entry, name, value)
        with db.get_session() as session:
            for meeting in meetings():
                session.add(MeetingSession(
                    host_token=PRIVATE, guest_token=PRIVATE, spool_dir=PRIVATE, state_seq=7,
                    agent_endpoint_json=json.dumps({"url": PRIVATE}), **meeting))
            session.flush()
            for segment_id, meeting_id, start, text in SEGMENTS:
                session.add(MeetingSegment(
                    id=segment_id, meeting_id=meeting_id, start_s=start, end_s=start + 3,
                    text=text, channel="mic", created_at=DATE, speaker_source="human",
                    speaker_participant_id="p_a", embedding=PRIVATE.encode()))
    finally:
        db.close()


def main() -> int:
    checkout = ow_isolation.checkout()
    ow_isolation.check_reference_commit(checkout)
    data = ow_isolation.isolate()
    ow_isolation.after_import_guards(data)
    work = ow_isolation.SCRATCH / "agentapi-fixture"
    shutil.rmtree(work, ignore_errors=True)
    work.mkdir(parents=True)
    path = work / "history.db"
    build(path)
    # One file: fold any write-ahead log into the database before copying it.
    with sqlite3.connect(path) as conn:
        conn.execute("PRAGMA wal_checkpoint(TRUNCATE)")
    shutil.copyfile(path, OUTPUT)
    print(f"{OUTPUT.relative_to(REPO)}: {OUTPUT.stat().st_size} bytes")
    return 0


if __name__ == "__main__":
    sys.exit(main())
