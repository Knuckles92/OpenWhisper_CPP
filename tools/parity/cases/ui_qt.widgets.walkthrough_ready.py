"""Curated inputs for ui_qt/widgets/walkthrough_ready.py (fv_ui_walkthrough's WalkthroughReadyCard).

    present(name, shots, duration_s, copy_format, auto_paste, write_up_error, copied)

presents a WalkthroughReadyCard for a stand-in result, with Settings' walkthrough_copy_format and
auto_paste as given (what present reads through settings_manager.load_all_settings), and reads
what the card then shows: {"title", "subtitle", "reason" (the label's text), "reason_visible"}.
The result has no first screenshot and its files do not exist, so only the words are in play; the
card is built on an offscreen QApplication and never shown to anyone.
"""

import os
from types import SimpleNamespace

_APP = []

REASON = ("Claude Code is not installed on this computer. Choose another agent "
          "in Settings → Dictation → Walkthroughs.")


def _app():
    os.environ["QT_QPA_PLATFORM"] = "offscreen"
    from PyQt6.QtWidgets import QApplication
    if not _APP:
        _APP.append(QApplication.instance() or QApplication([]))
    return _APP[0]


def _present(module, name, shots, duration_s, copy_format, auto_paste, write_up_error, copied):
    from pathlib import Path
    from unittest.mock import patch

    _app()
    folder = Path("/nonexistent")
    result = SimpleNamespace(name=name, shots=shots, duration_s=duration_s, first_shot=None,
                             write_up_error=write_up_error, folder=folder,
                             pdf=folder / "walkthrough.pdf", docx=folder / "walkthrough.docx")
    settings = {"walkthrough_copy_format": copy_format, "auto_paste": auto_paste}
    card = module.WalkthroughReadyCard()
    try:
        with patch.object(module.settings_manager, "load_all_settings", lambda: settings):
            card.present(result, copied=copied)
        return {"title": card.title.text(), "subtitle": card.subtitle.text(),
                "reason": card.reason.text(), "reason_visible": card.reason.isVisibleTo(card)}
    finally:
        card.hide()
        card.deleteLater()


WRAPPERS = {"present": _present}

CASES = {
    "present": (
        # Every copy format, auto-paste on and off, with and without a write-up failure, copied.
        [["Walkthrough", 3, 72, fmt, auto_paste, error, True]
         for fmt in ("pdf", "agent_task", "both", "agent", "word")
         for auto_paste in (True, False)
         for error in ("", REASON)]
        # A walkthrough that was written up, and the singular, the minute and the hour.
        + [["Write-up", 1, 11, "pdf", True, "", True],
           ["Write-up", 3, 72, "agent_task", True, "", True],
           ["Write-up", 0, 0, "pdf", True, "", True],
           ["Walkthrough", 2, 59.9, "pdf", True, "", True],
           ["Walkthrough", 12, 3723, "agent_task", False, "", True],
           ["Walkthrough", 1, -5, "pdf", True, "", True]]
        # Copying failed: the format and the failure change nothing but the reason line.
        + [["Walkthrough", shots, 75, fmt, True, error, False]
           for shots in (1, 4)
           for fmt in ("pdf", "agent_task")
           for error in ("", REASON)]
    ),
}
