"""Curated inputs for meeting/corrections.py (M7.3 meeting-state-model; guidance_prompt M8.2).

Rules are given as [[term, replacement], ...] (a dict in that order); scoped rules as
{segment_id: [rule, ...]}. term_rules results come back as their items, in order.

    correct_segment_text(text, segment_id, rules, scoped)
    term_rules_from_items(items), term_rules(state)   [[term, replacement], ...]
    occurrence_rules(state)                           {segment_id: [rule, ...]}
    vocabulary_terms(rules, max_terms, max_chars)
    guidance_prompt(state)                            the states of test_meeting_corrections.py
                                                      and test_meeting_scoped_corrections.py
"""


def _fingerprint(text):
    value = 0xCBF29CE484222325
    for byte in text.encode("utf-8"):
        value = ((value ^ byte) * 0x100000001B3) & 0xFFFFFFFFFFFFFFFF
    return f"fnv1a64:{value:016x}"


def _rules(pairs):
    return {term: replacement for term, replacement in pairs}


def _items(rules):
    return [[term, replacement] for term, replacement in rules.items()]


def _note(selected, replacement, *, kind="term_correction", **overrides):
    item = {"id": f"it_{selected}", "card": "user_notes", "status": "edited",
            "author_type": "user", "text": f"Correction: {selected} -> {replacement}",
            "data": {"kind": kind, "selected_text": selected, "replacement": replacement}}
    item.update(overrides)
    return item


def _occurrence(selected, replacement, index, base, segment="sg_1", **overrides):
    data = {"kind": "occurrence_correction", "selected_text": selected,
            "replacement": replacement, "occurrence_index": index,
            "base_fingerprint": base}
    item = {"id": f"it_{selected}_{index}", "card": "user_notes", "status": "edited",
            "author_type": "user", "text": "fix", "data": data, "evidence": [segment]}
    item.update(overrides)
    return item


_VOICE = {"author_type": "system", "author_id": "voice_command",
          "data": {"kind": "term_correction", "selected_text": "cloud", "replacement": "Claude",
                   "source": "voice_command", "command": "fix_transcript"}}

_ITEMS = [
    _note("Entropic", "Anthropic"),
    _note("gone", "Gone", status="removed"),
    _note("agent", "Agent", author_type="agent"),
    _note("insight", "Insight", kind="agent_insight"),
    _note("x" * 121, "Long"),
    _note(" " + "y" * 120 + " ", "Bounded"),
    _note("blank", "   "),
    _note("  Padded  ", "  Clean  "),
    _note("entropic", "Acme"),
    _note("ÉCOLE", "École"),
    _note(5, "Number"),
    {"text": "plain user note", "status": "edited", "author_type": "user"},
    dict(_note("cloud", "Claude"), **_VOICE),
    dict(_note("cloud", "Claude"), **dict(_VOICE, data=dict(_VOICE["data"], command="other"))),
    {"author_type": "user", "data": None},
    {"author_type": "user", "data": "junk"},
    None, "junk", 5,
]

_BASE = "Entropic ships Entropic today, entropic!"
_SEGMENT = "Anthropic ships Anthropic today, Anthropic!"


def _state(*notes):
    return {"cards": {"user_notes": list(notes)}}


CASES = {
    "segment_fingerprint": [[""], ["a"], ["hello world"], ["é"], ["\U0001F600"], [_BASE],
                            ["line\nbreak"]],
    "valid_segment_fingerprint": [
        [_fingerprint("x")], ["fnv1a64:0123456789abcdef"], ["fnv1a64:0123456789ABCDEF"],
        ["fnv1a64:0123456789abcde"], ["fnv1a64:0123456789abcdef0"], ["fnv1a64:0123456789abcdeg"],
        ["fnv1a64:0123456789abcdef\n"], ["FNV1A64:0123456789abcdef"], [""], [None], [5],
    ],
    "is_correction_author": [
        [{"author_type": "user"}], [{"author_type": "agent"}], [{}],
        [{"author_type": "system", "author_id": "voice_command",
          "data": {"source": "voice_command", "command": "fix_transcript"}}],
        [{"author_type": "system", "author_id": "voice_command",
          "data": {"source": "voice_command", "command": "remember"}}],
        [{"author_type": "system", "author_id": "voice_command", "data": None}],
        [{"author_type": "system", "author_id": "state_repair",
          "data": {"source": "voice_command", "command": "fix_transcript"}}],
    ],
    "term_rules_from_items": [[_ITEMS], [[]], [[_note("a", "b"), _note("A", "c")]]],
    "term_rules": [[_state(*_ITEMS[:12])], [{}], [{"cards": None}],
                   [{"cards": {"user_notes": [None, "junk"]}}], [{"cards": {"user_notes": None}}]],
    "occurrence_rules": [
        [_state(
            _occurrence("Entropic", "Anthropic", 1, _fingerprint(_BASE)),
            _occurrence("Entropic", "Acme", 0, _fingerprint(_BASE)),
            _occurrence("today", "now", 0, _fingerprint(_BASE), segment="sg_2"),
            _occurrence("bad", "x", True, _fingerprint(_BASE)),
            _occurrence("bad", "x", 1001, _fingerprint(_BASE)),
            _occurrence("bad", "x", 2.0, _fingerprint(_BASE)),
            _occurrence("bad", "x", 0, "fnv1a64:nothex"),
            _occurrence("bad", "  ", 0, _fingerprint(_BASE)),
            _occurrence("gone", "x", 0, _fingerprint(_BASE), status="removed"),
            _occurrence("agent", "x", 0, _fingerprint(_BASE), author_type="agent"),
            _occurrence("two", "x", 0, _fingerprint(_BASE), evidence=["sg_1", "sg_2"]),
            _occurrence("num", "x", 0, _fingerprint(_BASE), evidence=[5]),
            _occurrence("none", "x", 0, _fingerprint(_BASE), evidence=None),
            _occurrence(" spaced ", " out ", 3, _fingerprint(_BASE)),
            _note("Entropic", "Anthropic"),
        )],
        [{}],
    ],
    "correct_text": [
        ["Entropic and ENTROPIC, entropic.", [["entropic", "Anthropic"]]],
        ["entropically unentropic", [["entropic", "Anthropic"]]],
        ["a b", [["a", "b"], ["b", "c"]]],
        ["open whisper uses whisper",
         [["a", "b"], ["b", "c"], ["open whisper", "OpenWhisper"], ["whisper", "Whisper"]]],
        ["we use c++ (fast)", [["c++", "C++"], ["(fast)", "quick"]]],
        ["Entropic", []], ["", [["entropic", "Anthropic"]]],
        ["École et ÉCOLE", [["école", "École"]]],
        ["Straße and STRASSE", [["strasse", "Street"]]],
    ],
    "correct_segment_text": [
        [_BASE, "sg_1", [], {"sg_1": [{"selected_text": "Entropic", "replacement": "Acme",
                                       "occurrence_index": 1,
                                       "base_fingerprint": _fingerprint(_BASE)}]}],
        [_BASE, "sg_2", [], {"sg_1": [{"selected_text": "Entropic", "replacement": "Acme",
                                       "occurrence_index": 1,
                                       "base_fingerprint": _fingerprint(_BASE)}]}],
        [_BASE, "sg_1", [["entropic", "Anthropic"]],
         {"sg_1": [{"selected_text": "Anthropic", "replacement": "Acme",
                    "occurrence_index": 2, "base_fingerprint": _fingerprint(_SEGMENT)}]}],
        [_BASE, "sg_1", [["entropic", "Anthropic"]],
         {"sg_1": [{"selected_text": "Entropic", "replacement": "Acme",
                    "occurrence_index": 0, "base_fingerprint": _fingerprint(_BASE)}]}],
        [_BASE, "sg_1", [], {"sg_1": [
            {"selected_text": "Entropic ships", "replacement": "X", "occurrence_index": 0,
             "base_fingerprint": _fingerprint(_BASE)},
            {"selected_text": "ships", "replacement": "Y", "occurrence_index": 0,
             "base_fingerprint": _fingerprint(_BASE)},
            {"selected_text": "entropic", "replacement": "Z", "occurrence_index": 2,
             "base_fingerprint": _fingerprint(_BASE)},
            {"selected_text": "today", "replacement": "now", "occurrence_index": 5,
             "base_fingerprint": _fingerprint(_BASE)}]}],
        [_BASE, "sg_1", [], {"sg_1": [
            {"selected_text": "entropic", "replacement": "First", "occurrence_index": 0,
             "base_fingerprint": _fingerprint(_BASE)},
            {"selected_text": "Entropic", "replacement": "Second", "occurrence_index": 0,
             "base_fingerprint": _fingerprint(_BASE)}]}],
        ["café au lait, CAFÉ", "sg_1", [], {"sg_1": [
            {"selected_text": "café", "replacement": "coffee", "occurrence_index": 1,
             "base_fingerprint": _fingerprint("café au lait, CAFÉ")}]}],
        ["un_entropic entropic", "sg_1", [], {"sg_1": [
            {"selected_text": "entropic", "replacement": "X", "occurrence_index": 0,
             "base_fingerprint": _fingerprint("un_entropic entropic")}]}],
        ["c++ and c++", "sg_1", [], {"sg_1": [
            {"selected_text": "c++", "replacement": "C plus plus", "occurrence_index": 1,
             "base_fingerprint": _fingerprint("c++ and c++")}]}],
        ["", "sg_1", [], {"sg_1": []}],
    ],
    "vocabulary_terms": [
        [[["entropic", "Anthropic"], ["antropic", "anthropic"], ["cloud", "Claude"]], 12, 160],
        [[[str(i), f"Term{i}"] for i in range(40)], 12, 160],
        [[["a", "x" * 100], ["b", "y" * 100]], 12, 160],
        [[["a", "x" * 79], ["b", "y" * 79], ["c", "z"]], 12, 160],
        [[["a", "é" * 158], ["b", "c"]], 12, 160],
        [[["a", "One"], ["b", "Two"], ["c", "Three"]], 2, 160],
        [[], 12, 160],
    ],
    "vocabulary_hint": [
        [[["entropic", "Anthropic"], ["antropic", "anthropic"], ["cloud", "Claude"]]],
        [[]], [[["a", "Ünïcode"]]],
    ],
    "guidance_prompt": [
        [{}], [{"cards": None}], [{"cards": {"user_notes": None}}],
        [_state({"text": "plain", "status": "edited", "author_type": "user"})],
        [_state(
            _note("Entropic", "Anthropic", text="Correction: Entropic -> Anthropic"),
            _note("plan", "", kind="agent_insight", text="We mean the hiring plan"),
            _note("secret", "Secret", status="removed", text="deleted-secret"),
            _note("agent", "Agent", author_type="agent", text="agent-authored"),
            {"text": "plain note", "status": "edited", "author_type": "user", "data": {}},
        )],
        [_state(*_ITEMS[:14])],
        [_state(dict(_note("cloud", "Claude"), **_VOICE),
                {"author_type": "user", "data": None, "text": "no data"},
                {"author_type": "user", "data": {"kind": "agent_insight"}, "text": "Ünïcode ✓",
                 "evidence": ["sg_1"]})],
        [_state(_occurrence("word", "third", 2, _fingerprint("word word word"), segment="sg_one"),
                _occurrence("word", "first", 0, _fingerprint("word word word"), segment="sg_one"))],
        [_state({"author_type": "user", "data": "junk"})],
        [_state({"author_type": "user", "data": ["kind"]})],
        [{"cards": {"user_notes": [None, "junk", 5]}}],
    ],
}

WRAPPERS = {
    "term_rules_from_items": lambda module, items: _items(module.term_rules_from_items(items)),
    "term_rules": lambda module, state: _items(module.term_rules(state)),
    "correct_text": lambda module, text, rules: module.correct_text(text, _rules(rules)),
    "correct_segment_text": lambda module, text, segment_id, rules, scoped:
        module.correct_segment_text(text, segment_id, _rules(rules), scoped),
    "vocabulary_terms": lambda module, rules, max_terms, max_chars: module.vocabulary_terms(
        _rules(rules), max_terms=max_terms, max_chars=max_chars),
    "vocabulary_hint": lambda module, rules: module.vocabulary_hint(_rules(rules)),
}
