"""Curated inputs for meeting/web/ws.py (M7.13 meeting-web-hub).

    _on_store_batch(seq, results)   results are OpResult fields as dicts; the result is the list of
                                    [host_only, message] the hub hands schedule_broadcast
    _coerce_seq(value), _clean_guest_id(value)

The store results are made by OpenWhisper's MeetingStateStore over an ended meeting with AI
insights on (tests/test_meeting_custom_report.py's _store), with ids and time pinned as in
meeting.state.patches.py: an added note and a requested report, alone and together.
"""

import copy
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


def _fields(result):
    return copy.deepcopy({
        "ok": result.ok, "op": result.op, "reason": result.reason,
        "target_id": result.target_id, "effect": result.effect, "inverse": result.inverse,
        "current_revision": result.current_revision, "seq": result.seq,
    })


def _store_results():
    import meeting.state.schema as schema
    import meeting.time_utils as time_utils
    from meeting.state.schema import FinalizationState, MeetingState
    from meeting.state.store import MeetingStateStore

    saved_uuid, saved_datetime = schema.uuid, time_utils.datetime
    schema.uuid, time_utils.datetime = _FixedUuid(), _Pinned
    try:
        state = MeetingState(meeting_id="m_past", title="Vendor renewal", status="ended",
                             cloud_enabled=True)
        state.finalization = FinalizationState.coerce(
            {"status": "completed"}, cloud_enabled=True, meeting_status="ended")
        store = MeetingStateStore(state)
        item = store.apply("host", None, [{
            "op": "add_item", "card": "user_notes", "text": "a note"}])[0]
        report = store.apply("host", None, [{
            "op": "request_custom_report", "request": "a brief", "run_id": "run_1"}])[0]
        return _fields(item), _fields(report)
    finally:
        schema.uuid, time_utils.datetime = saved_uuid, saved_datetime


_CACHE = {}


def _batch(module, seq, results):
    from meeting.interfaces import OpResult

    hub = module.WsHub.__new__(module.WsHub)
    sent = []
    hub.schedule_broadcast = lambda message, host_only=False: sent.append([host_only, message])
    hub._on_store_batch(seq, [OpResult(**fields) for fields in results])
    return sent


def _cases():
    if "batch" not in _CACHE:
        item, report = _store_results()
        other_entity = {"ok": True, "op": {"op": "set_topic", "text": "Budget"},
                        "reason": None, "target_id": None,
                        "effect": {"entity": "topic", "topic": {"current": "Budget"}},
                        "inverse": None, "current_revision": None, "seq": 7}
        no_effect = {"ok": True, "op": {"op": "noop"}, "reason": None, "target_id": "x",
                     "effect": None, "inverse": None, "current_revision": None, "seq": 8}
        list_effect = dict(no_effect, effect=["custom_report"], seq=9)
        entity_not_text = dict(no_effect, effect={"entity": 5}, seq=10)
        _CACHE["batch"] = [
            [2, [item, report]],
            [1, [item]],
            [1, [report]],
            [3, [report, item]],
            [4, [report, report]],
            [5, []],
            [7, [other_entity, item]],
            [8, [no_effect]],
            [9, [list_effect, report]],
            [10, [entity_not_text]],
        ]
    return _CACHE["batch"]


class _LazyBatches(list):
    """The batch cases, built on first use (OpenWhisper is importable only after isolation)."""

    def __iter__(self):
        return iter(_cases())

    def __len__(self):
        return len(_cases())


CASES = {
    "_on_store_batch": _LazyBatches(),
    "_coerce_seq": [
        [0], [1], [3], [3.7], [-0.5], [-1], ["5"], [" 7 "], ["1_000"], ["3.5"], ["abc"], [""],
        [None], [True], [False], [[]], [{}], [[1]], [1e400], [float("-inf")], [float("nan")],
        [10 ** 40], [2 ** 63 - 1], [2 ** 63], [-(2 ** 63)], [9.223372036854776e18],
        [1e18], ["9223372036854775807"], ["9223372036854775808"], ["٣"], ["-0"],
    ],
    "_clean_guest_id": [
        [None], [""], ["   "], ["g_stable1"], ["  g_stable1  "], ["g-1_A"], ["a" * 64],
        ["a" * 65], ["has space"], ["dot.dot"], ["été"], ["g\n"], ["\tg\t"],
        ["　g　"], ["g/1"], ["g%20"],
    ],
}

WRAPPERS = {"_on_store_batch": _batch}

PINS = {"now_utc": NOW_UTC.isoformat(),
        "ids": "new_id gives <prefix>_<n as 12 hex digits>, n counting from 1"}
