"""Op scripts for meeting/state/patches.py (M7.3 meeting-state-model).

    script(initial, batches)
        initial is a MeetingState.to_dict() document, or None for MeetingState("m_test").
        Each batch is [actor_type, actor_id, known, pinned, ops]: known lists the segment ids
        segment_exists accepts (None: no predicate), pinned the ids segment_pinned reports
        pinned (None: no predicate; "!raise" in it makes the predicate raise). The result is
        {"results": [[OpResult fields per op] per batch], "state": the final to_dict()}.
    filter_notes_ops(ops, known_ids)    known_ids None or a list; the kept ops
    live_note_ids(snapshot)             sorted ids
    item_text_too_similar(left, right, jaccard, containment)
    normalize_item_text(text)
    check_data_size(data), check_text(value, max_len)

Ids and time are pinned: new_id counts "<prefix>_000000000001", "..._000000000002" (one
counter per script, testing/fixed_ids.h), and now is 2026-01-01T00:00:00Z (FakeClock's
default), through meeting.state.schema.uuid and meeting.time_utils.datetime.
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


def _result(r):
    return copy.deepcopy({
        "ok": r.ok, "op": r.op, "reason": r.reason, "target_id": r.target_id,
        "effect": r.effect, "inverse": r.inverse, "current_revision": r.current_revision,
        "seq": r.seq,
    })


def _predicate(ids, raises=False):
    if ids is None:
        return None
    known = frozenset(ids)

    def check(segment_id):
        if raises:
            raise RuntimeError("lookup failed")
        return segment_id in known

    return check


def _script(module, initial, batches):
    import meeting.state.schema as schema
    import meeting.time_utils as time_utils

    saved_uuid, saved_datetime = schema.uuid, time_utils.datetime
    schema.uuid, time_utils.datetime = _FixedUuid(), _Pinned
    try:
        state = (schema.MeetingState(meeting_id="m_test") if initial is None
                 else schema.MeetingState.from_dict(initial))
        results = []
        for actor_type, actor_id, known, pinned, ops in batches:
            raises = pinned is not None and "!raise" in pinned
            ctx = module.OpContext(actor_type, actor_id, segment_exists=_predicate(known),
                                   segment_pinned=_predicate(pinned, raises))
            results.append([_result(r) for r in module.apply_ops(state, ops, ctx)])
        return {"results": results, "state": state.to_dict()}
    finally:
        schema.uuid, time_utils.datetime = saved_uuid, saved_datetime


def _filter(module, ops, known_ids):
    return module.filter_notes_ops(ops, None if known_ids is None else frozenset(known_ids))


def _note_ids(module, snapshot):
    return sorted(module.live_note_ids(snapshot))


SEGMENTS = ["sg_1", "sg_2", "sg_3", "sg_4"]


def _host(*ops, known=SEGMENTS, pinned=None):
    return ["host", "p_host", known, pinned, list(ops)]


def _user(*ops, known=SEGMENTS, pinned=None):
    return ["user", "p_guest", known, pinned, list(ops)]


def _agent(*ops, known=SEGMENTS, pinned=None):
    return ["agent", "agent", known, pinned, list(ops)]


def _system(*ops, known=SEGMENTS, pinned=None, actor="state_repair"):
    return ["system", actor, known, pinned, list(ops)]


def _add(card, text, evidence=("sg_1",), **extra):
    op = {"op": "add_item", "card": card, "text": text}
    if evidence is not None:
        op["evidence"] = list(evidence)
    op.update(extra)
    return op


def _item(item_id, card, text, **extra):
    item = {"id": item_id, "card": card, "text": text, "data": {}, "status": "proposed",
            "author_type": "agent", "author_id": "agent", "pinned": False, "revision": 1,
            "evidence": ["sg_1"], "created_at": "2025-12-31T00:00:00Z",
            "updated_at": "2025-12-31T00:00:00Z", "review": {}, "citation_check": {}}
    item.update(extra)
    return item


def _state(cards=None, **extra):
    doc = {"meeting_id": "m_test", "cards": cards or {}}
    doc.update(extra)
    return doc


def _fingerprint(text):
    value = 0xCBF29CE484222325
    for byte in text.encode("utf-8"):
        value = ((value ^ byte) * 0x100000001B3) & 0xFFFFFFFFFFFFFFFF
    return f"fnv1a64:{value:016x}"


def _sized(length, accents=100):
    """{"note": "é"*accents + "x"*n} whose json.dumps is `length` characters long."""
    return {"note": "é" * accents + "x" * (length - 12 - 6 * accents)}


_REVIEW = {
    "enabled": True,
    "questions": [
        {"id": "rq_1", "item_id": "it_k", "note_id": "it_note", "superseded": False},
        {"id": "rq_2", "item_id": "it_k", "superseded": True},
        {"id": "rq_3", "item_id": "it_other"},
    ],
}

_REVIEW_STATE = _state(
    cards={
        "key_points": [_item("it_k", "key_points", "Vendor A wins")],
        "live_notes": [_item(
            "it_live", "live_notes",
            "Notes\n\nUser clarification (supersedes earlier wording on this point): "
            "Vendor B, not A")],
        "user_notes": [_item("it_note", "user_notes", "Vendor B, not A", status="edited",
                             author_type="user", author_id="p_host")],
    },
    insight_review=_REVIEW,
)

_BIG_CARD = _state(cards={"risks": [_item(f"it_{n}", "risks", f"Risk number {n}")
                                    for n in range(500)]})
_BIG_PEOPLE = _state(participants={
    f"p_{n}": {"id": f"p_{n}", "display_name": f"Speaker {n}", "kind": "others_cluster",
               "name_source": "default", "is_provisional": True,
               "created_at": "2025-12-31T00:00:00Z", "updated_at": "2025-12-31T00:00:00Z"}
    for n in range(200)})
_OPEN_QUESTIONS = _state(questions=[{"id": f"q_{n}", "text": f"Question {n}?"}
                                    for n in range(50)])

_PEOPLE = _state(participants={
    "p_me": {"id": "p_me", "display_name": "Me", "kind": "me", "name_source": "human",
             "is_provisional": False},
    "p_s1": {"id": "p_s1", "display_name": "Speaker 1", "kind": "others_cluster",
             "name_source": "default", "is_provisional": True},
    "p_named": {"id": "p_named", "display_name": "Dana", "kind": "others_cluster",
                "name_source": "human", "is_provisional": False},
})

SCRIPTS = [
    # Malformed and unknown ops, and the gates in apply_ops order.
    [None, [_host("set_topic", 5, ["add_item"], {"card": "key_points"}, {"op": "bogus"},
                  {"op": 5}, {"op": None}, {"op": True})]],
    [None, [_host({"op": "set_topic", "text": "Applied"}, {"op": ["add_item"]})]],
    [None, [_agent({"op": "pin_item", "id": "x"}, {"op": "set_title", "text": "T"},
                   {"op": "add_item", "card": "key_points", "text": "No evidence"},
                   {"op": "set_topic", "text": "Bad", "evidence": "sg_1"},
                   {"op": "set_topic", "text": "Many", "evidence": ["sg_1"] * 21},
                   {"op": "set_topic", "text": "Unknown", "evidence": ["sg_9"]},
                   {"op": "set_topic", "text": "Typed", "evidence": [5]},
                   {"op": "set_topic", "text": "Empty", "evidence": []},
                   {"op": "set_topic", "text": "Twenty", "evidence": ["sg_1"] * 20})]],
    [None, [_user({"op": "resolve_question", "question_id": "q"},
                  {"op": "revise_segment_text", "segment_id": "sg_1", "text": "x"},
                  {"op": "set_topic", "text": "Guest topic"},
                  {"op": "set_rolling_summary", "text": "Guest summary"},
                  {"op": "set_title", "text": "Guest title"},
                  {"op": "set_meeting_intent", "text": "Guest brief"},
                  {"op": "set_cloud_enabled", "enabled": True}),
            _host({"op": "resolve_question", "question_id": "q"},
                  {"op": "set_cloud_enabled", "enabled": 1},
                  {"op": "set_cloud_enabled"},
                  {"op": "set_title", "text": "  Vendor review  "},
                  {"op": "set_title", "text": "Second title"},
                  {"op": "set_title", "text": "bad\x01title"},
                  {"op": "set_title", "text": "a" * 201},
                  {"op": "set_title"})]],
    # Adds: cards, text, data and duplicate rules.
    [None, [_agent(_add("key_points", "Budget approved for the Q3 launch"),
                   _add("key_points", "budget APPROVED for the q3 launch!!"),
                   _add("key_points", "The Q3 launch budget was approved by finance today"),
                   _add("key_points", "Budget approved"),
                   _add("decisions", "Budget approved for the Q3 launch."),
                   _add("decisions", "We approved the budget for the Q3 launch and hiring"),
                   _add("live_notes", "Budget approved for the Q3 launch",
                        data={"heading": "Budget", "start_s": 4}),
                   _add("user_notes", "Agent note"),
                   _add("timeline", "Kickoff"),
                   _add("timeline", "Kickoff", data={"start_s": "12.5"}),
                   _add("timeline", "Bad", data={"start_s": -1}),
                   _add("timeline", "Words", data={"start_s": "soon"}),
                   _add("timeline", "Flag", data={"start_s": True}),
                   _add("timeline", "Null", data={"start_s": None}),
                   _add("bogus", "x"), _add(None, "x"),
                   _add("risks", "   "), _add("risks", 5),
                   _add("risks", "x" * 4001), _add("risks", "é" * 4000),
                   _add("risks", "\U0001F600" * 4000), _add("risks", "\U0001F600" * 4001),
                   _add("risks", "Data list", data=[1]), _add("risks", "Data empty", data=[]),
                   _add("risks", "Data zero", data=0),
                   _add("risks", "Data size", data=_sized(4096)),
                   _add("risks", "Data size over", data=_sized(4097)),
                   _add("risks", "Astral data", data={"e": "\U0001F600" * 340}),
                   _add("risks", "Astral data over", data={"e": "\U0001F600" * 341})),
            _host(_add("key_points", "Budget approved for the Q3 launch", evidence=None),
                  _add("timeline", "Human beat", evidence=None),
                  _add("user_notes", "Host note", evidence=None, data={"kind": "agent_insight"}),
                  _add("risks", "Evidence string", evidence="sg_1"))]],
    [_state(cards={
        "key_points": [_item("it_r", "key_points", "Removed point about hiring", status="removed")],
        "user_notes": [_item("it_u", "user_notes", "Hiring plan is frozen", author_type="user")],
        "live_notes": [_item("it_l", "live_notes", "Hiring plan is frozen until spring")],
    }), [_agent(_add("key_points", "Removed point about hiring"),
                _add("key_points", "Hiring plan is frozen"),
                _add("decisions", "Hiring plan is frozen until spring"))]],
    [_BIG_CARD, [_host(_add("risks", "One more risk", evidence=None)),
                 _agent(_add("risks", "Risk number 7"))]],
    # Updates: revision gating, protection, evidence union, live notes, inverses.
    [_state(cards={
        "key_points": [_item("it_a", "key_points", "Alpha", evidence=["sg_1"], revision=3),
                       _item("it_p", "key_points", "Pinned", pinned=True),
                       _item("it_c", "key_points", "Confirmed", status="confirmed"),
                       _item("it_v", "key_points", "Spoken", author_type="system",
                             author_id="voice_command"),
                       _item("it_e", "key_points", "No evidence", evidence=[]),
                       _item("it_s", "key_points", "Sample", author_type="system",
                             author_id="state_repair", data={"source": "repair"}),
                       _item("it_s2", "key_points", "Sample two", author_type="system",
                             author_id="state_repair",
                             data={"insight_synthesized": True})],
        "live_notes": [_item("it_n", "live_notes", "Note body",
                             data={"heading": "Budget", "start_s": 41.0, "x": 1})],
    }), [
        _agent({"op": "update_item", "id": "it_a", "set": {"text": "Alpha 2"}, "evidence": ["sg_2"]},
               {"op": "update_item", "id": "it_a", "base_revision": 3, "set": {"text": " Alpha 2 "},
                "evidence": ["sg_2", "sg_1", "sg_3"]},
               {"op": "update_item", "id": "it_a", "base_revision": 4.0,
                "set": {"data": {"n": 1}}, "evidence": ["sg_1"]},
               {"op": "update_item", "id": "it_p", "base_revision": 1, "set": {"text": "x"},
                "evidence": ["sg_1"]},
               {"op": "update_item", "id": "it_c", "base_revision": 1, "set": {"text": "x"},
                "evidence": ["sg_1"]},
               {"op": "update_item", "id": "it_v", "base_revision": 1, "set": {"text": "x"},
                "evidence": ["sg_1"]},
               {"op": "update_item", "id": "it_s", "base_revision": True,
                "set": {"text": "Synthesized"}, "evidence": ["sg_1"]},
               {"op": "update_item", "id": "it_s2", "base_revision": 1,
                "set": {"data": {"y": 2}}, "evidence": ["sg_1"]},
               {"op": "update_item", "id": "it_n", "base_revision": 1,
                "set": {"data": {"x": 2}}, "evidence": ["sg_2"]},
               {"op": "update_item", "id": "missing", "evidence": ["sg_1"]},
               {"op": "update_item", "id": "it_e", "base_revision": 1, "set": {},
                "evidence": ["sg_1"]},
               {"op": "remove_item", "id": "it_e", "base_revision": 2, "evidence": ["sg_1"]}),
        _host({"op": "update_item", "id": "it_e", "set": {"text": "Host words"}},
              {"op": "update_item", "id": "it_e", "set": {"text": "  "}},
              {"op": "update_item", "id": "it_e", "set": {"data": "bad"}},
              {"op": "update_item", "id": "it_e", "set": {"data": _sized(4097)}},
              {"op": "update_item", "id": "it_e", "set": "not a dict"},
              {"op": "update_item", "id": "it_e", "set": {}, "restore_status": "proposed"},
              {"op": "update_item", "id": "it_e", "set": {"text": "With evidence"},
               "evidence": ["sg_9"]},
              {"op": "update_item", "id": "it_n", "set": {"data": {"x": 3}}, "force": True},
              {"op": "update_item", "id": 5, "set": {"text": "x"}}),
        _system({"op": "update_item", "id": "it_a", "set": {"text": "Restored",
                 "data": {}, "evidence": ["sg_4"]}, "restore_status": "proposed",
                 "force": True},
                {"op": "update_item", "id": "it_n", "set": {"data": {"only": 1}},
                 "force": True},
                {"op": "update_item", "id": "it_c", "set": {"text": "System edit"}},
                {"op": "update_item", "id": "it_c", "set": {}, "restore_status": "edited"}),
    ]],
    # Remove, pin, confirm: soft delete and the quirks of A28-51.
    [_state(cards={"risks": [_item("it_1", "risks", "Risk one", revision=2),
                             _item("it_2", "risks", "Risk two", status="edited")]}), [
        _agent({"op": "remove_item", "id": "it_1", "base_revision": 1, "evidence": ["sg_1"]},
               {"op": "remove_item", "id": "it_2", "base_revision": 1, "evidence": ["sg_1"]},
               {"op": "remove_item", "id": "it_1", "base_revision": 2, "evidence": ["sg_1"]},
               {"op": "remove_item", "id": "it_1", "base_revision": 3, "evidence": ["sg_1"]}),
        _user({"op": "pin_item", "id": "it_1"}, {"op": "confirm_item", "id": "it_1"},
              {"op": "unpin_item", "id": "it_1"}, {"op": "pin_item", "id": "nope"},
              {"op": "confirm_item"}, {"op": "remove_item", "id": "it_2"},
              {"op": "remove_item", "id": "it_2"}),
    ]],
    # Review questions superseded by an edit (review.py item_effect).
    [_REVIEW_STATE, [_host({"op": "update_item", "id": "it_k", "set": {"text": "Vendor B wins"}})]],
    [_REVIEW_STATE, [_host({"op": "remove_item", "id": "it_k"})]],
    [_state(cards=_REVIEW_STATE["cards"], insight_review={"questions": None}),
     [_host({"op": "update_item", "id": "it_k", "set": {"text": "x"}})]],
    [_state(cards=_REVIEW_STATE["cards"], insight_review={"questions": [{"id": "rq"}]}),
     [_host({"op": "update_item", "id": "it_k", "set": {"text": "x"}})]],
    # Topic, rolling summary, intent.
    [None, [_agent({"op": "set_topic", "text": " First topic ", "evidence": ["sg_1"]},
                   {"op": "set_topic", "text": "Second topic", "evidence": ["sg_2", "sg_3"]},
                   {"op": "set_topic", "text": "t" * 501, "evidence": ["sg_1"]},
                   {"op": "set_rolling_summary", "text": "Summary", "evidence": ["sg_1"]},
                   {"op": "set_rolling_summary", "text": "s" * 8001, "evidence": ["sg_1"]},
                   {"op": "set_rolling_summary", "text": "   ", "evidence": ["sg_1"]}),
            _host({"op": "set_rolling_summary", "text": " Host summary "},
                  {"op": "set_rolling_summary"},
                  {"op": "set_rolling_summary", "text": 5},
                  {"op": "set_topic", "text": "Host topic"}),
            _system({"op": "set_rolling_summary", "text": ""},
                    {"op": "set_rolling_summary", "text": "é" * 8000, "evidence": ["sg_1"]})]],
    [None, [_host({"op": "set_meeting_intent", "text": " Decide the vendor "}),
            _user({"op": "set_meeting_intent", "text": "Only my bits"}),
            _agent({"op": "set_meeting_intent", "text": "Capture everything",
                    "evidence": ["sg_1"]}),
            _host({"op": "set_meeting_intent", "text": "x" * 2001},
                  {"op": "set_meeting_intent", "text": "é" * 2000},
                  {"op": "set_meeting_intent", "text": "  "},
                  {"op": "set_meeting_intent"},
                  {"op": "set_meeting_intent", "text": None}),
            ["host", None, None, None, [{"op": "set_meeting_intent", "text": "No author"}]],
            _system({"op": "set_meeting_intent", "text": "System brief"}, actor="")]],
    # Participants.
    [_PEOPLE, [
        _agent({"op": "upsert_participant", "display_name": " Alex ", "evidence": ["sg_1"]},
               {"op": "upsert_participant", "display_name": "Fake me", "kind": "me",
                "evidence": ["sg_1"]},
               {"op": "upsert_participant", "display_name": "Null kind", "kind": None,
                "evidence": ["sg_1"]},
               {"op": "upsert_participant", "id": "p_s1", "display_name": "Sam",
                "evidence": ["sg_1"]},
               {"op": "upsert_participant", "id": "p_named", "display_name": "Not Dana",
                "evidence": ["sg_1"]},
               {"op": "upsert_participant", "id": "p_zz", "display_name": "Ghost",
                "evidence": ["sg_1"]},
               {"op": "suggest_participant_name", "participant_id": "p_s1",
                "display_name": "Samuel", "evidence": ["sg_2"]},
               {"op": "suggest_participant_name", "participant_id": "p_named",
                "display_name": "D", "evidence": ["sg_2"]},
               {"op": "suggest_participant_name", "participant_id": "p_s1",
                "display_name": "n" * 121, "evidence": ["sg_2"]},
               {"op": "suggest_participant_name", "display_name": "Nobody",
                "evidence": ["sg_2"]},
               {"op": "rename_participant", "participant_id": "p_s1", "display_name": "X",
                "evidence": ["sg_2"]}),
        _user({"op": "upsert_participant", "display_name": "Guest", "kind": "guest"},
              {"op": "upsert_participant", "display_name": "Second me", "kind": "me"},
              {"op": "upsert_participant", "display_name": "Robot", "kind": "robot"},
              {"op": "upsert_participant", "display_name": "Typed kind", "kind": 5},
              {"op": "upsert_participant", "display_name": "Cluster",
               "is_provisional": "yes"},
              {"op": "upsert_participant", "id": "p_named", "display_name": " Dana Lee "},
              {"op": "upsert_participant", "id": 7, "display_name": "Seven"},
              {"op": "upsert_participant", "display_name": ""},
              {"op": "rename_participant", "participant_id": "p_s1", "display_name": " Sam B "},
              {"op": "rename_participant", "participant_id": "p_s1",
               "display_name": "Sam C", "_name_source": "agent_inferred"},
              {"op": "rename_participant", "participant_id": "nope", "display_name": "X"},
              {"op": "rename_participant", "participant_id": "p_s1", "display_name": "  "}),
        _system({"op": "upsert_participant", "display_name": "Host", "kind": "me"},
                {"op": "upsert_participant", "display_name": "Joined guest", "kind": "guest",
                 "is_provisional": True},
                {"op": "rename_participant", "participant_id": "p_s1",
                 "display_name": "Speaker 1", "_name_source": "default"},
                {"op": "rename_participant", "participant_id": "p_named",
                 "display_name": "Dana"}),
    ]],
    [_BIG_PEOPLE, [_host({"op": "upsert_participant", "display_name": "Overflow"},
                         {"op": "upsert_participant", "id": "p_3", "display_name": "Renamed"})]],
    # Questions: caps, thresholds, lifecycle.
    [None, [
        _agent(*[{"op": "ask_question", "text": f"Agent question {n}?", "evidence": ["sg_1"]}
                 for n in range(8)]),
        _system({"op": "ask_question", "text": "System question?"}),
        _agent({"op": "resolve_question", "question_id": "q_000000000001",
                "answer_text": " Yes ", "confidence": 0.8, "evidence": ["sg_2", "sg_1"]},
               {"op": "resolve_question", "question_id": "q_000000000001",
                "answer_text": "Again", "confidence": 0.9, "evidence": ["sg_2"]},
               {"op": "resolve_question", "question_id": "q_000000000002",
                "answer_text": "Maybe", "confidence": "0.5", "evidence": ["sg_2"]},
               {"op": "resolve_question", "question_id": "q_000000000003",
                "answer_text": "Unsure", "confidence": 0.39999, "evidence": ["sg_2"]},
               {"op": "resolve_question", "question_id": "q_000000000003",
                "answer_text": "Unsure", "confidence": "high", "evidence": ["sg_2"]},
               {"op": "resolve_question", "question_id": "q_000000000003",
                "answer_text": "Unsure", "evidence": ["sg_2"]},
               {"op": "resolve_question", "question_id": "q_000000000003",
                "answer_text": "Unsure", "confidence": None, "evidence": ["sg_2"]},
               {"op": "resolve_question", "question_id": "q_000000000003",
                "answer_text": "  ", "confidence": 0.9, "evidence": ["sg_2"]},
               {"op": "resolve_question", "question_id": "q_000000000004",
                "answer_text": "Sure", "confidence": True, "evidence": ["sg_2"]},
               {"op": "resolve_question", "question_id": "q_zzz", "answer_text": "x",
                "confidence": 1, "evidence": ["sg_2"]},
               {"op": "answer_question", "question_id": "q_000000000002",
                "answer_text": "x", "evidence": ["sg_2"]}),
        _user({"op": "answer_question", "question_id": "q_000000000002",
               "answer_text": " Thursday "},
              {"op": "answer_question", "question_id": "q_000000000002",
               "answer_text": "Friday"},
              {"op": "answer_question", "question_id": "q_000000000005", "answer_text": ""},
              {"op": "dismiss_question", "question_id": "q_000000000005"},
              {"op": "dismiss_question", "question_id": "q_000000000005"},
              {"op": "dismiss_question", "question_id": "q_000000000002"},
              {"op": "reopen_question", "question_id": "q_000000000002"},
              {"op": "reopen_question", "question_id": "q_000000000002"},
              {"op": "reopen_question", "question_id": "q_zzz"},
              {"op": "ask_question", "text": "Guest question?"}),
        _agent({"op": "dismiss_question", "question_id": "q_000000000006",
                "evidence": ["sg_1"]}),
    ]],
    [_OPEN_QUESTIONS, [_user({"op": "ask_question", "text": "One too many?"}),
                       _host({"op": "dismiss_question", "question_id": "q_0"},
                             {"op": "ask_question", "text": "Now it fits?",
                              "evidence": ["sg_1", "sg_1"]})]],
    # One-occurrence corrections (user_notes data validation).
    [None, [
        _host(_add("user_notes", "Correction", data={
            "kind": "occurrence_correction", "selected_text": "Entropic",
            "replacement": "Anthropic", "occurrence_index": 0,
            "base_fingerprint": _fingerprint("Entropic ships")})),
        _host(_add("user_notes", "Bad index", data={
            "kind": "occurrence_correction", "selected_text": "a", "replacement": "b",
            "occurrence_index": True, "base_fingerprint": _fingerprint("x")})),
        _host(_add("user_notes", "Float index", data={
            "kind": "occurrence_correction", "selected_text": "a", "replacement": "b",
            "occurrence_index": 1.0, "base_fingerprint": _fingerprint("x")}),
              _add("user_notes", "Big index", data={
                  "kind": "occurrence_correction", "selected_text": "a", "replacement": "b",
                  "occurrence_index": 1001, "base_fingerprint": _fingerprint("x")}),
              _add("user_notes", "Edge index", data={
                  "kind": "occurrence_correction", "selected_text": "a", "replacement": "b",
                  "occurrence_index": 1000, "base_fingerprint": _fingerprint("x")}),
              _add("user_notes", "Negative index", data={
                  "kind": "occurrence_correction", "selected_text": "a", "replacement": "b",
                  "occurrence_index": -1, "base_fingerprint": _fingerprint("x")}),
              _add("user_notes", "Bad fingerprint", data={
                  "kind": "occurrence_correction", "selected_text": "a", "replacement": "b",
                  "occurrence_index": 0, "base_fingerprint": "fnv1a64:ABCDEF0123456789"}),
              _add("user_notes", "Long term", data={
                  "kind": "occurrence_correction", "selected_text": "t" * 121,
                  "replacement": "b", "occurrence_index": 0,
                  "base_fingerprint": _fingerprint("x")}),
              _add("user_notes", "Two segments", evidence=["sg_1", "sg_2"], data={
                  "kind": "occurrence_correction", "selected_text": "a", "replacement": "b",
                  "occurrence_index": 0, "base_fingerprint": _fingerprint("x")}),
              _add("user_notes", "Unknown segment", evidence=["sg_9"], data={
                  "kind": "occurrence_correction", "selected_text": "a", "replacement": "b",
                  "occurrence_index": 0, "base_fingerprint": _fingerprint("x")}),
              _add("user_notes", "Empty segment", evidence=[""], data={
                  "kind": "occurrence_correction", "selected_text": "a", "replacement": "b",
                  "occurrence_index": 0, "base_fingerprint": _fingerprint("x")})),
        ["host", "p_host", None, None, [_add("user_notes", "No predicate", data={
            "kind": "occurrence_correction", "selected_text": "a", "replacement": "b",
            "occurrence_index": 0, "base_fingerprint": _fingerprint("x")})]],
        _system(_add("user_notes", "System correction", data={
            "kind": "occurrence_correction", "selected_text": "a", "replacement": "b",
            "occurrence_index": 0, "base_fingerprint": _fingerprint("x")})),
    ]],
    # Segment ops.
    [_state(participants=_PEOPLE["participants"], cards={"user_notes": [
        _item("it_occ", "user_notes", "Correction", author_type="user", status="edited",
              evidence=["sg_3"], data={"kind": "occurrence_correction"}),
        _item("it_gone", "user_notes", "Removed correction", author_type="user",
              status="removed", evidence=["sg_4"], data={"kind": "occurrence_correction"})]}), [
        _user({"op": "reassign_segment_speaker", "segment_id": "sg_1", "participant_id": "p_s1"},
              {"op": "reassign_segment_speaker", "segment_id": "sg_1", "participant_id": None},
              {"op": "reassign_segment_speaker", "segment_id": "sg_1"},
              {"op": "reassign_segment_speaker", "segment_id": "sg_1", "participant_id": "p_x"},
              {"op": "reassign_segment_speaker", "segment_id": "sg_9", "participant_id": "p_s1"},
              {"op": "reassign_segment_speaker", "segment_id": "", "participant_id": "p_s1"},
              {"op": "reassign_segment_speaker", "segment_id": 5},
              pinned=["sg_1"]),
        _system({"op": "reassign_segment_speaker", "segment_id": "sg_1",
                 "participant_id": "p_s1"},
                {"op": "reassign_segment_speaker", "segment_id": "sg_2",
                 "participant_id": "p_s1", "_source": "channel", "_pinned": 1},
                {"op": "reassign_segment_speaker", "segment_id": "sg_1",
                 "participant_id": "p_s1", "force": True, "_source": None},
                pinned=["sg_1"]),
        _system({"op": "reassign_segment_speaker", "segment_id": "sg_2",
                 "participant_id": "p_s1"}, pinned=["!raise"]),
        _system({"op": "reassign_segment_speaker", "segment_id": "sg_2",
                 "participant_id": "p_s1"}, known=None),
        _agent({"op": "reassign_segment_speaker", "segment_id": "sg_1", "evidence": ["sg_1"]},
               {"op": "revise_segment_text", "segment_id": "sg_1", "text": " Cleaned ",
                "evidence": ["sg_1"]},
               {"op": "revise_segment_text", "segment_id": "sg_2", "text": "Other",
                "evidence": ["sg_1"]},
               {"op": "revise_segment_text", "segment_id": "sg_3", "text": "Blocked",
                "evidence": ["sg_3"]},
               {"op": "revise_segment_text", "segment_id": "sg_4", "text": "Allowed",
                "evidence": ["sg_4"]},
               {"op": "revise_segment_text", "segment_id": "sg_1", "text": "  ",
                "evidence": ["sg_1"]},
               {"op": "revise_segment_text", "segment_id": "sg_1", "text": "x" * 4001,
                "evidence": ["sg_1"]},
               {"op": "revise_segment_text", "segment_id": "sg_9", "text": "Ghost",
                "evidence": ["sg_1"]},
               {"op": "revise_segment_text", "text": "No id", "evidence": ["sg_1"]}),
        _system({"op": "revise_segment_text", "segment_id": "sg_3", "text": "Undo text"}),
    ]],
]

_NOTES_SNAPSHOT = {"cards": {"live_notes": [
    {"id": "it_a", "status": "proposed"}, {"id": "it_b", "status": "removed"},
    {"id": "it_c", "status": "edited"}, {"id": ""}, {"status": "x"}, "junk", {"id": 5}]}}

_NOTE_OPS = [
    {"op": "add_item", "card": "live_notes", "text": "keep"},
    {"op": "add_item", "card": "key_points", "text": "drop"},
    {"op": "update_item", "id": "it_note1", "set": {"text": "keep"}},
    {"op": "update_item", "id": "it_key1", "set": {"text": "drop"}},
    {"op": "remove_item", "id": "it_note2"},
    {"op": "set_topic", "text": "drop"},
    {"op": "revise_segment_text", "segment_id": "sg_1", "text": "drop"},
    "junk", {"card": "live_notes"}, {"op": "remove_item", "id": 5},
]

CASES = {
    "script": [[initial, batches] for initial, batches in SCRIPTS],
    "filter_notes_ops": [[_NOTE_OPS, ["it_note1", "it_note2"]], [_NOTE_OPS, None],
                         [_NOTE_OPS, []], [[], ["it_note1"]]],
    "live_note_ids": [[_NOTES_SNAPSHOT], [{}], [{"cards": {}}], [{"cards": None}],
                      [{"cards": {"live_notes": None}}]],
    "normalize_item_text": [[t] for t in [
        "Budget APPROVED!!", "  café crème  ", "İstanbul", "x__y--z", "", "!!!", "Ünïcödé 123",
        "ǅungla", "Straße", "K", "tab\tand\nnewline"]],
    "item_text_too_similar": [
        ["Budget approved", "budget approved!", 0.6, 0.75],
        ["Budget approved", "Budget approved for hiring", 0.6, 0.75],
        ["one two three four five", "one two three four six seven", 0.6, 0.75],
        ["one two three four", "one two three four five six seven eight", 0.6, 0.75],
        ["one two three four", "one two three four five six seven eight", 0.6, 0.9],
        ["a b c", "a b d", 0.5, 0.75], ["a b c", "a b d", 0.75, 0.9], ["", "x", 0.6, 0.75],
        ["!!!", "!!!", 0.6, 0.75], ["café", "caf", 0.6, 0.75],
    ],
    "check_data_size": [[_sized(4096)], [_sized(4097)], [{}], [{"n": 1.5, "b": None}],
                        [{"s": "\U0001F600" * 340}], [{"s": "\U0001F600" * 341}]],
    "check_text": [["ok", 4000], ["", 10], [" 　", 10], [None, 10], [5, 10],
                   ["é" * 10, 10], ["é" * 11, 10], ["\U0001F600" * 10, 10]],
}

WRAPPERS = {
    "script": _script,
    "filter_notes_ops": _filter,
    "live_note_ids": _note_ids,
    "normalize_item_text": lambda module, text: module._normalize_item_text(text),
    "item_text_too_similar": lambda module, a, b, j, c: module._item_text_too_similar(
        a, b, jaccard_threshold=j, containment_threshold=c),
    "check_data_size": lambda module, data: module._check_data_size(data),
    "check_text": lambda module, value, max_len: module._check_text(value, max_len),
}

PINS = {
    "now_utc": NOW_UTC.isoformat(),
    "ids": "new_id gives <prefix>_<n as 12 hex digits>, n counting from 1 per script",
}
