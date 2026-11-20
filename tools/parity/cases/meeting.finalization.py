"""Curated inputs for meeting/finalization.py (M7.9 meeting-core).

    polish_blocks(rows)                 [[start, end], ...]: each block as indices into rows
    polish_blocks_lengths(lengths)      the same over rows {"id": "sg_<i>", "text": "x" * length}
    summary_stats(cards, questions, transcript, duration_s)
    saved_state_detail(stats), insights_ready_message(stats), failed_steps_message(steps)
    sparse_redecode_detail(new_words, old_words)
    make_step(step_id, status)          the STEP_NAMES / STEP_DETAILS table
    speaker_pass_gate(backend, consent, key, today)
        key: None (find_key=None, no key needed) or the string find_key returns; today: an ISO
        date. Result {ok, reason, offered, has_key}: the key itself is never recorded.
    speaker_step_outcome(result)        meeting/refinalize.py's, which EndRun reports with

OpenWhisper_CPP's redecode detail names Parakeet where OpenWhisper's names Whisper (lead decision);
the replay maps that one sentence.
"""

import random
from datetime import date


def _rows(lengths):
    return [{"id": f"sg_{i}", "text": "x" * length} for i, length in enumerate(lengths)]


def _ranges(rows, blocks):
    ranges = []
    for block in blocks:
        start = next(i for i, row in enumerate(rows) if row is block[0])
        ranges.append([start, start + len(block)])
    return ranges


def _polish_blocks(module, rows):
    return _ranges(rows, module.polish_blocks(rows))


def _polish_blocks_lengths(module, lengths):
    rows = _rows(lengths)
    return _ranges(rows, module.polish_blocks(rows))


def _speaker_pass_gate(module, backend, consent, key, today):
    find_key = None if key is None else (lambda: key)
    gate = module.speaker_pass_gate(backend=backend, consent=consent, find_key=find_key,
                                    today=date.fromisoformat(today))
    return {"ok": gate.ok, "reason": gate.reason, "offered": gate.offered,
            "has_key": bool(gate.api_key)}


def _speaker_step_outcome(module, result):
    from meeting.refinalize import speaker_step_outcome

    return list(speaker_step_outcome(result))


def _mixed_lengths():
    # tests/test_meeting_polish_batches.py::test_mixed_lengths_always_advance_and_preserve_text
    rng = random.Random(419)
    return [rng.choice([0, 30, 200, 2_000, 7_000, 9_000]) for _ in range(435)]


_CARDS = {
    "key_points": [{"status": "active"}, {"status": "removed"}, {"status": None}, {}],
    "action_items": [{"status": "removed"}],
    "decisions": [{"status": "edited"}, "plain"],
    "risks": [],
}
_QUESTIONS = [{"status": "dismissed"}, {"status": "open"}, {"status": ""}, {}]
_TRANSCRIPT = [
    {"text": "Hello there, team."},
    {"text": "  spaced\tout\nwords  "},
    {"text": ""},
    {"text": None},
    {},
    {"text": "café  non breaking"},
]

_STEP = {"id": "polish", "name": "Transcript Cleanup", "status": "failed", "detail": "Timed out."}

CASES = {
    "polish_blocks_lengths": [
        [[]], [[10]], [[10] * 79], [[10] * 80], [[10] * 81], [[10] * 144], [[10] * 152],
        [[10] * 160], [[10] * 435], [[2_000] * 12], [[7, 8_001, 6]], [[8_000, 8_000]],
        [[0] * 200], [[4_000, 4_000, 4_000]], [[7_999, 2, 1]], [_mixed_lengths()],
    ],
    "polish_blocks": [
        [[{"id": "a", "text": "é" * 4_000}, {"id": "b", "text": "é" * 4_000},
          {"id": "c", "text": "x"}]],
        [[{"id": "a", "text": None}, {"id": "b"}, {"id": "c", "text": ""}]],
    ],
    "summary_stats": [
        [_CARDS, _QUESTIONS, _TRANSCRIPT, 125.5],
        [{}, [], [], 0.0],
        [None, None, [], -3.0],
        [[], [{"status": "dismissed"}], [{"text": "one two"}], 7],
        [{"key_points": None, "action_items": [], "decisions": None}, None, [{"text": "a"}], 1e9],
    ],
    "saved_state_detail": [
        [{"segments": 32, "words": 410}], [{"segments": 0, "words": 0}],
    ],
    "insights_ready_message": [
        [{"segments": 32, "words": 410, "key_points": 4, "action_items": 0, "decisions": 1}],
        [{"segments": 1}],
        [{"segments": 0, "key_points": 2, "action_items": 3, "decisions": 4}],
        [{"segments": 5, "key_points": None, "action_items": 1}],
    ],
    "failed_steps_message": [
        [[_STEP, dict(_STEP, id="consolidation", name="Summary & Action Items",
                      detail="Provider unavailable."),
          {"id": "finalize", "name": "State Finalization", "status": "completed",
           "detail": "Saved"}]],
        [[{"id": "polish", "status": "failed"}]],
        [[{"id": "polish", "status": "completed"}]],
        [[]],
        [[{"status": "failed"}]],
        [[{"id": "redecode", "name": "", "status": "failed", "detail": ""},
          {"id": "speaker_id", "name": "Speaker Identification", "status": "failed",
           "detail": "Quota."}]],
    ],
    "sparse_redecode_detail": [[12, 300], [0, 0]],
    "make_step": [
        ["redecode", "pending"], ["speaker_id", "running"], ["polish", "failed"],
        ["consolidation", "completed"], ["finalize", "pending"], ["unknown", "pending"],
    ],
    "speaker_pass_gate": [
        [backend, True, "sk-test", "2026-11-16"] for backend in ("off", "local", "", "bogus")
    ] + [
        ["openai", True, "sk-test", "2027-02-26"],
        ["openai", True, "sk-test", "2027-02-25"],
        ["openai", False, "sk-test", "2026-11-16"],
        ["openai", False, "sk-test", "2027-03-01"],
        ["openai", True, "", "2026-11-16"],
        ["openai", True, None, "2026-11-16"],
        ["OpenAI", True, "sk-test", "2026-11-16"],
    ],
    "speaker_step_outcome": [
        [{"ok": True, "applied": 1}], [{"ok": True, "applied": 3}], [{"ok": True}],
        [{"ok": True, "applied": "2"}], [{"ok": False, "skipped": True}],
        [{"ok": False, "skipped": True, "error": "No OpenAI API key is configured."}],
        [{"ok": False, "error": "Upload failed."}], [{}],
    ],
}

WRAPPERS = {
    "polish_blocks": _polish_blocks,
    "polish_blocks_lengths": _polish_blocks_lengths,
    "speaker_pass_gate": _speaker_pass_gate,
    "speaker_step_outcome": _speaker_step_outcome,
}

PINS = {
    "polish_note": "polish_blocks results are [start, end) index ranges into the rows",
    "gate_note": "speaker_pass_gate records has_key, never the key",
}
