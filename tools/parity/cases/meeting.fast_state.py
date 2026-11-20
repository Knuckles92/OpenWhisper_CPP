"""Op scripts for meeting/fast_state.py (M7.11 meeting-state-extras).

    script(initial, batches)
        As meeting.state.patches's script: initial is a MeetingState.to_dict() document, each
        batch [actor_type, actor_id, known, pinned, ops] goes through patches.apply_ops (whose
        handler table holds FAST_HANDLERS), known listing the ids segment_exists accepts (None:
        no predicate); the result is {"results": [...], "state": ...}.

Time is pinned as in meeting.state.patches: now is 2026-01-01T00:00:00Z.
"""

import copy
from datetime import datetime, timezone

NOW_UTC = datetime(2026, 1, 1, tzinfo=timezone.utc)


class _Pinned(datetime):
    @classmethod
    def now(cls, tz=None):
        return NOW_UTC if tz is not None else NOW_UTC.replace(tzinfo=None)


def _result(r):
    return copy.deepcopy({
        "ok": r.ok, "op": r.op, "reason": r.reason, "target_id": r.target_id,
        "effect": r.effect, "inverse": r.inverse, "current_revision": r.current_revision,
        "seq": r.seq,
    })


def _predicate(ids):
    if ids is None:
        return None
    known = frozenset(ids)
    return lambda segment_id: segment_id in known


def _script(module, initial, batches):
    import meeting.state.patches as patches
    import meeting.state.schema as schema
    import meeting.time_utils as time_utils

    saved_datetime = time_utils.datetime
    time_utils.datetime = _Pinned
    try:
        state = (schema.MeetingState(meeting_id="m_test") if initial is None
                 else schema.MeetingState.from_dict(initial))
        results = []
        for actor_type, actor_id, known, _pinned, ops in batches:
            ctx = patches.OpContext(actor_type, actor_id, segment_exists=_predicate(known))
            results.append([_result(r) for r in patches.apply_ops(state, ops, ctx)])
        return {"results": results, "state": state.to_dict()}
    finally:
        time_utils.datetime = saved_datetime


def _item(item_id, card, text, **extra):
    item = {"id": item_id, "card": card, "text": text, "data": {}, "status": "proposed",
            "author_type": "agent", "author_id": "agent", "pinned": False, "revision": 1,
            "evidence": ["sg_1"], "created_at": "2025-12-31T00:00:00Z",
            "updated_at": "2025-12-31T00:00:00Z", "review": {}, "citation_check": {}}
    item.update(extra)
    return item


def _pulse(pulse_id, kind, start_s, text="Moment", segment_id="sg_1", **extra):
    pulse = {"id": pulse_id, "kind": kind, "start_s": start_s, "text": text,
             "segment_id": segment_id, "probability": 0.9}
    pulse.update(extra)
    return pulse


def _signals(*ops, known=("sg_1", "sg_2")):
    return ["system", "live-signals", None if known is None else list(known), None, list(ops)]


def _verifier(*ops):
    return ["system", "citation-verifier", None, None, list(ops)]


def _voice(*ops):
    return ["system", "voice_command", None, None, list(ops)]


def _as(actor_type, actor_id, *ops):
    return [actor_type, actor_id, None, None, list(ops)]


_CARDS = {
    "key_points": [_item("it_a", "key_points", "Alpha", evidence=["sg_1", "sg_2"]),
                   _item("it_r", "key_points", "Removed", status="removed")],
    "decisions": [_item("it_d", "decisions", "Ship Friday", revision=2,
                        citation_check={"status": "supported", "revision": 2}),
                  _item("it_e", "decisions", "No evidence", evidence=[])],
}

_HIGHLIGHTS = [_pulse("pulse_1_decision", "decision", 70), _pulse("pulse_1_number", "number", 61),
               _pulse("pulse_0_takeaway", "takeaway", 5)]

SCRIPTS = [
    [{"meeting_id": "m_test", "status": "active", "live_highlights": _HIGHLIGHTS}, [
        _as("user", "guest", {"op": "publish_highlights", "pulses": []}),
        _as("system", "other", {"op": "publish_highlights", "pulses": []}),
        _signals({"op": "publish_highlights", "pulses": [], "final": True}),
        _signals({"op": "publish_highlights", "pulses": "x"}),
        _signals({"op": "publish_highlights",
                  "pulses": [_pulse(f"p{n}", "decision", n) for n in range(6)]}),
        _signals({"op": "publish_highlights", "minute": True, "pulses": []}),
        _signals({"op": "publish_highlights", "minute": -1, "pulses": []}),
        _signals({"op": "publish_highlights", "minute": 1.5, "pulses": []}),
        _signals({"op": "publish_highlights", "pulses": [_pulse("x", "gossip", 3)]}),
        _signals({"op": "publish_highlights", "pulses": [_pulse("x", "decision", 3,
                                                                segment_id="sg_9")]}),
        _signals({"op": "publish_highlights", "pulses": [_pulse("x", "decision", 3,
                                                                segment_id=None)]}),
        _signals({"op": "publish_highlights", "minute": 1,
                  "pulses": [_pulse("pulse_1_commitment", "commitment", 65),
                             _pulse("pulse_0_takeaway", "takeaway", 4, text="Replaced")]}),
        _signals({"op": "publish_highlights"}),
        _signals({"op": "publish_highlights", "pulses": [_pulse("late", "number", 0.5,
                                                                segment_id="sg_7")]},
                 known=None),
    ]],
    [{"meeting_id": "m_test", "status": "ended"}, [
        _signals({"op": "publish_highlights", "pulses": [_pulse("p", "decision", 1)]}),
        _signals({"op": "publish_highlights", "final": 1, "pulses": []}),
        _signals({"op": "publish_highlights", "final": True,
                  "pulses": [_pulse("p", "decision", 1)]}),
    ]],
    [{"meeting_id": "m_test", "status": "paused",
      "live_highlights": [_pulse(f"old_{n}", "number", n) for n in range(1199)]},
     [_signals({"op": "publish_highlights",
                "pulses": [_pulse("new_a", "decision", 2000), _pulse("new_b", "takeaway", -1)]})]],
    [{"meeting_id": "m_test", "status": "active", "cards": _CARDS,
      "insight_review": {"enabled": True, "status": "ready"}}, [
        _as("user", "guest", {"op": "citation_check", "id": "it_d", "revision": 2}),
        _verifier({"op": "citation_check", "id": "it_nope", "revision": 1},
                  {"op": "citation_check", "id": "it_r", "revision": 1},
                  {"op": "citation_check", "revision": 1},
                  {"op": "citation_check", "id": "it_d", "revision": 1},
                  {"op": "citation_check", "id": "it_d", "revision": "2"},
                  {"op": "citation_check", "id": "it_d", "revision": 2.0,
                   "check": {"status": "contradicted", "revision": 2}},
                  {"op": "citation_check", "id": "it_a", "revision": 1, "check": None},
                  {"op": "citation_check", "id": "it_e", "revision": 1, "check": ["odd"]}),
        _verifier({"op": "invalidate_citations", "segment_ids": ["sg_9"]},
                  {"op": "invalidate_citations", "segment_ids": ["sg_2"]},
                  {"op": "invalidate_citations", "segment_ids": "sg_1"},
                  {"op": "invalidate_citations"}),
        _as("system", "voice_command", {"op": "invalidate_citations"}),
    ]],
    [{"meeting_id": "m_test", "status": "active"}, [
        _as("user", "guest", {"op": "voice_feedback", "message": "fake"}),
        _voice({"op": "voice_feedback", "message": "Saved your note."},
               {"op": "voice_feedback", "message": "é" * 600},
               {"op": "voice_feedback"},
               {"op": "voice_feedback", "message": None},
               {"op": "voice_feedback", "message": 42}),
    ]],
    [{"meeting_id": "m_test", "status": "ending"},
     [_voice({"op": "voice_feedback", "message": "Too late"})]],
]

CASES = {"script": [[initial, batches] for initial, batches in SCRIPTS]}

WRAPPERS = {"script": _script}

PINS = {"now_utc": NOW_UTC.isoformat()}
