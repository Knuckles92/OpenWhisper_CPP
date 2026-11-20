"""Curated inputs for meeting/state/schema.py (M7.3 meeting-state-model).

"Now" is pinned to 2026-01-01T00:00:00Z (the replay's FakeClock default) by swapping
meeting.time_utils.datetime, so legacy items that lack created_at get a fixed stamp.

    round_trip(doc)                     MeetingState.from_dict(doc).to_dict()
    coerce(value, cloud, status)        FinalizationState.coerce(...).to_dict()
    normalize_historical(value, cloud, status)
    history_pill(value, meeting_status) FinalizationState.coerce(value).history_pill(...)
    default_for_cloud(cloud), infer_legacy(cloud, status)   .to_dict()
    parse_state_json(raw)
    coerce_card_deferred(value)         _coerce_card_deferred
    card_protected(item)                CardItem.from_dict(item).protected
"""

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


def _round_trip(module, doc):
    return _pinned(lambda: module.MeetingState.from_dict(doc).to_dict())


def _coerce(module, value, cloud, status):
    return module.FinalizationState.coerce(
        value, cloud_enabled=cloud, meeting_status=status).to_dict()


def _normalize(module, value, cloud, status):
    return module.FinalizationState.normalize_historical(
        value, cloud_enabled=cloud, meeting_status=status).to_dict()


def _pill(module, value, meeting_status):
    pill = module.FinalizationState.coerce(value).history_pill(meeting_status=meeting_status)
    return None if pill is None else list(pill)


def _default(module, cloud):
    return module.FinalizationState.default_for_cloud(cloud).to_dict()


def _legacy(module, cloud, status):
    return module.FinalizationState.infer_legacy(
        cloud_enabled=cloud, meeting_status=status).to_dict()


def _protected(module, item):
    return _pinned(lambda: module.CardItem.from_dict(item).protected)


_ITEM = {
    "id": "it_1", "card": "key_points", "text": "Budget approved", "data": {"n": 1},
    "status": "proposed", "author_type": "agent", "author_id": "agent", "pinned": False,
    "revision": 2, "evidence": ["sg_1", "sg_2"], "created_at": "2026-01-02T00:00:00Z",
    "updated_at": "2026-01-02T00:00:01Z", "review": {"state": "supported"},
    "citation_check": {"revision": 2},
}


def _full_state():
    return {
        "meeting_id": "m_full", "seq": 7, "status": "ended", "cloud_enabled": True,
        "intelligence_online": True, "diarization_available": True,
        "diarization_reason": "", "speech": {"engine": "parakeet"}, "title": "Weekly sync",
        "intent": {"text": "Decide the vendor", "updated_at": "2026-01-02T00:00:00Z",
                   "author_id": "p_host"},
        "topic": {"current": "Vendors", "history": [
            {"text": "Vendors", "ts": "2026-01-02T00:00:00Z", "evidence": ["sg_1"],
             "actor_type": "agent"}]},
        "rolling_summary": "We talked.", "rolling_summary_evidence": ["sg_1"],
        "capture": {"mic_available": True, "loopback_available": False, "message": "ok",
                    "audio_incomplete": False, "integrity_error": ""},
        "participants": {"p_me": {
            "id": "p_me", "display_name": "Me", "kind": "me", "name_source": "human",
            "is_provisional": False, "created_at": "2026-01-02T00:00:00Z",
            "updated_at": "2026-01-02T00:00:00Z"}},
        "cards": {"key_points": [_ITEM], "decisions": [], "action_items": [], "risks": [],
                  "timeline": [], "live_notes": [], "user_notes": []},
        "questions": [{
            "id": "q_1", "text": "Who owns it?", "status": "resolved",
            "suggested_answer": None, "suggested_confidence": None, "answer": "Sam",
            "answer_source": "user", "confidence": 1, "thread": [{"text": "Sam"}],
            "evidence": ["sg_2"], "asked_at": "2026-01-02T00:00:00Z",
            "resolved_at": "2026-01-02T00:01:00Z", "resolved_by": "p_me"}],
        "finalization": {"status": "completed", "message": "", "stage": "done",
                         "current_step": 3, "total_steps": 3, "step_details": "",
                         "steps": [{"id": "a", "status": "done"}],
                         "summary_stats": {"segments": 4}, "card_deferred": False},
        "report_views": ["brief"], "insight_review": {"enabled": True, "questions": []},
        "live_highlights": [{"kind": "decision", "text": "Go", "start_s": 3.5}],
        "live_highlights_status": "ready", "voice_feedback": {"text": "ok"},
        "custom_reports": [{
            "id": "rep_1", "request": "Summarize", "status": "ready", "title": "Summary",
            "markdown": "# Summary", "message": "", "run_id": "r1", "sources": {"segments": 4},
            "requested_by": "p_me", "created_at": "2026-01-02T00:00:00Z",
            "updated_at": "2026-01-02T00:00:00Z"}],
    }


def _legacy_state(**overrides):
    doc = {"meeting_id": "m_old", "status": "ended", "cloud_enabled": True}
    doc.update(overrides)
    return doc


_STEPS = [{"id": "a", "status": "done"}, {"id": "b", "status": "running"},
          {"id": "c", "status": "pending", "detail": "Waiting"}, {"id": "d", "status": "failed"}]

_FINALIZATIONS = [
    None, "completed", 5, [], {}, {"status": "bogus"}, {"status": None},
    {"status": "pending"}, {"status": "running", "message": None},
    {"status": "completed", "message": 7, "stage": 0, "current_step": "2",
     "total_steps": 3.9, "step_details": None, "steps": _STEPS,
     "summary_stats": {"a": 1}, "card_deferred": "Yes "},
    {"status": "failed", "steps": "not a list", "summary_stats": ["x"], "card_deferred": 1},
    {"status": "unavailable", "card_deferred": "no"},
    {"status": "disabled", "message": "AI insights are off for this meeting."},
    {"status": "running", "steps": _STEPS, "card_deferred": True},
    {"status": "pending", "steps": [{"id": "x", "status": "pending", "detail": ""}]},
    {"status": "completed", "steps": [{"status": "failed"}]},
    {"status": "completed", "current_step": "x"},
    {"status": "completed", "steps": [5]},
]

_STATUSES = ["active", "paused", "ending", "ended", "failed", "needs_recovery", ""]

CASES = {
    "round_trip": [
        [_full_state()],
        [{"meeting_id": "m_min"}],
        [_legacy_state()],
        [_legacy_state(cloud_enabled=False)],
        [_legacy_state(status="active")],
        [_legacy_state(finalization=None)],
        [_legacy_state(finalization={"status": "running", "steps": _STEPS})],
        [_legacy_state(report_views=[])],
        [_legacy_state(report_views=None)],
        [_legacy_state(report_views=["signal", "ribbon"])],
        [_legacy_state(capture={"message": None, "mic_available": 1, "extra": True})],
        [_legacy_state(capture=None)],
        [_legacy_state(unknown_key="dropped", cards={"key_points": [_ITEM], "bogus": [_ITEM]})],
        [_legacy_state(cards={"key_points": [{"id": "it_x", "card": "key_points"}]})],
        [_legacy_state(cards={"live_notes": [{"id": "it_n", "card": "live_notes",
                                              "text": "Notes", "data": {"heading": "H"}}]})],
        [_legacy_state(questions=[{"id": "q_a", "text": "A?"}, {"id": "q_a", "text": "Again?"},
                                  {"id": "q_b"}])],
        [_legacy_state(participants={"p_x": {"id": "p_y", "display_name": "X"}})],
        [_legacy_state(custom_reports=[{"id": "rep_a", "status": "weird"}, {"request": "no id"},
                                       "junk", {"id": "", "status": "ready"}])],
        [_legacy_state(intent="not a dict", topic={"current": "T"})],
        [_legacy_state(intent={"text": 5, "author_id": None})],
        [_legacy_state(seq="12", live_highlights={"odd": True}, speech=None,
                       diarization_reason=0)],
        [_legacy_state(cards={"key_points": [dict(_ITEM, revision="3", pinned="yes",
                                                  evidence=None, data=None)]})],
        [{"status": "active"}],
        [_legacy_state(seq="x")],
        [_legacy_state(cards={"key_points": [{"card": "key_points"}]})],
        [_legacy_state(cards={"key_points": ["junk"]})],
        [_legacy_state(cards={"key_points": None})],
        [_legacy_state(cards=["key_points"])],
        [_legacy_state(topic=["x"])],
        [_legacy_state(finalization={"status": "completed", "current_step": "x"})],
    ],
    "coerce": [[value, cloud, status] for value in _FINALIZATIONS for cloud in (False, True)
               for status in ("active", "ended")],
    "normalize_historical": [[value, cloud, status] for value in _FINALIZATIONS[:16]
                             for cloud in (False, True) for status in _STATUSES],
    "history_pill": [[value, status] for value in [
        {"status": "pending"}, {"status": "running"}, {"status": "completed"},
        {"status": "completed", "steps": [{"status": "failed"}]},
        {"status": "completed", "steps": [{"status": ""}, {"status": None}]},
        {"status": "failed"}, {"status": "unavailable"}, {"status": "disabled"},
        {"status": "failed", "card_deferred": True},
        {"status": "unavailable", "card_deferred": True},
        {"status": "completed", "steps": [{"status": "failed"}], "card_deferred": True},
        {"status": "completed", "card_deferred": True},
        {"status": "disabled", "card_deferred": True},
    ] for status in ("", "active", "paused", "ending", "ended", "failed")],
    "default_for_cloud": [[True], [False]],
    "infer_legacy": [[cloud, status] for cloud in (False, True) for status in _STATUSES],
    "parse_state_json": [
        [None], [""], ["{}"], ['{"meeting_id": "m"}'], ["[1, 2]"], ["not json"], ['"text"'],
        [{"meeting_id": "m"}], [{}], [5], [0], [["a"]], ['{"a": NaN}'], ["  {\"a\": 1}  "],
    ],
    "coerce_card_deferred": [
        [True], [False], [1], [0], [0.5], [0.0], ["1"], ["true"], [" TRUE "], ["Yes"], ["no"],
        ["0"], [""], [None], [[1]], [{"a": 1}], ["y"],
    ],
    "card_protected": [
        [_ITEM], [dict(_ITEM, pinned=True)], [dict(_ITEM, status="edited")],
        [dict(_ITEM, status="confirmed")], [dict(_ITEM, status="removed")],
        [dict(_ITEM, author_type="system", author_id="voice_command")],
        [dict(_ITEM, author_type="system", author_id="state_repair")],
        [dict(_ITEM, author_type="user", author_id="voice_command")],
    ],
}

WRAPPERS = {
    "round_trip": _round_trip,
    "coerce": _coerce,
    "normalize_historical": _normalize,
    "history_pill": _pill,
    "default_for_cloud": _default,
    "infer_legacy": _legacy,
    "parse_state_json": lambda module, raw: module.parse_state_json(raw),
    "coerce_card_deferred": lambda module, value: module._coerce_card_deferred(value),
    "card_protected": _protected,
}

PINS = {"now_utc": NOW_UTC.isoformat()}
