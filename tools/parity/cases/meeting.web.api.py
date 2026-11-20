"""Curated inputs for meeting/web/api.py's pure helpers (M7.13 meeting-web-hub).

    _encode_cursor(segment), _decode_cursor(cursor)
    _history_cursor(meeting), _decode_history_cursor(cursor)
    _public_meeting(row)            no repository; "now" pinned (below)
    _meeting_digest(row)

The decoders raise HTTPException (status 400) for every malformed cursor; the cases include the
whole table of tests/test_meeting_web_history.py:102-142 (its 100,000 nested brackets as 10,000,
which fail the same way and keep the vector file small). "Now" is pinned to
2026-01-01T00:00:00Z (the replay's FakeClock default) by swapping meeting.time_utils.datetime,
for the running meetings' duration_s; rows use aware timestamps, so the local zone does not
matter.
"""

import base64
import json
from datetime import datetime, timezone

NOW_UTC = datetime(2026, 1, 1, tzinfo=timezone.utc)


class _Pinned(datetime):
    @classmethod
    def now(cls, tz=None):
        return NOW_UTC if tz is not None else NOW_UTC.replace(tzinfo=None)


def _pinned(fn, *args):
    import meeting.time_utils as time_utils

    saved = time_utils.datetime
    time_utils.datetime = _Pinned
    try:
        return fn(*args)
    finally:
        time_utils.datetime = saved


def _b64(raw: bytes) -> str:
    return base64.urlsafe_b64encode(raw).decode("ascii").rstrip("=")


def _b64_json(value) -> str:
    return _b64(json.dumps(value).encode("utf-8"))


_BAD_CURSORS = [
    "bad",
    "!!!",
    "e30",
    "WzEsMiwzXQ",
    "MQ",
    "bnVsbA",
    _b64(b"[" * 10_000),
]

_SEGMENTS = [
    {"id": "sg_1", "start_s": 1.0},
    {"id": "sg_2", "start_s": 0},
    {"id": "sg_\u00e9\u65e5", "start_s": 12.25},
    {"id": 7, "start_s": "3.5"},
    {"start_s": 2},
    {"id": "sg_3"},
    {},
    {"id": None, "start_s": True},
    {"id": "sg_big", "start_s": 1e300},
    {"id": "sg_small", "start_s": 1e-7},
    {"id": "sg_inf", "start_s": float("inf")},
    {"id": "sg_nan", "start_s": float("nan")},
    {"id": "sg_neg", "start_s": -0.0},
    {"id": "sg_none", "start_s": None},
    {"id": "sg_text", "start_s": "abc"},
    {"id": "sg_list", "start_s": [1]},
    {"id": ["a", 1], "start_s": 4},
    {"id": {"k": None}, "start_s": 5},
]

_TRANSCRIPT_CURSORS = [""] + _BAD_CURSORS + [
    "WyJhIiwiYiJd",
    _b64(b'[' + b"9" * 400 + b',"s"]'),
    _b64(b'[1e400,"s"]'),
    _b64(b'[-Infinity,"s"]'),
    _b64(b'[NaN,"s"]'),
    _b64(b'[1,"sg_1"]'),
    _b64(b'[1.5, "sg_1"]'),
    _b64(b' [ 2 , "x" ] '),
    _b64(b'["2.5",7]'),
    _b64(b'[true,null]'),
    _b64(b'[1,[1,2]]'),
    _b64(b'[1,{"a":1}]'),
    _b64(b'"12"'),
    _b64(b'"1"'),
    _b64(b'{"3.5":0,"seg":1}'),
    _b64(b'{"3.5":0}'),
    _b64('["1","\u00e9"]'.encode("utf-8")),
    _b64(b'["1","\\u00e9"]'),
    _b64(b'\xef\xbb\xbf[1,"s"]'),
    _b64(b'[1,"s"]\xff'),
    _b64(b'[' + b"9" * 300 + b',"s"]'),
    _b64(b'[-' + b"9" * 400 + b',"s"]'),
    _b64(b'[1_0,"s"]'),
    "WzEsInNnXzEiXQ==",
    "WzEsInNnXzEiXQ=",
    "WzEsInNn-XzEiXQ",
    "WzEsI.nNnXzEiXQ",
    "WzEsInNnXzEiXQ==junk",
    "W",
    "WzEsInNnXzEiX",
    "\u00e9\u00e9\u00e9\u00e9",
]

_MEETINGS_FOR_CURSOR = [
    {"id": "m_1", "started_at": "2026-01-01T09:00:00Z"},
    {"id": "m_\u00e9", "started_at": "2026-01-01T09:00:00+01:00"},
    {"id": 5, "started_at": None},
    {"id": "m_2", "started_at": 12.5},
    {"id": "m_3"},
    {"started_at": "x"},
]

_HISTORY_CURSORS = [""] + _BAD_CURSORS + [
    "WzEsMl0",
    "x" * 5000,
    "x" * 1024,
    "x" * 1025,
    _b64_json(["2026-01-01T09:00:00Z", "m_1"]),
    _b64_json(["a", "b"]),
    _b64_json(["a", "b", "c"]),
    _b64_json(["a", 1]),
    _b64_json(["a", None]),
    _b64_json({"a": "b"}),
    _b64_json("ab"),
    _b64(b'["\\u00e9","m"]'),
    _b64('["\u00e9","m"]'.encode("utf-8")),
    _b64(b'\xef\xbb\xbf["a","b"]'),
    _b64('["a","b"]'.encode("utf-16")),
    _b64('["a","b"]'.encode("utf-16-le")),
    _b64('["a","b"]'.encode("utf-16-be")),
    _b64('["a","b"]'.encode("utf-32")),
    _b64('["a","b"]'.encode("utf-32-le")),
    _b64('["a","b"]'.encode("utf-32-be")),
    _b64('["a","b"]'.encode("utf-16-le")[:-1]),
    _b64(b'["a","b"]\xff'),
    _b64(b'["a"  ,  "b"]  '),
    _b64(b"[" * 5000 + b"]" * 5000),
    "WyJhIiwiYiJd==",
    "WyJh-IiwiYiJd",
    "\u00e9" * 4,
    "W",
]


def _state(**fields):
    return json.dumps(fields)


_PEOPLE = {
    "p_b": {"id": "p_b", "display_name": "Bea", "kind": "guest", "created_at": "2026-01-01T09:05:00Z"},
    "p_me": {"id": "p_me", "display_name": "Me", "kind": "me", "created_at": "2026-01-01T09:10:00Z"},
    "p_a": {"id": "p_a", "display_name": "Ann", "kind": "others_cluster",
            "created_at": "2026-01-01T09:05:00Z"},
    "p_c": {"id": "p_c", "display_name": None, "kind": None},
    "p_d": {"id": "p_d", "display_name": "Dee", "kind": "guest", "created_at": "2026-01-01T09:01:00Z"},
    "p_e": {"id": "p_e", "display_name": "Eve", "kind": "guest", "created_at": "2026-01-01T09:02:00Z"},
    "p_f": {"id": "p_f", "display_name": "Fay", "kind": "guest", "created_at": "2026-01-01T09:03:00Z"},
    "p_noid": {"display_name": "Nobody", "kind": "guest"},
    "p_empty": {"id": "", "display_name": "Empty"},
    "p_num": {"id": 12, "display_name": 34, "kind": "guest", "created_at": 5},
    "p_junk": "not a dict",
}

_CARDS = {
    "decisions": [{"id": "d1", "status": "confirmed"}, {"id": "d2", "status": "removed"},
                  {"id": "d3"}, "junk"],
    "action_items": [{"id": "a1", "status": "proposed"}, {"id": "a2", "status": "removed"}],
    "risks": "not a list",
}

_ROWS = [
    None,
    {},
    {"id": "m_1", "title": "Weekly sync", "status": "ended",
     "started_at": "2026-01-01T09:00:00Z", "ended_at": "2026-01-01T09:50:00Z",
     "paused_total_s": 180, "cloud_enabled": True, "asr_model": "parakeet-tdt-0.6b-v3",
     "host_token": "secret-host", "guest_token": "secret-guest",
     "state_json": _state(title="Weekly sync", cards=_CARDS, participants=_PEOPLE,
                          questions=[{"id": "q1", "status": "open"},
                                     {"id": "q2", "status": "resolved"}, "junk"],
                          finalization={"status": "completed"}, cloud_enabled=True),
     "agent_provider": "openrouter", "spool_dir": "C:/spool/m_1"},
    {"id": "m_2", "title": "", "status": "ended", "started_at": "2026-01-01T09:00:00Z",
     "ended_at": "2026-01-01T10:00:00Z",
     "state_json": _state(topic={"current": "Budget planning"},
                          questions={"q1": {"status": "open"}, "q2": {"status": "open"},
                                     "q3": "x"},
                          finalization={"status": "failed", "card_deferred": True},
                          cloud_enabled=True)},
    {"id": "m_3", "status": "active", "started_at": "2025-12-31T23:00:00Z",
     "paused_total_s": 60, "state_json": "{broken"},
    {"id": "m_4", "status": "paused", "started_at": "2025-12-31T23:30:00Z",
     "ended_at": None, "paused_total_s": "x"},
    {"id": "m_5", "status": "ending", "started_at": "2025-12-31T23:59:00Z",
     "ended_at": "2025-12-31T23:59:30Z"},
    {"id": "m_6", "status": "failed", "started_at": None, "state_json": None},
    {"id": "m_7", "status": "ended", "started_at": "2026-01-01T09:00:00Z",
     "ended_at": "2026-01-01T08:00:00Z", "state_json": _state(cards="broken",
                                                              participants=["p1"])},
    {"id": "m_8", "status": "ended", "started_at": "2026-01-01T09:00:00",
     "ended_at": "2026-01-01T09:30:00", "paused_total_s": None,
     "content_summary": {"has_audio": True, "has_transcript": False,
                         "can_rerun_speakers": True, "preview_text": "Hi",
                         "is_empty": False}},
    {"id": "m_9", "status": "ended", "content_summary": {"has_audio": True}},
    {"id": "m_10", "status": "needs_recovery", "content_summary": "not a dict",
     "state_json": _state(finalization={"status": "running"}, cloud_enabled=True)},
    {"id": "m_11", "status": None, "title": None, "state_json": _state(title="From snapshot")},
    {"id": "m_12", "status": "ACTIVE", "started_at": "2025-12-31T23:00:00Z"},
    {"id": "m_13", "status": "ended", "state_json": _state(participants={})},
    {"id": "m_14", "status": "ended", "state_json": "[1, 2]"},
]

CASES = {
    "_encode_cursor": [[segment] for segment in _SEGMENTS],
    "_decode_cursor": [[cursor] for cursor in _TRANSCRIPT_CURSORS],
    "_history_cursor": [[meeting] for meeting in _MEETINGS_FOR_CURSOR],
    "_decode_history_cursor": [[cursor] for cursor in _HISTORY_CURSORS],
    "_public_meeting": [[row] for row in _ROWS],
    "_meeting_digest": [[row] for row in _ROWS[1:]],
}

WRAPPERS = {
    "_public_meeting": lambda module, row: _pinned(module._public_meeting, row),
}

PINS = {"now_utc": NOW_UTC.isoformat()}
