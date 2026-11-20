"""Op scripts for meeting/state/review.py (M7.11 meeting-state-extras).

    script(initial, batches)
        As meeting.state.patches's script: initial is a MeetingState.to_dict() document, each
        batch [actor_type, actor_id, known, pinned, ops] goes through patches.apply_ops (whose
        handler table holds REVIEW_HANDLERS); the result is {"results": [...], "state": ...}.
    invalidate_checks(initial)          the to_dict() after review.invalidate_checks

Ids and time are pinned as in meeting.state.patches: new_id counts "<prefix>_000000000001", ...
per script, and now is 2026-01-01T00:00:00Z.
"""

import copy
from datetime import datetime, timezone

NOW_UTC = datetime(2026, 1, 1, tzinfo=timezone.utc)
CONSENT = "typesafe-text-v1"


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


def _pinned(fn):
    import meeting.state.schema as schema
    import meeting.time_utils as time_utils

    saved_uuid, saved_datetime = schema.uuid, time_utils.datetime
    schema.uuid, time_utils.datetime = _FixedUuid(), _Pinned
    try:
        return fn(schema)
    finally:
        schema.uuid, time_utils.datetime = saved_uuid, saved_datetime


def _script(module, initial, batches):
    import meeting.state.patches as patches

    def run(schema):
        state = (schema.MeetingState(meeting_id="m_test") if initial is None
                 else schema.MeetingState.from_dict(initial))
        results = []
        for actor_type, actor_id, known, pinned, ops in batches:
            ctx = patches.OpContext(actor_type, actor_id, segment_exists=_predicate(known),
                                    segment_pinned=_predicate(pinned))
            results.append([_result(r) for r in patches.apply_ops(state, ops, ctx)])
        return {"results": results, "state": state.to_dict()}

    return _pinned(run)


def _invalidate(module, initial):
    def run(schema):
        state = schema.MeetingState.from_dict(initial)
        module.invalidate_checks(state)
        return state.to_dict()

    return _pinned(run)


def _item(item_id, card, text, **extra):
    item = {"id": item_id, "card": card, "text": text, "data": {}, "status": "proposed",
            "author_type": "agent", "author_id": "agent", "pinned": False, "revision": 1,
            "evidence": ["sg_1"], "created_at": "2025-12-31T00:00:00Z",
            "updated_at": "2025-12-31T00:00:00Z", "review": {}, "citation_check": {}}
    item.update(extra)
    return item


def _question(qid, item_id, field="acceptance", status="open", revision=1, evidence=("sg_1",),
              **extra):
    choices = {
        "acceptance": {"accepted": "Agreed", "offered": "Only proposed", "edit": "Correct it",
                       "incorrect": "Incorrect"},
        "resolution": {"open": "Still open", "resolved": "Resolved", "edit": "Correct it",
                       "incorrect": "Incorrect"},
        "owner": {"edit": "Save clarification", "incorrect": "Incorrect"},
        "deadline": {"edit": "Save clarification", "incorrect": "Incorrect"},
    }.get(field, {"accurate": "Accurate", "edit": "Correct it", "incorrect": "Incorrect"})
    question = {"id": qid, "item_id": item_id, "revision": revision, "field": field,
                "text": "Was this agreed, or only proposed?", "choices": choices,
                "status": status, "evidence": list(evidence), "sources": [], "owners": [],
                "score": 0.3, "reason": "The evidence needs clarification on this point.",
                "priority": 1.4}
    question.update(extra)
    return question


def _review(questions=(), **extra):
    review = {"enabled": True, "consent": CONSENT, "sensitivity": "normal", "status": "pending",
              "questions": list(questions), "message": ""}
    review.update(extra)
    return review


def _state(cards=None, review=None, **extra):
    doc = {"meeting_id": "m_test", "status": "ended", "cloud_enabled": True,
           "finalization": {"status": "completed"}, "cards": cards or {},
           "insight_review": review if review is not None else _review()}
    doc.update(extra)
    return doc


def _system(*ops, actor="insight-review"):
    return ["system", actor, None, None, list(ops)]


def _host(*ops, actor="p_host"):
    return ["host", actor, None, None, list(ops)]


def _as(actor_type, *ops):
    return [actor_type, "someone", None, None, list(ops)]


def _answer(qid, answer, **extra):
    op = {"op": "review_answer", "question_id": qid, "answer": answer}
    op.update(extra)
    return op


def _assessment(item_id, revision=1, state="provisional"):
    return {"item_id": item_id, "revision": revision,
            "review": {"state": state, "scores": {"support": 0.9}, "model": "jev-1.13.0",
                       "assessed_at": "2025-12-31T00:00:00Z", "revision": revision}}


_ACTIONS = {
    "action_items": [_item("it_a", "action_items", "Send the paper links"),
                     _item("it_b", "action_items", "send the  PAPER links"),
                     _item("it_c", "action_items", "Draft the renewal note", evidence=["sg_2"]),
                     _item("it_p", "action_items", "Pinned task", pinned=True),
                     _item("it_r", "action_items", "Removed task", status="removed"),
                     _item("it_v", "action_items", "Revised task", revision=3)],
    "risks": [_item("it_k", "risks", "Vendor may slip", data={"severity": "high"})],
    "live_notes": [_item("it_n", "live_notes", "Paper links will be sent.",
                         data={"heading": "Links", "start_s": 4.0}),
                   _item("it_n2", "live_notes", "Unrelated block", evidence=["sg_9"]),
                   _item("it_nr", "live_notes", "Removed block", status="removed")],
}

_PEOPLE = {"p_alex": {"id": "p_alex", "display_name": "Alex", "kind": "others_cluster",
                      "name_source": "human", "is_provisional": False,
                      "created_at": "2025-12-31T00:00:00Z", "updated_at": "2025-12-31T00:00:00Z"}}

_ANSWERABLE = _state(cards=_ACTIONS, participants=_PEOPLE, review=_review([
    _question("rv_a", "it_a"),
    _question("rv_c", "it_c", field="owner"),
    _question("rv_k", "it_k", field="resolution"),
    _question("rv_v", "it_v", field="support", revision=3),
    _question("rv_old", "it_a", status="answered", correction="Old words", note_id="it_gone"),
    _question("rv_sup", "it_c", superseded=True),
]))

_LONG_NOTE = _state(cards={
    "action_items": [_item("it_a", "action_items", "Send the paper links")],
    "live_notes": [_item("it_n", "live_notes", "n" * 3990)],
}, review=_review([_question("rv_a", "it_a")]))

SCRIPTS = [
    # Begin: who, when, and the consent it needs.
    [_state(review=_review(enabled=False)), [_system({"op": "review_begin", "run_id": "r"})]],
    [_state(review=_review(consent="old")), [_system({"op": "review_begin", "run_id": "r"})]],
    [_state(cloud_enabled=False), [_system({"op": "review_begin", "run_id": "r"})]],
    [_state(status="active"), [_system({"op": "review_begin", "run_id": "r"})]],
    [_state(finalization={"status": "running"}),
     [_system({"op": "review_begin", "run_id": "r"})]],
    [_state(), [_host({"op": "review_begin", "run_id": "r"}),
                _as("agent", {"op": "review_begin", "run_id": "r", "evidence": ["sg_1"]}),
                _system({"op": "review_begin"}),
                _system({"op": "review_begin", "run_id": "r"}),
                _system({"op": "review_begin", "run_id": "r2"}),
                _system({"op": "review_unavailable"}),
                _host({"op": "review_unavailable"})]],
    # Finish: stale runs, revoked consent, assessments and questions.
    [_state(cards=_ACTIONS, review=_review([
        _question("rv_kept", "it_k", status="answered"),
        _question("rv_skip", "it_a", status="skipped"),
        _question("rv_open", "it_c")], status="running", run_id="r")), [
        _host({"op": "review_finish", "run_id": "r", "status": "ready"}),
        _system({"op": "review_finish", "run_id": "other", "status": "ready"}),
        _system({"op": "review_finish", "run_id": "r", "status": "ready", "message": "Done",
                 "assessments": [_assessment("it_a"), _assessment("it_b"),
                                 _assessment("it_c"), _assessment("it_p"),
                                 _assessment("it_r"), _assessment("it_v", revision=1),
                                 _assessment("it_missing"), _assessment("it_k", revision=1.0)],
                 "questions": [_question("rv_new_a", "it_a"), _question("rv_new_b", "it_b"),
                               _question("rv_new_a2", "it_a", field="owner"),
                               _question("rv_new_c", "it_c", field="owner"),
                               _question("rv_new_p", "it_p"),
                               _question("rv_new_k", "it_k", field="resolution")]}),
        _system({"op": "review_finish", "run_id": "r", "status": "ready"}),
    ]],
    [_state(cards=_ACTIONS, review=_review([_question("rv_open", "it_c")], status="running",
                                           run_id="r")), [
        _system({"op": "review_finish", "run_id": "r", "status": "failed",
                 "message": "The review could not finish.",
                 "assessments": [_assessment("it_a")],
                 "questions": [_question("rv_new_a", "it_a")]})]],
    [_state(cards=_ACTIONS, review=_review(status="running", run_id="r")), [
        _system({"op": "review_finish", "run_id": "r", "status": "partial",
                 "questions": [_question("rv_new_a", "it_a")],
                 "assessments": [_assessment("it_a")]})]],
    [_state(cards=_ACTIONS, cloud_enabled=False, review=_review(status="running", run_id="r")),
     [_system({"op": "review_finish", "run_id": "r", "status": "ready"})]],
    [_state(review=_review([_question(f"rv_{n}", "it_a", status="answered")
                            for n in range(55)], status="running", run_id="r")),
     [_system({"op": "review_finish", "run_id": "r", "status": "ready"})]],
    [_state(cards=_ACTIONS, review=_review(status="running", run_id="r")),
     [_system({"op": "review_finish", "run_id": "r", "assessments": [_assessment("it_a")]})]],
    # Answers.
    [_ANSWERABLE, [
        _as("user", _answer("rv_a", "offered")),
        _host(_answer("rv_nope", "offered")),
        _host(_answer("rv_sup", "offered")),
        _host(_answer("rv_a", "accurate")),
        _host(_answer("rv_a", "offered", text="   ")),
        _host(_answer("rv_a", "offered", text=5)),
        _host(_answer("rv_a", "offered", text="t" * 3501)),
        _host(_answer("rv_a", "offered")),
        _host(_answer("rv_a", "offered")),
        _host({"op": "review_reopen", "question_id": "rv_a"}),
        _host(_answer("rv_a", "accepted")),
        _host({"op": "review_reopen", "question_id": "rv_a"}),
        _host(_answer("rv_a", "incorrect", text="Nobody will send links")),
        _host(_answer("rv_c", "edit", owner_participant_id="p_alex")),
        _host(_answer("rv_c", "edit", owner_participant_id="p_nobody",
                      text="Someone drafts it")),
        _host(_answer("rv_c", "edit", owner_participant_id=5, text="Someone drafts it")),
        _host(_answer("rv_c", "edit", owner_name="n" * 121, text="Someone drafts it")),
        _host(_answer("rv_c", "edit", owner_name=None, text="Someone drafts it")),
        _host(_answer("rv_c", "edit", owner_participant_id="p_alex",
                      text="Alex drafts the renewal note")),
        _host({"op": "review_skip", "question_id": "rv_k"}),
        _host({"op": "review_skip", "question_id": "rv_k"}),
        _host({"op": "review_reopen", "question_id": "rv_k"}),
        _host(_answer("rv_k", "resolved")),
        _host({"op": "review_reopen", "question_id": "rv_k"}),
        _host(_answer("rv_k", "open")),
        _host(_answer("rv_v", "accurate")),
    ]],
    [_state(cards={"action_items": [_item("it_d", "action_items", "Ship by Friday",
                                          data={"due_date": "Friday", "owner_name": "Sam"})]},
            review=_review([_question("rv_d", "it_d", field="deadline"),
                            _question("rv_o", "it_d", field="owner")])), [
        _host(_answer("rv_d", "edit", deadline=7, text="x")),
        _host(_answer("rv_d", "edit", deadline="d" * 121, text="x")),
        _host(_answer("rv_d", "edit", deadline="Monday")),
        _host(_answer("rv_d", "edit", deadline=" Friday ")),
        _host(_answer("rv_o", "edit", owner_name=" Robin ", text="Robin ships it")),
    ]],
    [_state(cards={"action_items": [_item("it_d", "action_items", "Ship by Friday",
                                          data={"blob": "x" * 4080})]},
            review=_review([_question("rv_d", "it_d")])),
     [_host(_answer("rv_d", "offered"))]],
    [_state(status="active", cards=_ACTIONS, review=_review([_question("rv_a", "it_a")])),
     [_host(_answer("rv_a", "offered"), {"op": "review_skip", "question_id": "rv_a"})]],
    [_LONG_NOTE, [_host(_answer("rv_a", "offered"))]],
    [_state(cards={"action_items": [_item("it_a", "action_items", "Send the paper links")]},
            review=_review([_question("rv_a", "it_a")])),
     [_host(_answer("rv_a", "offered"), {"op": "review_reopen", "question_id": "rv_a"},
            _answer("rv_a", "accepted"), {"op": "review_reopen", "question_id": "rv_a"},
            _answer("rv_a", "offered"), {"op": "review_reopen", "question_id": "rv_a"},
            _answer("rv_a", "accepted"), {"op": "review_reopen", "question_id": "rv_a"},
            _answer("rv_a", "offered"), {"op": "review_reopen", "question_id": "rv_a"},
            _answer("rv_a", "accepted"), {"op": "review_reopen", "question_id": "rv_a"},
            _answer("rv_a", "offered"), {"op": "review_reopen", "question_id": "rv_a"},
            _answer("rv_a", "accepted"), {"op": "review_reopen", "question_id": "rv_a"},
            _answer("rv_a", "offered"), {"op": "review_reopen", "question_id": "rv_a"},
            _answer("rv_a", "accepted"), {"op": "review_reopen", "question_id": "rv_a"},
            _answer("rv_a", "offered"), {"op": "review_reopen", "question_id": "rv_a"},
            _answer("rv_a", "accepted"))]],
    # A later edit supersedes the answered question everywhere (item_effect).
    [_state(cards={"action_items": [_item("it_a", "action_items", "Send the paper links")],
                   "live_notes": [_item("it_n", "live_notes", "Paper links will be sent.")]},
            review=_review([_question("rv_a", "it_a")])),
     [_host(_answer("rv_a", "offered")),
      _host({"op": "update_item", "id": "it_a", "set": {"text": "Links are optional"}}),
      _host({"op": "review_reopen", "question_id": "rv_a"})]],
]

_INVALIDATE = [
    _state(cards=_ACTIONS, review=_review([
        _question("rv_a", "it_a"), _question("rv_b", "it_b", status="answered"),
        _question("rv_c", "it_c", status="skipped")], status="ready")),
    _state(cards={"risks": [_item("it_k", "risks", "Risk", review={"state": "human"},
                                  citation_check={"status": "supported", "revision": 1}),
                            _item("it_j", "risks", "Other", review={"state": "inferred"},
                                  citation_check={"status": "missing"})]},
           review=_review([_question("rv_k", "it_k")], enabled=False)),
    _state(cards={"risks": [_item("it_j", "risks", "Other", review={"state": "provisional"})]}),
]

CASES = {
    "script": [[initial, batches] for initial, batches in SCRIPTS],
    "invalidate_checks": [[state] for state in _INVALIDATE],
}

WRAPPERS = {
    "script": _script,
    "invalidate_checks": _invalidate,
}

PINS = {
    "now_utc": NOW_UTC.isoformat(),
    "ids": "new_id gives <prefix>_<n as 12 hex digits>, n counting from 1 per script",
}
