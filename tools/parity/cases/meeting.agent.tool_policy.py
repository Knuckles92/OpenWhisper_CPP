"""Curated inputs for meeting/agent/tool_policy.py (M8.2 agent-prompts-and-policy).

A scope is [pass_kind, note_ids, citable_ids]. A host is null (an object with none of the
optional tools: no segment_exists, no searches) or {"segments": [...] | null,
"past": answer | absent, "folder": answer | absent, "results": [op_result, ...]}: segment_exists
is membership in "segments" when that is a list, a search tool exists when its answer is
given, and apply_agent_ops and the question tools answer the scripted results in order, then
OpResult(ok=True, op=op).

    pass_rejection(scope, op)
    tool_result_text(name, payload)
    serialize_op_result(fields)                 OpResult(**fields)
    op_results_payload([fields, ...])
    pass_kind_for(payload), citable_segment_ids(payload), for_payload(payload)
                                                CheckpointPayload(**payload); for_payload
                                                gives [pass_kind, sorted note_ids, citable_ids]
    run_read_tool(host, name, args)             [payload, calls]
    apply_patch_ops(host, ops, scope)           [results, calls]
    run_tool(host, name, args, scope)           [payload, results, calls]
    repair_evidence(host, ops, citable_ids)     [ops, calls]
Results are [ok, op, reason, target_id, current_revision, seq]; calls are the host's calls in
order, [method, args...], with searches as [method, query, target, limit].
"""

from meeting.interfaces import CheckpointPayload, OpResult


class _Host:
    def __init__(self, spec):
        self.calls = []
        self._results = list(spec.get("results") or [])
        segments = spec.get("segments")
        if isinstance(segments, list):
            self.segment_exists = lambda sid: self._record("segment_exists", sid) or sid in segments
        if "past" in spec:
            answer = spec["past"]
            self.search_past_meetings = (
                lambda query="", meeting_id=None, limit=10:
                self._record("search_past_meetings", query, meeting_id, limit) or answer)
        if "folder" in spec:
            answer_folder = spec["folder"]
            self.search_context_files = (
                lambda query="", relative_path=None, limit=10:
                self._record("search_context_files", query, relative_path, limit)
                or answer_folder)

    def _record(self, *call):
        self.calls.append(list(call))
        return None

    def _next(self, op):
        if self._results:
            return OpResult(**self._results.pop(0))
        return OpResult(ok=True, op=op)

    def apply_agent_ops(self, ops):
        self.calls.append(["apply_agent_ops", ops])
        return [self._next(op) for op in ops]

    def ask_question(self, text, evidence):
        self.calls.append(["ask_question", text, evidence])
        return self._next({"op": "ask_question"})

    def resolve_question(self, question_id, answer_text, confidence, evidence):
        self.calls.append(["resolve_question", question_id, answer_text, confidence, evidence])
        return self._next({"op": "resolve_question"})


class _Bare:
    """A host with the required methods only."""

    def __init__(self):
        self.calls = []

    def apply_agent_ops(self, ops):
        self.calls.append(["apply_agent_ops", ops])
        return [OpResult(ok=True, op=op) for op in ops]

    def ask_question(self, text, evidence):
        self.calls.append(["ask_question", text, evidence])
        return OpResult(ok=True, op={"op": "ask_question"})

    def resolve_question(self, question_id, answer_text, confidence, evidence):
        self.calls.append(["resolve_question", question_id, answer_text, confidence, evidence])
        return OpResult(ok=True, op={"op": "resolve_question"})


def _host(spec):
    return _Bare() if spec is None else _Host(spec)


def _scope(module, scope):
    kind, note_ids, citable = scope
    return module.ToolScope(pass_kind=kind, note_ids=frozenset(note_ids),
                            citable_ids=tuple(citable))


def _result(result):
    return [result.ok, result.op, result.reason, result.target_id, result.current_revision,
            result.seq]


def _payload(fields):
    return CheckpointPayload(**fields)


_KNOWN = ["sg_5f2c721de88ac0aa2321", "sg_e991e94d6decd27d036a", "sg_025f2c0b2c30e3e8dcb3"]
_CARDS = ["cards", [], []]
_CARDS_CITABLE = ["cards", [], _KNOWN]
_NOTES = ["notes", ["it_note1"], _KNOWN]
_POLISH = ["polish", [], _KNOWN]
_FINAL = ["consolidation", [], _KNOWN]

_OPS = [
    {"op": "add_item", "card": "live_notes", "text": "new block", "evidence": ["sg_e991e94d6decd27d03"]},
    {"op": "add_item", "card": "key_points", "text": "card", "evidence": ["sg_1"]},
    {"op": "update_item", "id": "it_note1", "base_revision": 1, "set": {"text": "x"},
     "evidence": ["sg_1"]},
    {"op": "update_item", "id": "it_keypoint", "base_revision": 1, "set": {"text": "x"},
     "evidence": ["sg_1"]},
    {"op": "remove_item", "id": "it_note1", "base_revision": 1, "evidence": ["sg_1"]},
    {"op": "set_topic", "text": "topic", "evidence": ["sg_1"]},
    {"op": "revise_segment_text", "segment_id": "sg_1", "text": "hello", "evidence": ["sg_1"]},
    {"op": "ask_question", "text": "q", "evidence": ["sg_1"]},
    "junk",
    None,
    {"text": "no op"},
]

_SEGMENTS_HOST = {"segments": _KNOWN}
_SCRIPTED = {"segments": _KNOWN, "results": [
    {"ok": False, "op": {"op": "x"}, "reason": "revision_mismatch", "target_id": "it_1",
     "current_revision": 4},
    {"ok": True, "op": {"op": "y"}, "target_id": "it_2", "seq": 17},
]}
_SEARCH_HOST = {"past": {"ok": True, "text": "past:m_old:1 excerpt", "hits": [{"ref": "p"}]},
                "folder": "plain folder text"}
_EMPTY_SEARCH = {"past": None, "folder": {"ok": True, "text": "", "hits": []}}

CASES = {
    "pass_rejection": [[scope, op] for scope in (_CARDS, _NOTES, _POLISH, _FINAL)
                       for op in _OPS],
    "tool_result_text": [
        ["search_past_meetings", {"ok": True, "text": "past:m_old:1 excerpt", "hits": []}],
        ["search_context_files", {"ok": False, "disabled": True,
                                  "text": "Knowledge-folder search is not available.",
                                  "hits": []}],
        ["search_past_meetings", {"ok": True, "text": "", "hits": [{"ref": "café"}]}],
        ["search_context_files", {"ok": True, "text": None, "hits": []}],
        ["search_context_files", {"ok": True, "text": 5, "hits": []}],
        ["patch_state", {"results": [{"ok": True, "reason": None, "target_id": "it_1",
                                      "seq": 3, "current_revision": None}]}],
        ["ask_question", {"ok": False, "reason": "polish_only", "target_id": None, "seq": None,
                          "current_revision": None}],
        ["patch_state", {"text": "ignored for writes", "naïve": 1.5, "nested": [True, None]}],
        ["unknown", {}],
    ],
    "serialize_op_result": [
        [{"ok": True, "op": {"op": "add_item"}}],
        [{"ok": False, "op": {"op": "update_item"}, "reason": "revision_mismatch",
          "target_id": "it_1", "current_revision": 3}],
        [{"ok": True, "op": {}, "target_id": "q_1", "seq": 42, "effect": {"x": 1},
          "inverse": {"op": "remove_item"}}],
    ],
    "op_results_payload": [
        [[]],
        [[{"ok": True, "op": {"op": "add_item"}, "seq": 1},
          {"ok": False, "op": {"op": "x"}, "reason": "notes_only"}]],
    ],
    "pass_kind_for": [
        [{"request_id": "r", "state_snapshot": {}, "new_segments": []}],
        [{"request_id": "r", "state_snapshot": {}, "new_segments": [], "is_notes": True}],
        [{"request_id": "r", "state_snapshot": {}, "new_segments": [], "is_polish": True,
          "is_notes": True}],
        [{"request_id": "r", "state_snapshot": {}, "new_segments": [], "is_consolidation": True,
          "is_polish": True, "is_notes": True}],
    ],
    "citable_segment_ids": [
        [{"request_id": "r", "state_snapshot": {}, "new_segments": []}],
        [{"request_id": "r", "new_segments": [{"id": "sg_1"}, {"id": ""}, {"text": "no id"},
                                              "junk", {"id": 7}, {"id": "sg_1"}],
          "state_snapshot": {"recent_transcript_context": [{"id": "sg_0"}, None,
                                                           {"id": "sg_1"}]}}],
        [{"request_id": "r", "new_segments": [{"id": "sg_2"}],
          "state_snapshot": {"recent_transcript_context": None}}],
    ],
    "for_payload": [
        [{"request_id": "r", "is_notes": True, "new_segments": [{"id": "sg_2"}],
          "state_snapshot": {"cards": {"live_notes": [{"id": "it_b"}, {"id": "it_a"},
                                                      {"text": "no id"}, "junk", {"id": 5}],
                                       "key_points": [{"id": "it_k"}]},
                             "recent_transcript_context": [{"id": "sg_1"}]}}],
        [{"request_id": "r", "new_segments": [], "state_snapshot": {
            "cards": {"live_notes": [{"id": "it_b"}]}}}],
        [{"request_id": "r", "is_polish": True, "new_segments": [{"id": "sg_9"}],
          "state_snapshot": {}}],
    ],
    "run_read_tool": [
        [None, "search_past_meetings", {"query": "budget"}],
        [None, "search_context_files", {"query": "roadmap", "relative_path": "plan.md"}],
        [_SEARCH_HOST, "search_past_meetings", {"query": "budget"}],
        [_SEARCH_HOST, "search_past_meetings",
         {"query": None, "meeting_id": "  m_old  ", "limit": "7"}],
        [_SEARCH_HOST, "search_context_files",
         {"query": "roadmap", "relative_path": "plan.md", "limit": 3.9}],
        [_SEARCH_HOST, "search_context_files",
         {"query": 12, "relative_path": "   ", "limit": None}],
        [_SEARCH_HOST, "search_past_meetings", {"limit": "many"}],
        [_SEARCH_HOST, "search_past_meetings", {"limit": True}],
        [_SEARCH_HOST, "search_past_meetings", {"limit": [5]}],
        [_SEARCH_HOST, "search_past_meetings", {"limit": -2}],
        [_EMPTY_SEARCH, "search_past_meetings", {"query": "x"}],
        [_EMPTY_SEARCH, "search_context_files", {"query": "x"}],
        [{"past": ["a", 1]}, "search_past_meetings", {"query": "x"}],
        [{"folder": 0}, "search_context_files", {"query": "x"}],
    ],
    "apply_patch_ops": [
        [None, _OPS, _CARDS],
        [None, _OPS, _NOTES],
        [None, _OPS, _POLISH],
        [_SEGMENTS_HOST, _OPS, _NOTES],
        [_SEGMENTS_HOST, _OPS, _FINAL],
        [_SCRIPTED, _OPS[:3], _CARDS_CITABLE],
        [_SCRIPTED, _OPS[5:8], _POLISH],
        [None, [], _CARDS],
        [None, [_OPS[1]], _POLISH],
        [None, {"op": "add_item"}, _CARDS],
        [None, None, _CARDS],
        [None, "ops", _POLISH],
    ],
    "run_tool": [
        [None, "patch_state", {"ops": _OPS[:3]}, _NOTES],
        [None, "patch_state", {}, _CARDS],
        [_SEGMENTS_HOST, "patch_state", {"ops": [_OPS[0], _OPS[6]]}, _POLISH],
        [None, "ask_question", {"text": "Who?", "evidence": ["sg_1"]}, _CARDS],
        [None, "ask_question", {"text": "Who?", "evidence": ["sg_1"]}, _NOTES],
        [None, "resolve_question", {"question_id": "q_1"}, _POLISH],
        [_SEGMENTS_HOST, "ask_question",
         {"text": "Who?", "evidence": ["sg_e991e94d6decd27d03"]}, _CARDS_CITABLE],
        [_SEGMENTS_HOST, "ask_question", {"text": None, "evidence": [7, None]}, _CARDS_CITABLE],
        [_SEGMENTS_HOST, "ask_question", {"text": 5, "evidence": "sg_1"}, _CARDS_CITABLE],
        [None, "ask_question", {"evidence": {"sg_1": True}}, _FINAL],
        [None, "ask_question", {"evidence": None}, _CARDS],
        [_SEGMENTS_HOST, "resolve_question",
         {"question_id": "q_1", "answer_text": "Yes", "confidence": 0.9,
          "evidence": ["sg_025f2c0b2c30e3e8dcb4"]}, _FINAL],
        [None, "resolve_question",
         {"question_id": 7, "answer_text": None, "confidence": "0.55", "evidence": []},
         _CARDS],
        [None, "resolve_question", {"confidence": None}, _CARDS],
        [None, "resolve_question", {"confidence": True}, _CARDS],
        [None, "resolve_question", {"confidence": "high"}, _CARDS],
        [None, "resolve_question", {"confidence": [1]}, _CARDS],
        [_SCRIPTED, "resolve_question", {"question_id": "q_2", "confidence": 1}, _CARDS],
        [_SEARCH_HOST, "search_past_meetings", {"query": "budget"}, _POLISH],
        [None, "search_context_files", {"query": "budget"}, _NOTES],
        [None, "noop", {}, _CARDS],
        [None, "unknown_tool", {}, _CARDS],
    ],
    "repair_evidence": [
        [None, [{"op": "add_item", "evidence": ["sg_e991e94d6decd27d03"]}], _KNOWN],
        [_SEGMENTS_HOST, [{"op": "add_item", "evidence": ["sg_e991e94d6decd27d03"]}], _KNOWN],
        [_SEGMENTS_HOST, [{"op": "add_item", "evidence": ["sg_e991e94d6decd27d03"]}], []],
        [_SEGMENTS_HOST, [{"op": "add_item", "evidence": [_KNOWN[0], 3]}], _KNOWN],
    ],
}


def _run_read_tool(module, host, name, args):
    tools = _host(host)
    return [module.run_read_tool(tools, name, args), tools.calls]


def _apply_patch_ops(module, host, ops, scope):
    tools = _host(host)
    results = module.apply_patch_ops(tools, ops, _scope(module, scope))
    return [[_result(r) for r in results], tools.calls]


def _run_tool(module, host, name, args, scope):
    tools = _host(host)
    payload, results = module.run_tool(tools, name, args, _scope(module, scope))
    return [payload, [_result(r) for r in results], tools.calls]


def _repair_evidence(module, host, ops, citable):
    tools = _host(host)
    return [module.repair_evidence(tools, ops, citable), tools.calls]


def _for_payload(module, payload):
    scope = module.ToolScope.for_payload(_payload(payload))
    return [scope.pass_kind, sorted(scope.note_ids), list(scope.citable_ids)]


WRAPPERS = {
    "pass_rejection": lambda module, scope, op: module.pass_rejection(_scope(module, scope), op),
    "serialize_op_result": lambda module, fields: module.serialize_op_result(OpResult(**fields)),
    "op_results_payload": lambda module, results: module.op_results_payload(
        [OpResult(**fields) for fields in results]),
    "pass_kind_for": lambda module, payload: module.pass_kind_for(_payload(payload)),
    "citable_segment_ids": lambda module, payload: module.citable_segment_ids(_payload(payload)),
    "for_payload": _for_payload,
    "run_read_tool": _run_read_tool,
    "apply_patch_ops": _apply_patch_ops,
    "run_tool": _run_tool,
    "repair_evidence": _repair_evidence,
}
