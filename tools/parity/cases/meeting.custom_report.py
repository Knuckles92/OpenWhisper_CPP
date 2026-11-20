"""Curated inputs for meeting/custom_report.py (M8.5 custom-reports).

    format_clock, report_title, _clean_markdown       direct
    validated_request(request)           _validated_request: {"result"} or {"error": message}
    corpus(segments, participants)       TranscriptCorpus: lines, duration_s, total_chars, len
    render_edges(segments, participants, edge_chars)
    window(segments, start_s, end_s)     corpus.window over the case's segments, no participants
    search(segments, query, limit)       corpus.search; limit "<default>" leaves it out
    terms(text)                          sorted(_terms(text))
    clip_lines(lines, budget)            _clip_lines
    build_digest(meeting, state, segments)          the corpus is built from the state's
    build_corpus_prompt(meeting, state, segments, request)   participants, as generate_report does
    tool_schemas(past_recall, context_files)        _tool_schemas with keyword arguments
    run_tool(segments, name, arguments)  _run_tool over _make_dispatcher(corpus, None, "m_report",
                                         used): {"content", "used"}; recall and the knowledge
                                         folder are off in the scratch settings
    generate_report(turns, request, recall, folder, endpoint)
        generate_report through a fake openai client over the real services.text_generation
        (Chat Completions): each turn is {"text", "calls": [{"id", "name", "arguments"}],
        "finish_reason", "usage"} and becomes the client's response; the result is
        {"requests": [the keyword arguments of every chat.completions.create call], and
        "result": {...} or "error": str(ReportUnavailable)}. `recall` and `folder` patch the
        module's consent gates (with a repository present); `endpoint` is the meeting's text
        endpoint snapshot (a custom endpoint on 127.0.0.1, whose base URL the C++ replay
        replaces with its loopback provider's).

The C++ replay is tests/meeting_intel/test_report_golden.cpp.
"""

from types import SimpleNamespace
from unittest.mock import patch

MEETING_ID = "m_past"


_AFTER_START = object()


def _segment(index, start_s, text, channel="mic", pid=None, end_s=_AFTER_START):
    return {
        "id": f"sg_{index}", "meeting_id": MEETING_ID, "chunk_id": 1, "channel": channel,
        "start_s": start_s, "end_s": (start_s + 4.0) if end_s is _AFTER_START else end_s,
        "text": text, "speaker_participant_id": pid, "speaker_source": "channel",
        "speaker_pinned": False,
    }


SEGMENTS = [
    _segment(1, 0.0, "Lets talk about the vendor renewal and pricing."),
    _segment(2, 6.0, "We cannot go below a twelve percent discount.", "loopback"),
    _segment(3, 14.0, "Priya will draft the renewal note by Friday."),
    _segment(4, 24.0, "Agreed, we hold at twelve and revisit the terms in Q3."),
]

PARTICIPANTS = {
    "p_1": {"id": "p_1", "display_name": "Priya"},
    "p_2": {"id": "p_2", "display_name": ""},
    "p_3": {"id": "p_3", "display_name": "  Sam Lee  "},
}

ODD = [
    _segment(1, 3725.9, "line\nbreaks  inside ", pid="p_1"),
    _segment(2, "7.5", "  text as a string offset  ", "loopback", pid="p_2", end_s="12"),
    _segment(3, None, "no start", pid="p_missing", end_s=None),
    _segment(4, 2.0, "   ", pid="p_1"),
    _segment(5, 9.0, None),
    _segment(6, 11.0, "mixed Ünïcödé — Straße DÉJÀ vu", "other", pid="p_3", end_s=-3.0),
    _segment(7, True, "a bool offset", end_s=False),
    _segment(8, 40.0, 42, pid=7),
]

def _brief(start_s, text):
    """A segment with only what the corpus reads, to keep the long cases small."""
    return {"start_s": start_s, "end_s": start_s + 4.0, "text": text}


LONG = [_brief(i * 5.0, f"line number {i} about budget") for i in range(300)]
# Above INLINE_TRANSCRIPT_CHARS (60,000) once rendered.
VERY_LONG = [_brief(i * 5.0, f"line number {i} about the budget review") for i in range(1300)]
HOURS = [_brief(3000.0 + i * 120.0, f"late item {i} on the roadmap") for i in range(80)]

MEETING = {"id": MEETING_ID, "title": "Vendor renewal", "started_at": "2026-09-19T10:00:00Z",
           "cloud_enabled": True}


def _state(**fields):
    from meeting.state.schema import FinalizationState, MeetingState

    state = MeetingState(meeting_id=MEETING_ID, title="Vendor renewal", status="ended",
                         cloud_enabled=True)
    state.finalization = FinalizationState.coerce(
        {"status": "completed"}, cloud_enabled=True, meeting_status="ended")
    snapshot = state.to_dict()
    snapshot.update(fields)
    return snapshot


def _rich_state():
    cards = {
        "live_notes": [{"id": "it_n", "text": "Opened with the renewal", "status": "proposed",
                        "data": {"start_s": 0}}],
        "key_points": [
            {"id": "it_k1", "text": "  Price floor  ", "status": "confirmed",
             "data": {"start_s": 6.4, "heading": "  Pricing "}},
            {"id": "it_k2", "text": "Edited point", "status": "edited", "data": {}},
            {"id": "it_k3", "text": "Gone", "status": "removed", "data": {}},
            "not an item",
        ],
        "decisions": [
            {"id": "it_d1", "text": "Hold at twelve", "status": "proposed",
             "data": {"start_s": 3725}, "review": {"state": "unsupported"}},
            {"id": "it_d2", "text": "Removed decision", "status": "removed", "data": {}},
        ],
        "action_items": [
            {"id": "it_a1", "text": "Draft the renewal note", "status": "proposed",
             "data": {"owner_participant_id": "p_1", "deadline": " Friday "},
             "review": {"state": "provisional"}},
            {"id": "it_a2", "text": "Call legal", "status": "proposed",
             "data": {"owner_participant_id": "p_2", "deadline": "  ", "due_date": "Monday"}},
            {"id": "it_a3", "text": "Unowned", "status": "proposed",
             "data": {"owner_participant_id": "p_unknown", "due_date": "  next week "}},
            {"id": "it_a4", "text": "Bool start", "status": "proposed",
             "data": {"start_s": True, "deadline": 5}},
        ],
        "risks": [
            {"id": "it_r1", "text": "Churn", "status": "proposed", "data": {"severity": "high"}},
            {"id": "it_r2", "text": "Empty severity", "status": "proposed",
             "data": {"severity": "", "heading": "   "}},
            {"id": "it_r3", "text": "Numeric severity", "status": "proposed",
             "data": {"severity": 3}},
        ],
        "timeline": [],
        "user_notes": [{"id": "it_u", "text": "", "status": "proposed", "data": None}],
    }
    return _state(
        participants=PARTICIPANTS,
        rolling_summary="The team held pricing at twelve percent.",
        topic={"current": "Vendor renewal", "history": []},
        cards=cards,
        questions=[
            {"id": "q_1", "text": "What is the term length?", "status": "open"},
            {"id": "q_2", "text": "Who signs?", "status": "resolved", "answer": "Legal does."},
            {"id": "q_3", "text": "Dismissed", "status": "dismissed", "answer": ""},
            {"id": "q_4", "text": None, "status": "answered", "answer": "No text"},
            "not a question",
        ],
        insight_review={"questions": [
            {"correction": "The floor is twelve, not ten."},
            {"correction": "Old correction", "superseded": True},
            {"correction": ""},
            "not a dict",
        ]},
    )


def _corpus(module, segments, participants):
    corpus = module.TranscriptCorpus(segments, participants)
    return {"lines": corpus.lines, "duration_s": corpus.duration_s,
            "total_chars": corpus.total_chars, "len": len(corpus)}


def _render_edges(module, segments, participants, edge_chars):
    return module.TranscriptCorpus(segments, participants).render_edges(edge_chars=edge_chars)


def _window(module, segments, start_s, end_s):
    return module.TranscriptCorpus(segments, {}).window(start_s, end_s)


def _search(module, segments, query, limit):
    corpus = module.TranscriptCorpus(segments, {})
    if limit == "<default>":
        return corpus.search(query)
    return corpus.search(query, limit)


def _terms(module, text):
    return sorted(module._terms(text))


def _clip_lines(module, lines, budget):
    return module._clip_lines(lines, budget)


def _corpus_for(module, state, segments):
    return module.TranscriptCorpus(segments, state.get("participants") or {})


def _build_digest(module, meeting, state, segments):
    return module.build_digest(meeting, state, _corpus_for(module, state, segments))


def _build_corpus_prompt(module, meeting, state, segments, request):
    return module.build_corpus_prompt(meeting, state, _corpus_for(module, state, segments),
                                      request)


def _validated(module, request):
    try:
        return {"result": module._validated_request(request)}
    except module.ReportUnavailable as exc:
        return {"error": str(exc)}


def _tool_schemas(module, past_recall, context_files):
    return module._tool_schemas(past_recall=past_recall, context_files=context_files)


def _run_tool(module, segments, name, arguments):
    used = {}
    dispatch = module._make_dispatcher(module.TranscriptCorpus(segments, {}), None, "m_report",
                                       used)
    call = SimpleNamespace(id="c1", function=SimpleNamespace(name=name, arguments=arguments))
    return {"content": module._run_tool(dispatch, call), "used": dict(sorted(used.items()))}


class _Completions:
    def __init__(self, turns):
        self.turns = turns
        self.requests = []

    def create(self, **kwargs):
        self.requests.append(kwargs)
        turn = self.turns[min(len(self.requests) - 1, len(self.turns) - 1)]
        calls = [SimpleNamespace(id=c["id"], function=SimpleNamespace(
            name=c["name"], arguments=c["arguments"])) for c in turn.get("calls", [])]
        message = SimpleNamespace(content=turn.get("text", ""), tool_calls=calls or None,
                                  refusal=None)
        choice = SimpleNamespace(message=message, finish_reason=turn.get("finish_reason", "stop"))
        usage = turn.get("usage")
        return SimpleNamespace(choices=[choice],
                               usage=SimpleNamespace(**usage) if usage is not None else None)


class _Client:
    def __init__(self, turns):
        self.chat = SimpleNamespace(completions=_Completions(turns))

    def with_options(self, **_):
        return self


def _generate_report(module, turns, request, recall, folder, endpoint):
    from services.text_llm import profile_from_agent_config

    client = _Client(turns)
    profile = profile_from_agent_config("custom_report", endpoint)
    out = {}
    with patch.object(module, "past_recall_enabled", return_value=recall), \
            patch.object(module, "context_folder_enabled", return_value=folder):
        try:
            out["result"] = module.generate_report(
                MEETING, _state(participants={"p_1": {"display_name": "Priya"}}), SEGMENTS,
                request, provider="custom_report", model="test-model", endpoint=endpoint,
                repository=object(), client=client, profile=profile)
        except module.ReportUnavailable as exc:
            out["error"] = str(exc)
    out["requests"] = client.chat.completions.requests
    return out


ENDPOINT = {"profile_id": "custom_report", "name": "Report test", "kind": "custom",
            "base_url": "http://127.0.0.1:9/v1", "api_key_env": ""}
NO_TOOLS_ENDPOINT = dict(ENDPOINT, model_metadata={
    "model": "test-model", "protocol": "chat", "tools": False, "context_window": 8192,
    "max_output_tokens": 1024})

USAGE = {"prompt_tokens": 10, "completion_tokens": 5, "total_tokens": 15}


def _turn(text="", calls=(), finish_reason="stop", usage=USAGE):
    turn = {"text": text, "calls": list(calls), "finish_reason": finish_reason}
    if usage is not None:
        turn["usage"] = usage
    return turn


def _call(call_id, name, arguments):
    return {"id": call_id, "name": name, "arguments": arguments}


SEARCH_CALL = _call("c1", "search_transcript", '{"query": "discount"}')

CASES = {
    "format_clock": [[v] for v in (
        0, 59, 60, 61, 599.99, 3599, 3600, 3661, 36000, 360000.5, -5, -0.5, 7.9, "125", " 42 ",
        "abc", None, True, False, [], {}, "nan", float("nan"), float("inf"), "-inf", 1e16)],
    "corpus": [
        [SEGMENTS, {}], [SEGMENTS, PARTICIPANTS], [ODD, PARTICIPANTS], [ODD, None], [[], {}],
        [[_segment(1, "abc", "bad start", end_s=10.0)], {}],
        [[_segment(1, 1.0, "bad end", end_s=[1])], {}],
        [[_segment(1, 1.0, "list pid", pid=["p_1"])], PARTICIPANTS],
        [[_segment(1, 1.0, "nan start", end_s=float("nan")),
          _segment(2, 2.0, "later", end_s=9.0)], {}],
    ],
    "render_edges": [
        [LONG, {}, 400], [LONG, {}, 14000], [SEGMENTS, {}, 14000], [SEGMENTS, {}, 60],
        [SEGMENTS, {}, 0], [SEGMENTS, {}, 49], [HOURS, {}, 300],
    ],
    "window": [
        [SEGMENTS, 6, 15], [SEGMENTS, 15, 6], [SEGMENTS, 500, 600], [SEGMENTS, 0, 0],
        [SEGMENTS, -10, 1e9], [SEGMENTS, float("nan"), 10], [LONG, 0, 3000],
        [[], 0, 10], [SEGMENTS, 0, float("inf")], [SEGMENTS, float("inf"), float("inf")],
    ],
    "search": [
        [SEGMENTS, "renewal terms", "<default>"], [SEGMENTS, "submarine", "<default>"],
        [SEGMENTS, "  ", "<default>"], [SEGMENTS, "the and", "<default>"],
        [SEGMENTS, "Twelve DISCOUNT", 1], [SEGMENTS, "twelve", 0], [SEGMENTS, "twelve", None],
        [SEGMENTS, "twelve", "2"], [SEGMENTS, "twelve", 3.9], [SEGMENTS, "twelve", -4],
        [SEGMENTS, "twelve", 10 ** 30], [SEGMENTS, "twelve", -(10 ** 30)], [SEGMENTS, "twelve", True],
        [SEGMENTS, "twelve", "abc"], [SEGMENTS, "twelve", [1]], [SEGMENTS, "twelve", float("inf")],
        [SEGMENTS, "zz 'quoted' isn't", "<default>"], [LONG, "budget line", 60],
        [LONG, "number", "<default>"], [ODD, "straße déjà vu", "<default>"],
        [SEGMENTS, "it's \"odd\"", "<default>"],
    ],
    "terms": [[t] for t in (
        "", "The and of", "Pricing, pricing; PRICING!", "isn't it o'clock", "Straße DÉJÀ vu ÜBER",
        "a1 b22 c333 _x_ __", "日本語 テキスト", "tab\tsep\nline")],
    "clip_lines": [
        [["a", "bb", "ccc"], 100], [["a", "bb", "ccc"], 5], [["a", "bb", "ccc"], 2],
        [["a", "bb", "ccc"], 0], [[], 10], [["ééé", "x"], 4], [["ééé", "x"], 6],
    ],
    "build_digest": [
        [MEETING, _state(), SEGMENTS],
        [MEETING, _rich_state(), SEGMENTS],
        [{"id": MEETING_ID, "title": "Row title"}, _state(title=""), []],
        [{}, _state(title=None, participants=None, cards=None, questions=None), []],
        [MEETING, _state(cards={"key_points": [
            {"id": f"it_{i}", "text": "word " * 60, "status": "proposed", "data": {}}
            for i in range(120)]}), SEGMENTS],
    ],
    "build_corpus_prompt": [
        [MEETING, _state(), SEGMENTS, "  every commitment we made  "],
        [MEETING, _rich_state(), SEGMENTS, "a brief"],
        [MEETING, _state(), [], "a brief"],
        [MEETING, _state(), VERY_LONG, "a brief"],
    ],
    "report_title": [
        ["# Pricing brief\n\nbody", "summarize the pricing"],
        ["\n\n#  Spaced  \n", "summarize the pricing"],
        ["no heading here", "summarize the pricing"],
        ["", "  many   spaced\twords\n here  "],
        ["## Second level", "fallback"],
        ["#NoSpace", "fallback"],
        ["\r\n  # Windows lines\r\nbody", "x"],
        ["# " + "t" * 250, "x"],
        ["# ", "the request"],
        ["", "r" * 78], ["", "r" * 79], ["", "é" * 100],
        [" # After a separator", "x"],
    ],
    "_clean_markdown": [
        ["# Brief\n\nbody"], ["```markdown\n# Brief\n\nbody\n```"], ["```\n# Brief\n```  "],
        ["```markdown\n# Brief\nunclosed"], ["```"], ["``` \n```"], ["  \n  "], [""],
        ["x" * 60010], ["é" * 59990 + "  " + "y" * 30],
        ["```md\n" + "z" * 60100 + "\n```"],
    ],
    "validated_request": [[v] for v in (
        "  a brief  ", "", "   ", "x" * 2000, "x" * 2001, "é" * 2000, " " + "x" * 2000 + " ")],
    "tool_schemas": [[False, False], [True, False], [False, True], [True, True]],
    "run_tool": [
        [SEGMENTS, "search_transcript", '{"query": "discount"}'],
        [SEGMENTS, "search_transcript", '{"query": "twelve", "limit": 1}'],
        [SEGMENTS, "search_transcript", '{"query": "twelve", "limit": "abc"}'],
        [SEGMENTS, "search_transcript", '{"query": null}'],
        [SEGMENTS, "read_transcript", '{"start_s": 0, "end_s": 10}'],
        [SEGMENTS, "read_transcript", '{"start_s": "6", "end_s": "x"}'],
        [SEGMENTS, "read_transcript", '{"start_s": "inf", "end_s": "inf"}'],
        [SEGMENTS, "read_transcript", ""],
        [SEGMENTS, "nonexistent_tool", "{}"],
        [SEGMENTS, "read_transcript", "{not json"],
        [SEGMENTS, "read_transcript", "[1, 2]"],
        [SEGMENTS, "search_transcript", "NaN"],
        [SEGMENTS, "search_past_meetings", '{"query": "budget"}'],
        [SEGMENTS, "search_context_files", '{"query": "budget"}'],
    ],
    "generate_report": [
        [[_turn("# Commitments\n\n- Hold at 12%")], "every commitment", False, False, ENDPOINT],
        [[_turn("", [SEARCH_CALL, _call("c2", "read_transcript", '{"start_s": 0, "end_s": 10}')]),
          _turn("# Brief\n\nHeld at twelve.")], "every commitment", False, False, ENDPOINT],
        [[_turn("", [_call("c1", "search_transcript", '{"query": "x"}'),
                     _call("c2", "nonexistent_tool", "{}"),
                     _call("c3", "read_transcript", "{not json")]),
          _turn("# Brief\n\nbody")], "every commitment", False, False, ENDPOINT],
        [[_turn("# Partial\n\nbody", [SEARCH_CALL])] * 6, "every commitment", False, False,
         ENDPOINT],
        [[_turn("# Draft one", [SEARCH_CALL]), _turn("   ", [SEARCH_CALL]), _turn("No heading")],
         "  summarize   the pricing  ", False, False, ENDPOINT],
        [[_turn("```markdown\n# Brief\n\nbody\n```")], "every commitment", False, False, ENDPOINT],
        [[_turn("   ")], "every commitment", False, False, ENDPOINT],
        [[_turn("# Brief\n\nbody", usage=None)], "every commitment", True, True, ENDPOINT],
        [[_turn("# Brief\n\nbody")], "every commitment", True, False, ENDPOINT],
        [[_turn("# One shot", [SEARCH_CALL])], "every commitment", True, True, NO_TOOLS_ENDPOINT],
        [[_turn("cut", finish_reason="length")], "every commitment", False, False, ENDPOINT],
        [[_turn("# Brief")], "   ", False, False, ENDPOINT],
        [[_turn("# Brief")], "x" * 2001, False, False, ENDPOINT],
    ],
}

WRAPPERS = {
    "corpus": _corpus,
    "render_edges": _render_edges,
    "window": _window,
    "search": _search,
    "terms": _terms,
    "clip_lines": _clip_lines,
    "build_digest": _build_digest,
    "build_corpus_prompt": _build_corpus_prompt,
    "validated_request": _validated,
    "tool_schemas": _tool_schemas,
    "run_tool": _run_tool,
    "generate_report": _generate_report,
}

PINS = {
    "settings": "the scratch settings: past-meeting recall and the knowledge folder are off, so "
                "run_tool's recall and folder searches answer that they are disabled",
    "generate_report": "a fake openai client under the real services.text_generation; the "
                       "consent gates patched per case",
}
