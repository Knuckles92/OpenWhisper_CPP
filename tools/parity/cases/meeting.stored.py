"""Curated inputs for meeting/stored.py (M7.7 meeting-state-store).

"Now" is pinned to 2026-01-01T00:00:00Z (the replay's FakeClock default) by swapping
meeting.time_utils.datetime, so legacy items that lack created_at get a fixed stamp. A fresh
state reads the report views from OpenWhisper's settings, which the isolated data folder leaves
at their defaults (every view on); the replay passes empty settings.

    load_state(row, historical)             load_state(row, historical=...).to_dict()
    load_state_with_id(row, meeting_id)     load_state(row, meeting_id).to_dict()
    compact_finalization_list_fields(row)
    finalization_from_meeting_row(row)      .to_dict()
    stored_state_dict(row), stored_endpoint(row)
"""

import json
from datetime import datetime, timezone

NOW_UTC = datetime(2026, 1, 1, tzinfo=timezone.utc)


class _Pinned(datetime):
    @classmethod
    def now(cls, tz=None):
        return NOW_UTC if tz is not None else NOW_UTC.replace(tzinfo=None)


def _pinned(fn, *args, **kwargs):
    import meeting.time_utils as time_utils

    saved = time_utils.datetime
    time_utils.datetime = _Pinned
    try:
        return fn(*args, **kwargs)
    finally:
        time_utils.datetime = saved


def _load(module, row, historical):
    return _pinned(lambda: module.load_state(row, historical=historical).to_dict())


def _load_with_id(module, row, meeting_id):
    return _pinned(lambda: module.load_state(row, meeting_id).to_dict())


def _snapshot(**overrides):
    state = {
        "meeting_id": "m_saved", "seq": 4, "status": "ending", "cloud_enabled": False,
        "title": "Snapshot title",
        "topic": {"current": "Budget", "history": []},
        "cards": {"key_points": [{"id": "it_1", "card": "key_points", "text": "Point",
                                  "evidence": ["sg_1"]}]},
        "finalization": {"status": "running", "message": "Writing insights"},
        "insight_review": {"status": "running", "questions": [], "extra": 1},
        "custom_reports": [
            {"id": "rp_running", "request": "Budget recap", "status": "running"},
            {"id": "rp_ready", "request": "Owners", "status": "ready", "markdown": "Done"},
            "junk",
        ],
    }
    state.update(overrides)
    return state


def _row(state, **fields):
    row = {
        "id": "m_row", "title": "Row title", "status": "ended", "cloud_enabled": True,
        "state_seq": 9,
        "state_json": json.dumps(state) if isinstance(state, (dict, list)) else state,
    }
    row.update(fields)
    return row


_ROWS = [
    _row(_snapshot()),
    _row("{broken"),
    _row(None),
    _row(""),
    _row("[1, 2]"),
    _row(_snapshot(), title="", status=""),
    _row(_snapshot(), title=None, status=None, cloud_enabled=False, state_seq=None),
    _row(_snapshot(seq=None)),
    _row({"meeting_id": "m_other", "finalization": {"status": "completed"}}),
    _row({"finalization": {"status": "pending"}, "cloud_enabled": True}, status="needs_recovery"),
    _row({"finalization": {"status": "failed", "card_deferred": True}}, status="ended"),
    _row({"insight_review": "not a dict", "custom_reports": "not a list"}),
    _row({"cards": "broken"}),
    _row(_snapshot(), status="active"),
    {"id": "m_bare"},
    {"state_json": json.dumps(_snapshot())},
    {"id": "m_fresh", "title": "Fresh", "cloud_enabled": 1, "state_json": "{oops"},
]

CASES = {
    "load_state": [[row, historical] for row in _ROWS for historical in (False, True)],
    "load_state_with_id": [[row, "m_given"] for row in _ROWS[:4]] + [[_ROWS[0], ""]],
    "compact_finalization_list_fields": [[row] for row in _ROWS] + [
        [{"status": "ended", "state_json": json.dumps({"finalization": {
            "status": "completed", "steps": [{"status": "failed"}]}}), "cloud_enabled": True}],
        [{"status": "failed"}],
        [{"status": "active", "state_json": json.dumps({"finalization": {"status": "running"},
                                                        "cloud_enabled": True})}],
    ],
    "finalization_from_meeting_row": [[row] for row in _ROWS],
    "stored_state_dict": [
        [None], [{}], [{"state_json": None}], [{"state_json": "{broken"}],
        [{"state_json": "[1, 2]"}], [{"state_json": '{"title": "x"}'}],
        [{"state_json": {"title": "already parsed"}}],
    ],
    "stored_endpoint": [
        [None], [{}], [{"agent_endpoint_json": None}], [{"agent_endpoint_json": ""}],
        [{"agent_endpoint_json": "   "}], [{"agent_endpoint_json": "{broken"}],
        [{"agent_endpoint_json": "[1]"}],
        [{"agent_endpoint_json": '{"base_url": "http://127.0.0.1:1234/v1", "kind": "custom"}'}],
        [{"agent_endpoint_json": {"kind": "dict"}}],
        [{"agent_endpoint_json": 5}],
    ],
}

WRAPPERS = {
    "load_state": _load,
    "load_state_with_id": _load_with_id,
    "finalization_from_meeting_row":
        lambda module, row: module.finalization_from_meeting_row(row).to_dict(),
}

PINS = {"now_utc": NOW_UTC.isoformat()}
