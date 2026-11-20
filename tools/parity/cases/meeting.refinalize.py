"""Curated inputs for meeting/refinalize.py (M7.14 meeting-recovery).

    _run_ids(steps, from_step)                       direct
    overall_from_steps(steps, cloud_enabled)         _overall_from_steps -> [status, message]
    steps_from_state(steps, from_step, cloud_enabled, have_audio)
        _steps_from_state over a store whose finalization holds `steps`
    coverage_refusal(existing, decoded, guard)
        _coverage_refusal: `existing` the draft's segment texts (get_segments rows) or "raise"
        (the read fails); `decoded` the new pass's texts
    strip_unevidenced_proposed(state)
        the function over a store of MeetingState.from_dict(state); every card's
        [{id, status}] afterwards
"""


def _overall_from_steps(module, steps, cloud_enabled):
    status, message = module._overall_from_steps(steps, cloud_enabled=cloud_enabled)
    return [status, message]


def _steps_from_state(module, steps, from_step, cloud_enabled, have_audio):
    from meeting.state.schema import FinalizationState, MeetingState
    from meeting.state.store import MeetingStateStore

    state = MeetingState(meeting_id="m_gold", finalization=FinalizationState(steps=steps))
    store = MeetingStateStore(state)
    return module._steps_from_state(store, from_step=from_step, cloud_enabled=cloud_enabled,
                                    have_audio=have_audio)


class _Segments:
    def __init__(self, existing):
        self.existing = existing

    def get_segments(self, meeting_id):
        if self.existing == "raise":
            raise RuntimeError("database is locked")
        return [{"id": f"sg_{index}", "text": text} for index, text in enumerate(self.existing)]


def _coverage_refusal(module, existing, decoded, guard):
    from meeting.interfaces import TranscriptSegment

    segments = [
        TranscriptSegment(segment_id=f"sg_new_{index}", meeting_id="m_gold", chunk_id=None,
                          channel="mic", start_s=float(index), end_s=float(index + 1), text=text)
        for index, text in enumerate(decoded)
    ]
    return module._coverage_refusal(_Segments(existing), "m_gold", segments, guard)


def _strip(module, state):
    from meeting.state.schema import MeetingState
    from meeting.state.store import MeetingStateStore

    store = MeetingStateStore(MeetingState.from_dict(state))
    module.strip_unevidenced_proposed(store)
    cards = store.snapshot()["cards"]
    return {card: [{"id": item["id"], "status": item["status"]} for item in items]
            for card, items in cards.items()}


def _step(step_id, status="pending", **extra):
    out = {"id": step_id, "status": status}
    out.update(extra)
    return out


_STEP_LISTS = [
    [],
    [_step("redecode", "failed"), _step("polish", "completed"),
     _step("consolidation", "completed"), _step("finalize", "completed")],
    [_step("polish", "failed"), _step("consolidation"), _step("finalize")],
    [_step("redecode", "completed"), _step("speaker_id", "skipped"), _step("finalize")],
    [_step("redecode", "completed"), _step("consolidation", "completed"),
     _step("finalize", "completed")],
    [_step("redecode", "completed"), _step("finalize", "completed")],
    [_step("finalize", "completed")],
    [{"status": "failed"}, _step("finalize")],
    [{"id": "", "status": "pending"}, _step("polish", "failed")],
    [_step("custom", "failed"), _step("finalize")],
    [_step("finalize", "running"), _step("redecode", "completed")],
]
_FROM = ["failed", "redecode", "speaker_id", "polish", "consolidation", "finalize", "custom", ""]

_OVERALL = [
    [[], False],
    [[], True],
    [[_step("finalize", "completed")], False],
    [[_step("finalize", "completed")], True],
    [[_step("polish", "completed"), _step("finalize", "completed")], False],
    [[_step("consolidation", "pending")], False],
    [[_step("redecode", "failed", name="Audio Re-transcription", detail="Re-decoding failed"),
      _step("finalize", "completed")], True],
    [[_step("polish", "failed", name="Transcript Cleanup", detail="timed out"),
      _step("consolidation", "failed", name="Summary & Action Items", detail="no key")], True],
    [[_step("speaker_id", "failed", detail="api down"), _step("finalize", "completed")], False],
    [[_step("finalize", "failed")], False],
]

_DEFAULT_STEPS = [
    {"id": "redecode", "name": "Audio Re-transcription", "status": "failed",
     "detail": "Re-decoding failed; kept live transcript"},
    {"id": "polish", "name": "Transcript Cleanup", "status": "completed", "detail": "Done"},
    {"id": "finalize", "name": "State Finalization", "status": "completed", "detail": "Done"},
]

_LONG = "We should ship the budget review on Friday as planned"


def _item(item_id, card, status="proposed", **extra):
    out = {"id": item_id, "card": card, "text": f"text {item_id}", "status": status,
           "created_at": "2026-01-01T10:00:00Z", "updated_at": "2026-01-01T10:00:00Z"}
    out.update(extra)
    return out


_STRIP_STATES = [
    {"meeting_id": "m_gold", "cards": {}},
    {"meeting_id": "m_gold", "cards": {
        "key_points": [_item("k1", "key_points"), _item("k2", "key_points", evidence=["sg_1"]),
                       _item("k3", "key_points", "edited"),
                       _item("k4", "key_points", "confirmed"),
                       _item("k5", "key_points", pinned=True),
                       _item("k6", "key_points", "removed"),
                       _item("k7", "key_points", revision=4)],
        "decisions": [_item("d1", "decisions", author_type="system",
                            author_id="voice_command")],
        "action_items": [_item("a1", "action_items", author_type="user")],
        "risks": [_item("r1", "risks", evidence=[])],
        "timeline": [_item("t1", "timeline", data={"start_s": 3.0})],
        "live_notes": [_item("n1", "live_notes")],
        "user_notes": [_item("u1", "user_notes")],
    }},
]

CASES = {
    "_run_ids": [[steps, start] for steps in _STEP_LISTS for start in _FROM],
    "overall_from_steps": _OVERALL,
    "steps_from_state": [
        [steps, start, cloud, audio]
        for steps in ([], _DEFAULT_STEPS, [_step("finalize", "completed")])
        for start in ("failed", "redecode", "speaker_id", "polish", "consolidation", "finalize",
                      "custom")
        for cloud in (False, True)
        for audio in (False, True)
    ],
    "coverage_refusal": [
        [[_LONG, "Agreed"], [_LONG], True],
        [[_LONG, "Agreed"], [_LONG], False],
        [[_LONG], ["Hi"], True],
        [[_LONG], ["Hi"], False],
        [[], ["Hi"], True],
        ["raise", ["Hi"], True],
        [["one two three four five"], ["one two three four"], True],
        [["one two three four five"], ["one two three"], True],
        [["a b c d e f g h i j"], ["a b c d e f g h"], True],
        [["a b c d e f g h i j"], ["a b c d e f g"], True],
        [["  ", ""], ["word"], True],
    ],
    "strip_unevidenced_proposed": [[state] for state in _STRIP_STATES],
}

WRAPPERS = {
    "overall_from_steps": _overall_from_steps,
    "steps_from_state": _steps_from_state,
    "coverage_refusal": _coverage_refusal,
    "strip_unevidenced_proposed": _strip,
}
