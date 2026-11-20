"""The paired-computer queries of meeting/persist/repository.py (M9.4 remote-origins).

There is no module of this name: M7.7's meeting.persist.repository.py owns the vectors of the
module's pure helpers, so the case file registers an empty placeholder under this name, which
golden_vectors.py then "imports", and every function is a wrapper over the real
SqlMeetingRepository.

Every case starts from a fresh database (out/parity/repository-origin/case-<n>.db) holding
`setup`: a list of meetings, each {"session": {...}, "chunks": [...], "segments": [...],
"participants": [...], "state_items": [...], "questions": [...], "events": [...]} of full rows
(every column given, ids included), written through OpenWhisper's ORM models in that order.
Bytes, in rows and in results, are {"$b64": "<base64>"}.

    export_meeting_rows(setup, meeting_id)
    replace_meeting_rows(setup, meeting_id, session_values, rows)
        {"raised": exception class name or None, "message": str(exc) for TypeError and KeyError,
         "after": dump}, where dump is {"meetings": {id: export_meeting_rows(id)} for every
         meeting by id, "delivery": [[segment_id, meeting_id]] by seq, "fts": the segment ids
         whose text matches "budget" in the full-text index, sorted}
    record_owners(setup)
    rename_origin(setup, origin, name)             {"changed": n, "owners": record_owners()}
    origin_spools(setup, origin)                   [[meeting_id, spool_dir], ...]
    count_unfinished_chunks(setup, meeting_id)
    list_past_meeting_ids(setup, origin)           ids list_past_meeting_summaries(origin=...)
                                                   returns; origin "..." stands for Python's ...
"""

import base64
import itertools
import sys
import types
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import ow_isolation  # noqa: E402

_NAME = "meeting.persist.repository_origin"
sys.modules.setdefault(_NAME, types.ModuleType(_NAME))

WORK = ow_isolation.SCRATCH / "repository-origin"
_counter = itertools.count()
_KEYS = ("chunks", "segments", "participants", "state_items", "questions", "events")


def _models():
    from services.models import (
        MeetingAudioChunk,
        MeetingEvent,
        MeetingParticipant,
        MeetingQuestion,
        MeetingSegment,
        MeetingSession,
        MeetingStateItem,
    )

    return {"session": MeetingSession, "chunks": MeetingAudioChunk, "segments": MeetingSegment,
            "participants": MeetingParticipant, "state_items": MeetingStateItem,
            "questions": MeetingQuestion, "events": MeetingEvent}


def _from_json(value):
    if isinstance(value, dict):
        if set(value) == {"$b64"}:
            return base64.b64decode(value["$b64"])
        return {key: _from_json(item) for key, item in value.items()}
    if isinstance(value, list):
        return [_from_json(item) for item in value]
    return value


def _to_json(value):
    if isinstance(value, (bytes, bytearray, memoryview)):
        return {"$b64": base64.b64encode(bytes(value)).decode("ascii")}
    if isinstance(value, dict):
        return {key: _to_json(item) for key, item in value.items()}
    if isinstance(value, (list, tuple)):
        return [_to_json(item) for item in value]
    return value


def _repository(setup):
    from meeting.persist.repository import SqlMeetingRepository
    from services.database import DatabaseManager

    WORK.mkdir(parents=True, exist_ok=True)
    path = WORK / f"case-{next(_counter)}.db"
    for suffix in ("", "-wal", "-shm", "-journal"):
        Path(str(path) + suffix).unlink(missing_ok=True)
    database = DatabaseManager(db_path=str(path))
    models = _models()
    with database.get_session() as session:
        for meeting in setup:
            session.add(models["session"](**_from_json(meeting["session"])))
            session.flush()
            for key in _KEYS:
                for row in meeting.get(key, ()):
                    session.add(models[key](**_from_json(row)))
                    session.flush()
    return database, SqlMeetingRepository(database)


def _closing(fn):
    def wrapper(module, setup, *args):
        database, repository = _repository(setup)
        try:
            return fn(database, repository, *args)
        finally:
            database.close()
    return wrapper


def _dump(database, repository):
    from sqlalchemy import text

    with database.get_session() as session:
        ids = [row[0] for row in session.execute(
            text("SELECT id FROM meeting_sessions ORDER BY id")).all()]
        delivery = [[row[0], row[1]] for row in session.execute(text(
            "SELECT segment_id, meeting_id FROM meeting_segment_delivery ORDER BY seq")).all()]
        fts = sorted(row[0] for row in session.execute(text(
            "SELECT meeting_segments.id FROM meeting_segments_fts JOIN meeting_segments "
            "ON meeting_segments.rowid = meeting_segments_fts.rowid "
            "WHERE meeting_segments_fts MATCH 'budget'")).all())
    return {"meetings": {meeting_id: _to_json(repository.export_meeting_rows(meeting_id))
                         for meeting_id in ids},
            "delivery": delivery, "fts": fts}


@_closing
def _export(database, repository, meeting_id):
    return _to_json(repository.export_meeting_rows(meeting_id))


@_closing
def _replace(database, repository, meeting_id, session_values, rows):
    raised = message = None
    try:
        repository.replace_meeting_rows(meeting_id, _from_json(session_values), _from_json(rows))
    except Exception as exc:  # the exception type is part of the contract
        raised = type(exc).__name__
        if isinstance(exc, (TypeError, KeyError)):
            message = str(exc)
    return {"raised": raised, "message": message, "after": _dump(database, repository)}


@_closing
def _owners(database, repository):
    return repository.record_owners()


@_closing
def _rename(database, repository, origin, name):
    changed = repository.rename_origin(origin, name)
    return {"changed": changed, "owners": repository.record_owners()}


@_closing
def _spools(database, repository, origin):
    return [list(item) for item in repository.origin_spools(origin)]


@_closing
def _unfinished(database, repository, meeting_id):
    return repository.count_unfinished_chunks(meeting_id)


@_closing
def _past_ids(database, repository, origin):
    origin = ... if origin == "..." else origin
    return [meeting["id"] for meeting in repository.list_past_meeting_summaries(origin=origin)]


# ---------------------------------------------------------------- fixture rows


def _session(meeting_id, origin=None, origin_name=None, status="ended",
             started="2026-03-02T09:00:00+00:00", spool=None, state=None):
    return {
        "id": meeting_id, "title": f"Meeting {meeting_id}", "status": status,
        "started_at": started, "ended_at": None if status == "active" else started,
        "paused_total_s": 1.5, "host_token": f"h-{meeting_id}", "guest_token": f"g-{meeting_id}",
        "cloud_enabled": origin is not None, "asr_model": "parakeet-tdt-0.6b-v3",
        "asr_remote_json": None, "agent_provider": "openai", "agent_model": "gpt-x",
        "agent_endpoint_json": None,
        "spool_dir": spool if spool is not None else f"C:/meetings/{meeting_id}",
        "state_json": state if state is not None else '{"title": "Meeting %s"}' % meeting_id,
        "state_seq": 3, "app_pid": None, "app_heartbeat_at": None,
        "origin_device_id": origin, "origin_device_name": origin_name,
    }


def _chunk(chunk_id, meeting_id, channel="mic", seq=0, status="done", start=0.0):
    return {
        "id": chunk_id, "meeting_id": meeting_id, "channel": channel, "seq": seq,
        "file_path": f"C:/meetings/{meeting_id}/{channel}_{seq:05d}.wav", "start_s": start,
        "duration_s": 20.0, "sample_rate": 16000, "asr_status": status,
        "asr_attempts": 1 if status != "pending" else 0,
        "asr_error": "timed out" if status == "failed" else None,
    }


def _segment(segment_id, meeting_id, chunk_id, text, start=0.0, embedding=None,
             pinned=False, speaker=None):
    return {
        "id": segment_id, "meeting_id": meeting_id, "chunk_id": chunk_id, "channel": "mic",
        "start_s": start, "end_s": start + 2.5, "text": text,
        "speaker_participant_id": speaker, "speaker_source": "human" if pinned else "channel",
        "speaker_pinned": pinned, "embedding": embedding,
        "created_at": "2026-03-02T09:00:05+00:00",
    }


def _participant(participant_id, meeting_id, name, provisional=False):
    return {
        "id": participant_id, "meeting_id": meeting_id, "display_name": name, "kind": "me",
        "name_source": "human", "is_provisional": provisional,
        "created_at": "2026-03-02T09:00:00+00:00", "updated_at": "2026-03-02T09:01:00+00:00",
    }


def _item(item_id, meeting_id, text):
    return {
        "id": item_id, "meeting_id": meeting_id, "card": "decisions", "text": text,
        "data_json": '{"owner": "Ana"}', "status": "confirmed", "author_type": "agent",
        "author_id": "agent-1", "pinned": True, "revision": 2, "evidence_json": '["sg_a1"]',
        "created_at": "2026-03-02T09:02:00+00:00", "updated_at": "2026-03-02T09:03:00+00:00",
    }


def _question(question_id, meeting_id, text):
    return {
        "id": question_id, "meeting_id": meeting_id, "text": text, "status": "resolved",
        "answer_text": "Friday", "answer_source": "audio", "answer_confidence": 0.75,
        "suggested_answer_text": None, "suggested_confidence": None, "thread_json": "[]",
        "evidence_json": '["sg_a2"]', "asked_at": "2026-03-02T09:04:00+00:00",
        "resolved_at": "2026-03-02T09:05:00+00:00", "resolved_by": "host",
    }


def _event(event_id, meeting_id, seq, action):
    return {
        "id": event_id, "meeting_id": meeting_id, "seq": seq, "ts": "2026-03-02T09:06:00+00:00",
        "actor_type": "host", "actor_id": None, "action": action, "target_id": "it_1",
        "payload_json": '{"op": "%s"}' % action, "inverse_json": None,
    }


_EMBEDDING = {"$b64": base64.b64encode(bytes(range(16))).decode("ascii")}

_LAPTOP1 = {
    "session": _session("m_laptop1", "dev-laptop", "Kitchen laptop"),
    "chunks": [_chunk(3, "m_laptop1"), _chunk(5, "m_laptop1", "loopback", 0, "pending", 20.0)],
    "segments": [
        _segment("sg_a1", "m_laptop1", 3, "The budget is approved.", 0.0, _EMBEDDING, True,
                 "p_me"),
        _segment("sg_a2", "m_laptop1", 5, "Ship it on Friday.", 20.0),
        _segment("sg_a0", "m_laptop1", None, "Typed note about the budget.", 40.0),
    ],
    "participants": [_participant("p_me", "m_laptop1", "Ana"),
                     _participant("p_others", "m_laptop1", "Others", True)],
    "state_items": [_item("it_1", "m_laptop1", "Approve the budget")],
    "questions": [_question("q_1", "m_laptop1", "When do we ship?")],
    "events": [_event(1, "m_laptop1", 1, "add_item"), _event(2, "m_laptop1", 2, "confirm")],
}
_LAPTOP2 = {
    "session": _session("m_laptop2", "dev-laptop", None, started="2026-03-03T09:00:00+00:00"),
    "chunks": [_chunk(7, "m_laptop2", status="failed")],
    "segments": [_segment("sg_b1", "m_laptop2", 7, "Second laptop meeting.")],
}
_DESK = {"session": _session("m_desk", "dev-desk", "", started="2026-03-04T09:00:00+00:00",
                             spool="")}
_TABLET = {"session": _session("m_tablet", "dev-tablet", None,
                               started="2026-03-05T09:00:00+00:00")}
_OWN = {
    "session": _session("m_own", started="2026-03-01T09:00:00+00:00"),
    "chunks": [_chunk(1, "m_own")],
    "segments": [_segment("sg_own", "m_own", 1, "Our own budget review.")],
}
_LIVE = {"session": _session("m_live", status="active", started="2026-03-06T09:00:00+00:00"),
         "chunks": [_chunk(2, "m_live", status="processing")]}

_ALL = [_OWN, _LIVE, _LAPTOP1, _LAPTOP2, _DESK, _TABLET]


def _copy_rows(meeting, refs=True, drop_event_ids=True):
    """The rows kinds.py hands replace_meeting_rows: chunk ids as chunk_ref, no event ids."""
    rows = {}
    for key in _KEYS:
        items = []
        for row in meeting.get(key, ()):
            row = dict(row)
            if key == "chunks" and refs:
                row["chunk_ref"] = row.pop("id")
            if key == "events" and drop_event_ids:
                row.pop("id")
            items.append(row)
        rows[key] = items
    return rows


_MOVED_SESSION = dict(_LAPTOP1["session"], spool_dir="D:/kept/m_laptop1",
                      host_token="new-host", guest_token="new-guest")
_MOVED_ROWS = _copy_rows(_LAPTOP1)
# A segment of a chunk the copy lacks loses its chunk; one without a chunk keeps none.
_MOVED_ROWS["segments"].append(
    _segment("sg_a9", "m_laptop1", 99, "Orphan budget line.", 60.0))

_SHORT_ROWS = {"chunks": [dict(_copy_rows(_LAPTOP1)["chunks"][1])],
               "segments": [dict(_LAPTOP1["segments"][1])]}

_BAD_PARTICIPANT = _copy_rows(_LAPTOP1)
_BAD_PARTICIPANT["participants"] = [
    {key: value for key, value in _LAPTOP1["participants"][0].items() if key != "display_name"}]

_UNKNOWN_KEY = _copy_rows(_LAPTOP1)
_UNKNOWN_KEY["segments"] = [dict(_LAPTOP1["segments"][1], bogus=1)]

_NO_REF = _copy_rows(_LAPTOP1, refs=False)

# Values SQLAlchemy's Boolean and LargeBinary bind processors refuse at the flush.
_BAD_BOOL = _copy_rows(_LAPTOP1)
_BAD_BOOL["segments"] = [dict(_LAPTOP1["segments"][1], speaker_pinned="yes")]
_BAD_BLOB = _copy_rows(_LAPTOP1)
_BAD_BLOB["segments"] = [dict(_LAPTOP1["segments"][1], embedding="AAECAw==")]

_DEFAULTED = {
    "chunks": [{key: value for key, value in _copy_rows(_LAPTOP1)["chunks"][0].items()
                if key not in ("asr_status", "asr_attempts", "asr_error")}],
    "segments": [{key: value for key, value in _LAPTOP1["segments"][0].items()
                  if key not in ("speaker_source", "speaker_pinned", "embedding",
                                 "speaker_participant_id")}],
    "participants": [{key: value for key, value in _LAPTOP1["participants"][0].items()
                      if key not in ("name_source", "is_provisional")}],
    "state_items": [{key: value for key, value in _LAPTOP1["state_items"][0].items()
                     if key not in ("pinned", "revision", "evidence_json", "data_json")}],
    "questions": [{key: value for key, value in _LAPTOP1["questions"][0].items()
                   if key not in ("thread_json", "evidence_json", "answer_confidence")}],
    "events": [{key: value for key, value in _LAPTOP1["events"][0].items()
                if key not in ("id", "inverse_json")}],
}
_DEFAULTED_SESSION = {key: value for key, value in _LAPTOP1["session"].items()
                      if key not in ("title", "paused_total_s", "cloud_enabled", "state_seq",
                                     "ended_at", "agent_provider")}

CASES = {
    "export_meeting_rows": [
        [_ALL, "m_laptop1"],
        [_ALL, "m_own"],
        [_ALL, "m_desk"],
        [_ALL, "m_missing"],
        [[_LAPTOP1], "m_laptop1"],
    ],
    "replace_meeting_rows": [
        # Into a database that does not hold it: new chunk ids, every other meeting untouched.
        [[_OWN, _LIVE], "m_laptop1", _MOVED_SESSION, _MOVED_ROWS],
        # Over the copy already here.
        [_ALL, "m_laptop1", _MOVED_SESSION, _MOVED_ROWS],
        # A shorter copy replaces every child row of the old one.
        [_ALL, "m_laptop1", _MOVED_SESSION, _SHORT_ROWS],
        [_ALL, "m_laptop2", _LAPTOP2["session"], {}],
        # Missing columns take the ORM's defaults.
        [[_OWN], "m_laptop1", _DEFAULTED_SESSION, _DEFAULTED],
        # Bad rows roll everything back.
        [_ALL, "m_laptop1", _MOVED_SESSION, _BAD_PARTICIPANT],
        [_ALL, "m_laptop1", _MOVED_SESSION, _UNKNOWN_KEY],
        [_ALL, "m_laptop1", _MOVED_SESSION, _NO_REF],
        [_ALL, "m_laptop1", _MOVED_SESSION, _BAD_BOOL],
        [_ALL, "m_laptop1", _MOVED_SESSION, _BAD_BLOB],
        [_ALL, "m_laptop1", dict(_MOVED_SESSION, nope="x"), _MOVED_ROWS],
        [_ALL, "m_laptop1", {key: value for key, value in _MOVED_SESSION.items()
                             if key != "started_at"}, _MOVED_ROWS],
    ],
    "record_owners": [[_ALL], [[_OWN, _LIVE]], [[_LAPTOP2]], [[_TABLET, _DESK, _LAPTOP1]]],
    "rename_origin": [
        [_ALL, "dev-laptop", "Kitchen laptop (new)"],
        [_ALL, "dev-tablet", ""],
        [_ALL, "dev-unknown", "Nobody"],
    ],
    "origin_spools": [[_ALL, "dev-laptop"], [_ALL, "dev-desk"], [_ALL, "dev-unknown"]],
    "count_unfinished_chunks": [
        [_ALL, "m_laptop1"], [_ALL, "m_laptop2"], [_ALL, "m_own"], [_ALL, "m_live"],
        [_ALL, "m_missing"],
    ],
    "list_past_meeting_ids": [[_ALL, "..."], [_ALL, None], [_ALL, "dev-laptop"],
                              [_ALL, "dev-unknown"]],
}

WRAPPERS = {
    "export_meeting_rows": _export,
    "replace_meeting_rows": _replace,
    "record_owners": _owners,
    "rename_origin": _rename,
    "origin_spools": _spools,
    "count_unfinished_chunks": _unfinished,
    "list_past_meeting_ids": _past_ids,
}
