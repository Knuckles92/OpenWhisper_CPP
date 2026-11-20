"""TranscriptionHistory.preview_text and history_context of services/models.py (M1.13).

preview_text cuts in code points, not UTF-8 bytes or UTF-16 units, at the last space (U+0020
only) inside the first 100.
"""

_WORDS = " ".join(["word"] * 30)


def _preview_text(module, text):
    return module.TranscriptionHistory(text=text).preview_text


def _history_context(module, values):
    return module.history_context(values)


CASES = {
    "preview_text": [
        [None], [""], ["   "], ["\n\t "], ["short"], ["  padded  "], ["x" * 100], ["x" * 101],
        ["x" * 99 + " yz"], ["x" * 100 + " yz"], ["x" * 98 + " y z"], [_WORDS],
        [" " + _WORDS + " "], ["é" * 101], ["\U0001F600" * 60 + " " + "\U0001F600" * 60],
        ["我喜欢你" * 30], ["a " * 60], ["a\tb " * 30], ["x" * 100 + " "],
        [" " * 3 + "y" * 120],
    ],
    "history_context": [
        [{}], [{"app_name": "Outlook", "language": None}],
        [{"app_id": "a", "app_name": "b", "app_category": "c", "cleanup_level": "d",
          "entry_kind": "e", "cleaned_text": "f", "language": "g"}],
        [{"window_title": "Inbox"}], [{"zeta": 1, "alpha": 2, "app_name": "x"}],
    ],
}

WRAPPERS = {
    "preview_text": _preview_text,
    "history_context": _history_context,
}
