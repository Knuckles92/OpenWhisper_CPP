"""Curated inputs for ui_qt/dialogs/cleanup_levels.py (M4.4 text-prompts): the level
control's segments and notes, and segment_index. The tables are compared whole, so a retyped
character in fv_text's copy fails the replay.
"""

CASES = {
    "SEGMENTS": [[]],
    "NOTES": [[]],
    "SHORT_NOTES": [[]],
    "segment_index": [[value] for value in (
        "light", "medium", "high", "custom", "none", "", "Custom", "bogus")],
}

WRAPPERS = {
    "SEGMENTS": lambda module: [list(segment) for segment in module.SEGMENTS],
    "NOTES": lambda module: dict(module.NOTES),
    "SHORT_NOTES": lambda module: dict(module.SHORT_NOTES),
}
