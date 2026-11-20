"""Curated inputs for meeting/state/repair.py (M7.11 meeting-state-extras).

    build_timeline_backfill_ops(state, segments), build_timeline_coverage_from_segments(...),
    build_topic_backfill_ops(...), build_summary_backfill_ops(...)
        over MeetingState.to_dict()-shaped snapshots and segment rows.
    repair(initial, segments)
        repair_meeting_state over a MeetingStateStore without a repository, opened on
        MeetingState.from_dict(initial): {"applied": n, "state": the final snapshot}.

Ids and time are pinned as in meeting.state.patches: new_id counts "<prefix>_000000000001", ...
per case, and now is 2026-01-01T00:00:00Z.
"""

import itertools
from datetime import datetime, timezone

NOW_UTC = datetime(2026, 1, 1, tzinfo=timezone.utc)


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


def _repair(module, initial, segments):
    import meeting.state.schema as schema
    import meeting.time_utils as time_utils
    from meeting.state.store import MeetingStateStore

    saved_uuid, saved_datetime = schema.uuid, time_utils.datetime
    schema.uuid, time_utils.datetime = _FixedUuid(), _Pinned
    try:
        store = MeetingStateStore(schema.MeetingState.from_dict(initial))
        applied = module.repair_meeting_state(store, segments)
        return {"applied": applied, "state": store.snapshot()}
    finally:
        schema.uuid, time_utils.datetime = saved_uuid, saved_datetime


_IDS = itertools.count(1)


def _item(text, card="key_points", evidence=("sg_1",), **extra):
    item = {"id": f"it_{next(_IDS):04d}", "card": card, "text": text,
            "data": {}, "status": "proposed", "author_type": "agent", "author_id": "agent",
            "pinned": False, "revision": 1, "evidence": list(evidence),
            "created_at": "2025-12-31T00:00:00Z", "updated_at": "2025-12-31T00:00:00Z",
            "review": {}, "citation_check": {}}
    item.update(extra)
    return item


def _note(text, heading=None, start_s=None, evidence=("sg_1",), **extra):
    data = {}
    if heading is not None:
        data["heading"] = heading
    if start_s is not None:
        data["start_s"] = start_s
    return _item(text, card="live_notes", evidence=evidence, data=data, **extra)


def _seg(seg_id, start_s, text="words"):
    end_s = start_s + 2 if isinstance(start_s, (int, float)) else None
    return {"id": seg_id, "start_s": start_s, "end_s": end_s, "text": text}


SEGMENTS = [_seg("sg_1", 3.0), _seg("sg_2", 12.5), _seg("sg_3", "40"), _seg("sg_4", None),
            _seg("sg_5", "soon"), _seg("sg_6", 0), "junk", {"id": ""}, _seg("sg_7", 75.25)]

_REPAIRED = {"author_type": "system", "author_id": "state_repair"}

_SNAPSHOTS = [
    {"cards": {}},
    {"cards": {"timeline": [_item("Beat", card="timeline", data={"start_s": 1.0})]}},
    {"cards": {"timeline": [_item("Gone", card="timeline", status="removed")],
               "key_points": [_item("Budget approved for the launch", evidence=["sg_2", "sg_1"]),
                              _item("Unanchored point", evidence=[]),
                              _item("Strange anchors", evidence=["sg_4", "sg_5"]),
                              _item("String start", evidence=["sg_3"]),
                              _item("x" * 200 + "\nmore", evidence=["sg_7"]),
                              _item("   ", evidence=["sg_1"]),
                              _item("Legacy sample", evidence=["sg_1"], **_REPAIRED),
                              _item("Edited sample", evidence=["sg_1"], status="edited",
                                    **_REPAIRED),
                              _item("Pinned sample", evidence=["sg_1"], pinned=True, **_REPAIRED),
                              _item("Synthesized sample", evidence=["sg_1"],
                                    data={"insight_synthesized": True}, **_REPAIRED),
                              _item("Truthy but not True", evidence=["sg_1"],
                                    data={"insight_synthesized": 1}, **_REPAIRED)]}},
    {"cards": {"key_points": [_item(f"Point number {n}", evidence=["sg_7"]) for n in range(10)]}},
    {"cards": {"live_notes": [_note("Opened the meeting.", heading="Opening", start_s=2.0),
                              _note("Opening: already headed.", heading="Opening"),
                              _note("No heading here.", start_s=True, evidence=["sg_2"]),
                              _note("Unknown anchors", evidence=["sg_9"]),
                              _note("", heading="", evidence=["sg_1"]),
                              _note("Float string start", start_s="7.5"),
                              _note("Removed note", status="removed")]}},
    {"topic": {"current": "  "}, "rolling_summary": " ",
     "cards": {"key_points": [_item("Why do assumptions fail?", evidence=["sg_1", "sg_1"]),
                              _item("Second point.", evidence=["sg_2"]),
                              _item("Third point...", evidence=[]),
                              _item("", evidence=["sg_3"]),
                              _item("Fourth", evidence=["sg_4"]),
                              _item("Fifth", evidence=["sg_5"]),
                              _item("Sixth", evidence=["sg_6"]),
                              _item("Seventh is beyond six", evidence=["sg_7"])]}},
    {"topic": {"current": "Already set"}, "rolling_summary": "Already written.",
     "cards": {"key_points": [_item("Point")]}},
    {"topic": {"current": ""}, "rolling_summary": "",
     "cards": {"live_notes": [_note("Maria walked the numbers.", heading="Revenue review"),
                              _note("Team agreed.", heading="Decision")]}},
    {"topic": None, "rolling_summary": None,
     "cards": {"live_notes": [_note("Only text here.", evidence=[])]}},
    {"cards": {"key_points": [_item("Opening puzzle about assumptions", evidence=["sg_1"]),
                              _item("For example, why is Apple so innovative?",
                                    evidence=["sg_2"]),
                              _item("Two words", evidence=["sg_2"]),
                              _item("Résumé naïve façade coöperation", evidence=["sg_7"]),
                              _item("Unanchored but long enough text", evidence=[])],
               "timeline": [_item("Opening puzzle about assumptions", card="timeline",
                                  data={"start_s": 0.0})]}},
    {"cards": {"key_points": [_item(f"Distinct topic number {n} here", evidence=["sg_7"])
                              for n in range(9)],
               "timeline": [_item(f"Existing beat {n}", card="timeline") for n in range(6)]}},
]

_STATES = [
    {"meeting_id": "m_repair", "status": "active",
     "cards": {"key_points": [_item("Ship Friday", evidence=["sg_1"])]}},
    {"meeting_id": "m_named", "status": "ended", "title": "",
     "topic": {"current": "Q3 roadmap", "history": []}},
    {"meeting_id": "m_named", "status": "failed", "title": "Kept",
     "topic": {"current": "Q3 roadmap", "history": []}},
    {"meeting_id": "m_noribbon", "status": "needs_recovery", "report_views": ["brief"],
     "cards": {"key_points": [_item("The benchmarks held under peak load.",
                                    evidence=["sg_2"])]}},
    {"meeting_id": "m_notes", "status": "ended",
     "cards": {"live_notes": [_note("Read replicas reduced latency by forty percent.",
                                    heading="Database latency", start_s=30.0,
                                    evidence=["sg_7"])]}},
    {"meeting_id": "m_raw", "status": "ended"},
]

CASES = {
    "build_timeline_backfill_ops": [[s, SEGMENTS] for s in _SNAPSHOTS] + [[_SNAPSHOTS[2], []]],
    "build_timeline_coverage_from_segments": [[s, SEGMENTS] for s in _SNAPSHOTS],
    "build_topic_backfill_ops": [[s, SEGMENTS] for s in _SNAPSHOTS],
    "build_summary_backfill_ops": [[s, SEGMENTS] for s in _SNAPSHOTS],
    "repair": [[s, SEGMENTS] for s in _STATES],
}

WRAPPERS = {"repair": _repair}

PINS = {
    "now_utc": NOW_UTC.isoformat(),
    "ids": "new_id gives <prefix>_<n as 12 hex digits>, n counting from 1 per case",
}
