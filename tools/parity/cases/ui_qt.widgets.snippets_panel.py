"""Curated inputs for ui_qt/widgets/snippets_panel.py (M4.4 text-prompts): the editor's
preview line, from SnippetsPanel._update_hints run on a stub panel (no widgets, no
QApplication). `_update_hints(draft)` returns the preview text for a draft snippet given as
{id, trigger, text, formatted}; the draft's fields are what the panel's _draft() hands it.
"""

from types import SimpleNamespace


class _Label:
    def __init__(self):
        self.text = ""

    def setText(self, text):
        self.text = text

    def setVisible(self, _visible):
        pass


def _update_hints(module, draft):
    from services.snippets import Snippet

    snippet = Snippet(**draft)
    stub = SimpleNamespace(_draft=lambda: snippet, _snippets=[], trigger_note=_Label(),
                           preview=_Label())
    original = module.set_style_property
    module.set_style_property = lambda *_args: None
    try:
        module.SnippetsPanel._update_hints(stub)
    finally:
        module.set_style_property = original
    return stub.preview.text


def _draft(trigger, text, formatted=False):
    return {"id": "d", "trigger": trigger, "text": text, "formatted": formatted}


CASES = {
    "_update_hints": [[draft] for draft in (
        _draft("", ""),
        _draft("", "Some text."),
        _draft("my sign-off", ""),
        _draft("my sign-off", "  \n "),
        _draft("my sign-off", "B"),
        _draft("my sign-off", "Best, Alex"),
        _draft("my sign-off", "Best, Alex", True),
        _draft("my sign-off", "**Best**, [Alex](https://example.com)", True),
        _draft("my sign-off", "**Best**, Alex", False),
        _draft("big", "x" * 1234),
        _draft("big", "y" * 10000, True),
        _draft("café", "éé"),
    )],
}

WRAPPERS = {"_update_hints": _update_hints}
