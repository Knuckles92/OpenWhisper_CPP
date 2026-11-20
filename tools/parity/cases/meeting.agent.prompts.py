"""Curated inputs for meeting/agent/prompts.py (M8.2 agent-prompts-and-policy).

    build_system_prompt(), build_note_taker_system_prompt()
    build_consolidation_instructions(report_views)
    build_checkpoint_user_prompt(state, new_segments, is_consolidation, is_polish)
    build_notes_user_prompt(state, new_segments)
    intent_prompt(state), render_state_compact(state), render_notes_page(state)
    format_segment_line(segment, participants)
    select_spotlight_items(cards, limit)

States are MeetingState.to_dict()-shaped snapshots plus the keys the scheduler adds
(recent_transcript_context, report_views, note_adjustment_request, notes_review_requested).
The prompts name the product; the C++ replay compares them rebranded.
"""

_LONG = ("This is a detailed and comprehensive note block explaining architectural "
         "decisions. ") * 4
_UNICODE = "Café déjà vu — naïve résumé " * 12

_PARTICIPANTS = {
    "p_me": {"display_name": "Me", "kind": "me", "name_source": "human"},
    "p_s1": {"display_name": "Speaker 1", "kind": "others_cluster", "is_provisional": True,
             "name_source": "agent"},
    "p_blank": {"display_name": "", "kind": ""},
    "p_maria": {"display_name": "Maria", "kind": "guest", "name_source": "human",
                "is_provisional": True},
}

_SEGMENTS = [
    {"id": "sg_a1", "channel": "mic", "start_s": 12, "end_s": 14,
     "text": "Hello\nworld  ", "speaker_participant_id": None},
    {"id": "sg_a2", "channel": "loopback", "start_s": 15.25, "text": "We picked Acme.",
     "speaker_participant_id": "p_s1"},
    {"id": "sg_a3", "channel": "loopback", "start_s": 0.05, "text": "Blank name.",
     "speaker_participant_id": "p_blank"},
    {"id": "sg_a4", "channel": "loopback", "start_s": None, "text": None,
     "speaker_participant_id": "p_unknown"},
    {"id": "sg_a5", "channel": "mic", "start_s": "3.25", "text": "Text start.",
     "speaker_participant_id": "p_maria"},
    {"id": "sg_a6", "start_s": 0.25, "text": "No channel."},
]

_CARDS = {
    "key_points": [
        {"id": "it_k1", "revision": 1, "status": "proposed", "text": "Short key point.",
         "updated_at": "2026-08-16T10:00:00Z"},
        {"id": "it_k2", "revision": 3, "status": "edited", "pinned": True,
         "text": "Line one\nline two", "updated_at": "2026-08-16T10:05:00Z"},
        {"id": "it_k3", "status": "removed", "text": "Gone"},
        {"id": "it_k4", "revision": 2, "status": "proposed", "text": _UNICODE,
         "author_type": "system", "author_id": "voice_command",
         "created_at": "2026-08-16T10:06:00Z"},
        {"id": "it_k5", "status": "proposed", "text": "Let's take a look at this.",
         "author_type": "system", "author_id": "state_repair"},
    ],
    "decisions": [
        {"id": "it_d1", "revision": 2, "status": "confirmed", "text": "Adopt SQLite for storage.",
         "data": {}, "updated_at": "2026-08-16T10:01:00Z"},
    ],
    "action_items": [
        {"id": "it_a1", "revision": 1, "status": "proposed", "text": "Send the deck.",
         "data": {"owner_participant_id": "p_maria"}, "updated_at": "2026-08-16T10:03:00Z"},
    ],
    "risks": [
        {"id": "it_r1", "revision": 1, "status": "proposed", "text": "Budget overrun.",
         "data": {"severity": "high", "start_s": 0}, "updated_at": "2026-08-16T10:02:00Z"},
    ],
    "timeline": [
        {"id": "it_t1", "revision": 1, "status": "proposed", "text": "Opening question.",
         "data": {"start_s": 12.0}},
        {"id": "it_t2", "revision": 1, "status": "proposed", "text": _LONG,
         "data": {"start_s": 41, "heading": "Long beat"}},
    ],
    "live_notes": [
        {"id": "it_n1", "revision": 2, "status": "proposed", "text": _LONG,
         "data": {"heading": "Architecture Review", "start_s": 15.0}},
        {"id": "it_n2", "revision": 1, "status": "edited", "pinned": True,
         "text": "Human block.", "data": {"start_s": 0.0}},
        {"id": "it_n3", "status": "removed", "text": "Removed block."},
        {"id": "it_n4", "text": "No revision.", "data": None},
    ],
    "user_notes": [
        {"id": "it_u1", "status": "edited", "author_type": "user",
         "text": "Customer spells it Acme"},
        {"id": "it_u2", "status": "removed", "author_type": "user", "text": "deleted-secret"},
        {"id": "it_u3", "status": "edited", "author_type": "user",
         "text": "Correction: Entropic -> Anthropic",
         "data": {"kind": "term_correction", "selected_text": "Entropic",
                  "replacement": "Anthropic"}},
    ],
}

_QUESTIONS = [
    {"id": "q_2", "text": "Who owns QA?", "status": "open", "asked_at": "2"},
    {"id": "q_1", "text": "Budget?", "status": "open", "asked_at": "1",
     "suggested_answer": "$500", "suggested_confidence": 0.5},
    {"id": "q_3", "text": "Settled", "status": "resolved", "asked_at": "0"},
    {"id": "q_4", "text": "Unknown confidence", "status": "open", "asked_at": "1",
     "suggested_answer": "Maybe", "suggested_confidence": None},
    {"id": "q_5", "text": "Integer confidence", "status": "open",
     "suggested_answer": "Yes", "suggested_confidence": 1},
    {"id": "q_6", "status": "open", "asked_at": "3", "suggested_answer": "",
     "suggested_confidence": 0.125},
]

_FULL = {
    "meeting_id": "m_golden",
    "title": "Roadmap Sync",
    "topic": {"current": "Q3 Planning"},
    "rolling_summary": "Discussed roadmap and priorities.",
    "intent": {"text": "  We decide the vendor today. Capture who objected and why.  "},
    "participants": _PARTICIPANTS,
    "cards": _CARDS,
    "questions": _QUESTIONS,
    "recent_transcript_context": _SEGMENTS[:2],
}

_EMPTY = {"participants": {}, "cards": {}, "questions": []}

_GUIDED = {"participants": {}, "cards": {"user_notes": [
    {"id": "it_g1", "status": "edited", "author_type": "user", "text": "Correction: Entropic -> Anthropic",
     "data": {"kind": "term_correction", "selected_text": "Entropic", "replacement": "Anthropic"}},
    {"id": "it_g2", "status": "edited", "author_type": "user", "text": "We mean the hiring plan",
     "data": {"kind": "agent_insight"}, "evidence": ["sg_a2"]},
]}}

_REVIEW = dict(_FULL, notes_review_requested=True)
_REQUEST = dict(_FULL, note_adjustment_request="Use bullets\nand short lines")
_REQUEST_AND_REVIEW = dict(_REQUEST, notes_review_requested=True)
_VIEWS_BRIEF = dict(_FULL, report_views=["brief"])
_VIEWS_RIBBON = dict(_FULL, report_views=["signal", "ribbon"])

_VIEW_SETS = [None, [], ["ribbon"], ["brief"], ["signal"], ["ribbon", "brief"],
              ["ribbon", "signal"], ["brief", "signal"], ["ribbon", "brief", "signal"],
              ["other"], "ribbon", {"ribbon": True}]

_SPOTLIGHT_CARDS = {
    "key_points": [
        {"id": "it_1", "text": "Launch beta version on Monday", "status": "proposed",
         "pinned": False, "updated_at": "2026-08-16T10:00:00Z"},
        {"id": "it_2", "text": "Different key takeaway regarding architecture",
         "status": "proposed", "pinned": False, "updated_at": "2026-08-16T10:05:00Z"},
        {"id": "it_6", "text": "Tie A", "status": "proposed", "updated_at": "2026-08-16T09:00:00Z"},
        {"id": "it_7", "text": "Tie B", "status": "proposed", "updated_at": "2026-08-16T09:00:00Z"},
    ],
    "decisions": [
        {"id": "it_3", "text": "Launch beta version by Monday morning", "status": "confirmed",
         "pinned": False, "updated_at": "2026-08-16T10:02:00Z"},
        {"id": "it_4", "text": "Adopt SQLite for persistence", "status": "confirmed",
         "pinned": True, "updated_at": "2026-08-16T10:01:00Z"},
    ],
    "action_items": [
        {"id": "it_5", "text": "Deploy staging server", "status": "edited", "pinned": False,
         "updated_at": "2026-08-16T10:03:00Z"},
        {"id": "it_8", "text": "Deploy the staging server tonight please", "status": "proposed",
         "created_at": "2026-08-16T11:00:00Z"},
    ],
    "timeline": [{"id": "beat", "text": "Seasoning.", "status": "proposed"},
                 {"id": "beat2", "text": "Pinned beat", "status": "proposed", "pinned": True}],
    "risks": [{"id": "rep", "text": "Repair sample", "status": "proposed",
               "author_type": "system", "author_id": "state_repair"},
              {"id": "rep2", "text": "Synthesized repair", "status": "proposed",
               "author_type": "system", "author_id": "state_repair",
               "data": {"insight_synthesized": True}},
              {"id": "rep3", "text": "Truthy but not True", "status": "proposed",
               "author_type": "system", "author_id": "state_repair",
               "data": {"insight_synthesized": 1}}],
    "live_notes": [{"id": "it_note", "text": "Note taker block", "status": "proposed",
                    "pinned": True, "updated_at": "2026-08-16T10:09:00Z"}],
    "user_notes": [{"id": "it_user", "text": "User", "status": "edited"}],
}

_DUPLICATES = {
    "key_points": [
        {"id": "a", "text": "Alpha beta gamma delta epsilon", "status": "proposed",
         "updated_at": "5"},
        {"id": "b", "text": "alpha, BETA gamma delta!", "status": "proposed", "updated_at": "4"},
        {"id": "c", "text": "!!!", "status": "proposed", "updated_at": "3"},
        {"id": "d", "text": "", "status": "proposed", "updated_at": "2"},
        {"id": "e", "text": "Zeta eta theta", "status": "proposed", "updated_at": "1"},
        {"id": "a", "text": "Same id again", "status": "proposed", "updated_at": "0"},
    ],
    "decisions": [
        {"id": "f", "text": "one two three four five six seven eight", "status": "proposed",
         "updated_at": "6"},
        {"id": "g", "text": "one two three nine ten eleven twelve", "status": "proposed",
         "updated_at": "6"},
        {"id": "h", "text": "Ünïcode Straße résumé", "status": "proposed", "updated_at": "7"},
        {"id": "i", "text": "unicode strasse resume", "status": "proposed", "updated_at": "7"},
    ],
}

CASES = {
    "build_system_prompt": [[]],
    "build_note_taker_system_prompt": [[]],
    "build_consolidation_instructions": [[views] for views in _VIEW_SETS],
    "build_checkpoint_user_prompt": [
        [_EMPTY, [], False, False],
        [_EMPTY, [], True, False],
        [_EMPTY, _SEGMENTS[:1], False, True],
        [{}, [], False, False],
        [_FULL, _SEGMENTS, False, False],
        [_FULL, _SEGMENTS, False, True],
        [_FULL, _SEGMENTS, True, False],
        [_FULL, _SEGMENTS, True, True],
        [_VIEWS_BRIEF, _SEGMENTS, True, False],
        [_VIEWS_RIBBON, [], True, False],
        [_GUIDED, [], False, False],
        [dict(_GUIDED, intent={"text": "Watch the budget."}), _SEGMENTS[1:3], False, False],
    ],
    "build_notes_user_prompt": [
        [_EMPTY, []],
        [{"cards": {}, "participants": {}}, []],
        [_FULL, _SEGMENTS],
        [_GUIDED, _SEGMENTS[:1]],
        [_REVIEW, []],
        [_REQUEST, _SEGMENTS[:2]],
        [_REQUEST_AND_REVIEW, []],
        [{"cards": {}, "note_adjustment_request": "Use bullets"}, []],
        [{"cards": {"user_notes": [
            {"text": "Customer spells it Acme", "status": "edited"},
            {"text": "deleted-secret", "status": "removed"},
        ]}}, []],
    ],
    "intent_prompt": [
        [{}], [{"intent": None}], [{"intent": {"text": "   "}}],
        [{"intent": {"text": "  Brief with spaces \n"}}], [{"intent": {"text": 42}}],
        [_FULL],
    ],
    "render_state_compact": [
        [{}], [_EMPTY], [_FULL], [_GUIDED],
        [{"questions": _QUESTIONS}],
        [{"title": "", "topic": {"current": ""}, "rolling_summary": None,
          "participants": {"p": {}}, "cards": {"key_points": [{"text": "No id"}]}}],
        [{"cards": {"key_points": [{"id": "spoken", "text": "Budget A needs $1,000.",
                                    "author_type": "system", "author_id": "voice_command",
                                    "status": "proposed"}]}}],
    ],
    "render_notes_page": [[{}], [_EMPTY], [_FULL], [_GUIDED]],
    "format_segment_line": [[segment, _PARTICIPANTS] for segment in _SEGMENTS] + [
        [{"id": "sg_x", "channel": "mic", "start_s": 1e-7, "text": "tiny",
          "speaker_participant_id": "p_me"}, {}],
        [{"id": "sg_y", "start_s": 123456.75, "text": "  padded  "}, {}],
        [{"start_s": 2.5}, {}],
    ],
    "select_spotlight_items": [
        [_SPOTLIGHT_CARDS, 3], [_SPOTLIGHT_CARDS, 1], [_SPOTLIGHT_CARDS, 5],
        [_SPOTLIGHT_CARDS, 10], [_SPOTLIGHT_CARDS, 0], [_DUPLICATES, 3], [_DUPLICATES, 10],
        [_CARDS, 3], [{}, 3], [None, 3],
    ],
}

TRACE = {
    "tests": [
        "tests/test_meeting_notes_agent.py",
        "tests/test_meeting_intent.py",
        "tests/test_meeting_spotlight_context.py",
        "tests/test_meeting_scheduler.py",
    ],
    "functions": [
        "build_checkpoint_user_prompt",
        "build_notes_user_prompt",
        "build_consolidation_instructions",
        "intent_prompt",
        "render_state_compact",
        "render_notes_page",
        "format_segment_line",
        "select_spotlight_items",
    ],
}
