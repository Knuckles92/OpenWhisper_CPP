"""Curated inputs for ui_qt/dialogs/settings_snippets.py (M4.4 text-prompts): the snippet
count and the Snippets rail value.
"""


def _row(index):
    return {"id": f"s{index}", "trigger": f"trigger number {index}", "text": "Text.",
            "formatted": False}


_SETTINGS = [
    {},
    {"snippets_enabled": False},
    {"snippets_enabled": False, "dictation_snippets": [_row(1)]},
    {"dictation_snippets": []},
    {"dictation_snippets": [_row(1)]},
    {"dictation_snippets": [_row(1), _row(2)]},
    {"dictation_snippets": [_row(1), _row(1)]},
    {"dictation_snippets": "oops"},
    {"snippets_enabled": "yes", "dictation_snippets": [_row(3)]},
]

CASES = {
    "_count": [[settings] for settings in _SETTINGS],
    "rail_value": [[settings] for settings in _SETTINGS],
}
