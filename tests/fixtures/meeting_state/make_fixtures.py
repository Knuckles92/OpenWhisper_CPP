"""Writes tests/fixtures/meeting_state: a meeting database made by OpenWhisper's own repository and
store, and everything OpenWhisper's repository reads back from it.

    $env:OPENWHISPER_CPP_OW_ROOT = 'D:\\coding\\whisper_local'
    & $env:OPENWHISPER_CPP_OW_PYTHON -I -B tools/parity/check_isolation.py -- `
        $env:OPENWHISPER_CPP_OW_PYTHON -I -B tests/fixtures/meeting_state/make_fixtures.py

OpenWhisper is imported through tools/parity/ow_isolation.py; the database is built under
out/parity/ (git-ignored) and only the two files below are written into this folder.

- openwhisper-meetings.db: a v18 database (DatabaseManager) holding three meetings. An ended one
  with mic and loopback chunks, committed segments, participants, a decision, a question, a
  term correction and a one-occurrence correction (so reads correct the text), a speaker
  reassignment, an undo, and the delivery and FTS rows the triggers write; a paired computer's
  meeting with non-ASCII text; and an interrupted meeting with a pending chunk. The WAL is
  checkpointed into the file.
- openwhisper-meetings.json: what SqlMeetingRepository returns for it: every meeting, its chunks,
  segments (corrected text and raw original_text), events with their inverses, delivery page,
  the Past Meetings page, the calendar times, interrupted meetings and a transcript search.
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

DATABASE = HERE / "openwhisper-meetings.db"
REPORT = HERE / "openwhisper-meetings.json"

ENDED = "m_fixture_ended"
PAIRED = "m_fixture_paired"
INTERRUPTED = "m_fixture_interrupted"


def build(path: Path) -> dict:
    from meeting.corrections import correct_text, segment_fingerprint
    from meeting.interfaces import TranscriptSegment
    from meeting.persist.repository import SqlMeetingRepository
    from meeting.stored import open_store
    from services.database import DatabaseManager

    database = DatabaseManager(db_path=str(path))
    try:
        repo = SqlMeetingRepository(database)
        repo.create_meeting(id=ENDED, title="Budget review", status="active",
                            started_at="2026-10-04T09:00:00Z", host_token="host-a",
                            guest_token="guest-a", cloud_enabled=False,
                            spool_dir="meetings/" + ENDED, asr_model="parakeet-v3")
        mic = repo.register_chunk(meeting_id=ENDED, channel="mic", seq=0,
                                  file_path="meetings/m_fixture_ended/mic-000.wav",
                                  start_s=0.0, duration_s=20.0, sample_rate=16000)
        loop = repo.register_chunk(meeting_id=ENDED, channel="loopback", seq=0,
                                   file_path="meetings/m_fixture_ended/loopback-000.wav",
                                   start_s=0.0, duration_s=20.0, sample_rate=16000)
        empty = repo.register_chunk(meeting_id=ENDED, channel="mic", seq=1,
                                    file_path="meetings/m_fixture_ended/mic-001.wav",
                                    start_s=20.0, duration_s=0.0, sample_rate=16000)

        def segment(seg_id, chunk, channel, start, text):
            return TranscriptSegment(segment_id=seg_id, meeting_id=ENDED, chunk_id=chunk,
                                     channel=channel, start_s=start, end_s=start + 2.5,
                                     text=text)

        repo.commit_chunk_transcription(ENDED, mic, [
            segment("sg_fx_mic_1", mic, "mic", 0.5, "Entropic ships the budget on Friday."),
            segment("sg_fx_mic_2", mic, "mic", 4.0, "Friday works, entropic said Friday."),
        ])
        repo.commit_chunk_transcription(ENDED, loop, [
            segment("sg_fx_loop_1", loop, "loopback", 2.0, "Who owns QA for the budget?"),
            segment("sg_fx_loop_2", loop, "loopback", 7.25, "   "),
        ])
        repo.set_chunk_status(empty, "processing")
        repo.set_chunk_status(empty, "blocked", error="Saved WAV is empty")

        store = open_store(repo, ENDED, repo.get_meeting(ENDED))
        [me] = store.apply("system", None, [
            {"op": "upsert_participant", "display_name": "Me", "kind": "me"}])
        store.apply("agent", "agent", [
            {"op": "add_item", "card": "decisions", "text": "Ship the budget on Friday",
             "evidence": ["sg_fx_mic_1"]},
            {"op": "ask_question", "text": "Who owns QA?", "evidence": ["sg_fx_loop_1"]},
            {"op": "upsert_participant", "display_name": "Speaker 1",
             "evidence": ["sg_fx_loop_1"]},
            {"op": "set_topic", "text": "Budget \u2014 Q3 caf\u00e9 plan",
             "evidence": ["sg_fx_mic_1"]},
        ])
        speaker = next(p for p in store.snapshot()["participants"].values()
                       if p["display_name"] == "Speaker 1")
        store.apply("host", me.target_id, [{
            "op": "add_item", "card": "user_notes", "text": "Correction: Entropic -> Anthropic",
            "data": {"kind": "term_correction", "selected_text": "Entropic",
                     "replacement": "Anthropic"},
        }])
        rules = {"entropic": "Anthropic"}
        raw = repo.get_segment(ENDED, "sg_fx_mic_2")["original_text"]
        store.apply("host", me.target_id, [{
            "op": "add_item", "card": "user_notes", "text": "Friday -> Thursday (second)",
            "data": {"kind": "occurrence_correction", "selected_text": "Friday",
                     "replacement": "Thursday", "occurrence_index": 1,
                     "base_fingerprint": segment_fingerprint(correct_text(raw, rules))},
            "evidence": ["sg_fx_mic_2"],
        }])
        [reassigned] = store.apply("host", me.target_id, [{
            "op": "reassign_segment_speaker", "segment_id": "sg_fx_loop_1",
            "participant_id": speaker["id"]}])
        [question] = [q for q in store.snapshot()["questions"]]
        [answered] = store.apply("user", me.target_id, [{
            "op": "answer_question", "question_id": question["id"],
            "answer_text": "Priya owns QA \u2713"}])
        store.undo(answered.seq, me.target_id)
        store.update_runtime_fields(status="ended")
        repo.update_meeting(ENDED, ended_at="2026-10-04T09:30:00Z", paused_total_s=12.5)
        repo.advance_delivery_cursor(ENDED, "cards", 2)
        del reassigned

        repo.create_meeting(id=PAIRED, title="Caf\u00e9 \u2615 sync", status="ended",
                            started_at="2026-10-03T14:00:00+02:00",
                            ended_at="2026-10-03T14:20:00+02:00", host_token="host-b",
                            guest_token="guest-b", cloud_enabled=True,
                            spool_dir="meetings/" + PAIRED, origin_device_id="device-42",
                            origin_device_name="Studio PC")
        repo.add_segments([TranscriptSegment(
            segment_id="sg_fx_paired", meeting_id=PAIRED, chunk_id=None, channel="mic",
            start_s=1.0, end_s=3.0, text="Na\u00efve r\u00e9sum\u00e9 budget \U0001F600",
            speaker_participant_id=None, speaker_source="channel", speaker_pinned=True)])

        repo.create_meeting(id=INTERRUPTED, title="", status="active",
                            started_at="2026-10-05T08:00:00", host_token="host-c",
                            guest_token="guest-c", spool_dir="meetings/" + INTERRUPTED,
                            app_pid=4242, app_heartbeat_at="2026-10-05T08:05:00Z")
        repo.register_chunk(meeting_id=INTERRUPTED, channel="mic", seq=0,
                            file_path="meetings/m_fixture_interrupted/mic-000.wav",
                            start_s=0.0, duration_s=20.0, sample_rate=16000)

        meetings = [ENDED, PAIRED, INTERRUPTED]
        report = {
            "meetings": {mid: repo.get_meeting(mid) for mid in meetings},
            "list_meetings": [row["id"] for row in repo.list_meetings()],
            "chunks": {mid: repo.get_audio_chunks(mid) for mid in meetings},
            "segments": {mid: repo.get_segments(mid) for mid in meetings},
            "events": {mid: repo.list_events(mid, limit=500) for mid in meetings},
            "event_detail": {mid: [repo.get_event(mid, event["seq"])
                                   for event in repo.list_events(mid, limit=500)]
                             for mid in meetings},
            "delivery": {mid: repo.get_segments_delivery_page(mid) for mid in meetings},
            "cursors": {"cards": repo.get_delivery_cursor(ENDED, "cards"),
                        "notes": repo.get_delivery_cursor(ENDED, "notes")},
            "past_page": repo.list_past_meeting_summaries(),
            "past_page_local": repo.list_past_meeting_summaries(origin=None),
            "past_page_query": [row["id"] for row in
                                repo.list_past_meeting_summaries(query="anthropic")],
            "past_times": [list(row) for row in repo.list_past_meeting_times()],
            "interrupted": sorted(row["id"] for row in repo.find_interrupted_meetings()),
            "search": repo.search_transcripts("budget"),
            "content_summary": repo.get_meeting_content_summary(INTERRUPTED),
        }
    finally:
        database.close()
    return report


def main() -> None:
    checkout = ow_isolation.checkout()
    ow_isolation.check_reference_commit(checkout)
    data = ow_isolation.isolate()
    ow_isolation.after_import_guards(data)

    work = ow_isolation.SCRATCH / "meeting-state-fixture"
    shutil.rmtree(work, ignore_errors=True)
    work.mkdir(parents=True)
    path = work / DATABASE.name
    report = build(path)
    # One self-contained file: the WAL goes into the database before it is copied.
    connection = sqlite3.connect(str(path))
    try:
        connection.execute("PRAGMA wal_checkpoint(TRUNCATE)")
    finally:
        connection.close()
    for suffix in ("-wal", "-shm"):
        leftover = Path(str(path) + suffix)
        if leftover.exists() and leftover.stat().st_size:
            raise SystemExit(f"{leftover.name} still holds data")
    shutil.copyfile(path, DATABASE)
    REPORT.write_bytes((json.dumps(report, indent=1, ensure_ascii=False) + "\n").encode("utf-8"))
    print(f"{DATABASE.name}: {DATABASE.stat().st_size} bytes; {REPORT.name} written")


if __name__ == "__main__":
    main()
