"""Curated inputs for ui_qt/widgets/past_walkthroughs.py (fv_ui_walkthrough's past_walkthroughs:
the card title, summarize's text fields and the search filter).

    _title(markdown, texts)
        _title of a new temporary folder holding write-up.md with `markdown` (None: no such file;
        a str is written as UTF-8 bytes, newlines as given; {"hex": "..."} is written as those
        bytes) and a stand-in document whose sections say `texts`.
    summarize(folder)
        summarize(folder, date(2026, 10, 25)) of a folder built in a new temporary folder from
        `folder`: {"name", "manifest" (a dict written as JSON, a str written as is, None: none),
        "draft" (draft.json's text or None), "write_up" (write-up.md's text or None),
        "write_up_pdf" (write-up.pdf is written), "shots" ([[width, height], ...]: shot-<n>.png
        filled with one colour)}. The result is the card's text fields, `when` as
        %Y-%m-%dT%H:%M:%S or None, and the thumbnail's size or None.
    _filter(words, search, total)
        _filter of a PastWalkthroughsPanel holding stand-in cards with `words`, its search box
        holding `search`, after a read that found `total` folders (None: none finished yet).
        The result is each card's visibility and both lines' text and visibility.

Widgets are built on an offscreen QApplication and never shown.
"""

import json
import os
import tempfile
from datetime import date
from pathlib import Path
from types import SimpleNamespace

_APP = []
TODAY = date(2026, 10, 25)


def _app():
    os.environ["QT_QPA_PLATFORM"] = "offscreen"
    from PyQt6.QtWidgets import QApplication
    if not _APP:
        _APP.append(QApplication.instance() or QApplication([]))
    return _APP[0]


def _bytes(value):
    if isinstance(value, dict):
        return bytes.fromhex(value["hex"])
    return value.encode("utf-8")


def _title(module, markdown, texts):
    with tempfile.TemporaryDirectory() as scratch:
        folder = Path(scratch)
        if markdown is not None:
            (folder / "write-up.md").write_bytes(_bytes(markdown))
        document = SimpleNamespace(sections=[SimpleNamespace(text=text) for text in texts])
        return module._title(folder, document)


def _summarize(module, spec):
    _app()
    from PyQt6.QtGui import QColor, QImage

    with tempfile.TemporaryDirectory() as scratch:
        folder = Path(scratch) / spec["name"]
        folder.mkdir()
        manifest = spec.get("manifest")
        if isinstance(manifest, dict):
            (folder / "walkthrough.json").write_text(json.dumps(manifest), encoding="utf-8")
        elif manifest is not None:
            (folder / "walkthrough.json").write_bytes(manifest.encode("utf-8"))
        if spec.get("draft") is not None:
            (folder / "draft.json").write_bytes(spec["draft"].encode("utf-8"))
        if spec.get("write_up") is not None:
            (folder / "write-up.md").write_bytes(spec["write_up"].encode("utf-8"))
        if spec.get("write_up_pdf"):
            (folder / "write-up.pdf").write_bytes(b"%PDF-1.4")
        for number, (width, height) in enumerate(spec.get("shots", []), 1):
            image = QImage(width, height, QImage.Format.Format_RGB32)
            image.fill(QColor("#3a6ea5"))
            image.save(str(folder / f"shot-{number}.png"))
        summary = module.summarize(folder, TODAY)
        return {
            "title": summary.title,
            "meta": summary.meta,
            "pill": summary.pill,
            "pill_tone": summary.pill_tone,
            "words": summary.words,
            "when": None if summary.when is None else summary.when.strftime("%Y-%m-%dT%H:%M:%S"),
            "thumb": None if summary.thumb is None
            else [summary.thumb.width(), summary.thumb.height()],
        }


class _Card:
    def __init__(self, words):
        self.summary = SimpleNamespace(words=words)
        self.visible = None

    def setVisible(self, visible):
        self.visible = bool(visible)


def _filter(module, words, search, total):
    _app()
    panel = module.PastWalkthroughsPanel(root=Path(tempfile.gettempdir()) / "unused")
    try:
        panel.cards = [_Card(each) for each in words]
        panel._total = total
        panel.search_input.blockSignals(True)
        panel.search_input.setText(search)
        panel.search_input.blockSignals(False)
        panel._filter()
        return {
            "visible": [card.visible for card in panel.cards],
            "count": panel.count_header.text(),
            "count_shown": not panel.count_header.isHidden(),
            "message": panel.empty_label.text(),
            "message_shown": not panel.empty_label.isHidden(),
        }
    finally:
        panel.cards = []
        panel.deleteLater()


WRAPPERS = {
    "_title": _title,
    "summarize": _summarize,
    "_filter": _filter,
}


def _manifest(sections, started="2026-10-25T22:46:05", duration_s=106.0):
    listed = []
    for number, (text, with_shot) in enumerate(sections, 1):
        listed.append({"text": text, "shot": f"shot-{number}.png" if with_shot else None,
                       "kept_at_s": float(number)})
    return {"version": 1, "started_at": started, "duration_s": duration_s, "sections": listed}


_LONG = "word " * 40
_NINETY = "a" * 89 + " b"

CASES = {
    "_title": [
        ["# Rethinking the Recent area\n\nBody.", ["spoken words"]],
        ["---\nfont: Georgia\n---\n\n# Rethinking the Recent area\n\nBody.", ["x"]],
        ["# Title with closing hashes ##\n", []],
        ["#   Spaced   out   \n", []],
        ["#Title without a space\n", ["the words"]],
        ["Intro line\n# Second line heading\n", []],
        ["## Not a top heading\n", ["fallback words"]],
        ["#\nTitle on the next line\n", []],
        ["# Windows ends\r\nBody\r\n", []],
        ["# Old Mac ends\rBody\r", []],
        ["﻿# Behind a byte order mark\n", ["after the mark"]],
        ["# Café — “quotes” \U0001F600\n", []],
        ["# No-break space\n", []],
        ["# ###\n", ["only hashes"]],
        ["#  \n# Real one\n", []],
        [{"hex": "232054c3a974c3a920ff0a"}, ["bad bytes fall back"]],
        ["", ["empty file"]],
        [None, ["first", "second", "last"]],
        [None, ["  spaced\n\nout   words  ", "", "   ", "next\tone"]],
        [None, []],
        [None, ["", "  \n "]],
        [None, [_LONG]],
        [None, [_NINETY]],
        [None, ["a" * 90]],
        [None, ["a" * 91]],
        [None, ["a" * 95 + " tail"]],
        [None, ["é" * 85 + " " + "é" * 10]],
        [None, ["\U0001F600" * 50 + " end " + "x" * 50]],
        [None, ["one two three " * 10]],
    ],
    "summarize": [
        [{"name": "2026-10-25_22-46-05",
          "manifest": _manifest([("I think we don't need this whole recent area", True),
                                 ("and this", True)]),
          "write_up": "---\nfont: Georgia\n---\n\n# Rethinking the Recent area\n\nBody.",
          "write_up_pdf": True, "shots": [[400, 300], [400, 300]]}],
        [{"name": "2026-10-25_09-00-00",
          "manifest": _manifest([(_LONG, False)], started="2026-10-25T09:00:00")}],
        [{"name": "2026-10-24_09-00-00", "draft": '{"started_at": "2026-10-24T09:00:00"}'}],
        [{"name": "2026-10-24_09-00-00", "draft": "not json"}],
        [{"name": "2026-10-20_18-30-00"}],
        [{"name": "notes"}],
        [{"name": "2026-10-23_08-15-00", "manifest": "{broken"}],
        [{"name": "2026-10-25_10-30-00",
          "manifest": _manifest([("The History panel", True), ("Second shot", True),
                                 ("closing words", False)], started="2026-10-25T10:30:00",
                                duration_s=3723.0),
          "write_up": "# Heading only in the write-up\n",
          "shots": [[1600, 900], [50, 600]]}],
        [{"name": "2026-10-25_11-00-00",
          "manifest": _manifest([("Tall shot", True)], started="2026-10-25T11:00:00",
                                duration_s=9.5),
          "write_up_pdf": True, "shots": [[300, 2000]]}],
        [{"name": "2026-10-25_12-00-00",
          "manifest": _manifest([("Shot missing on disk", True)], started="2026-10-25T12:00:00",
                                duration_s=0.0)}],
        [{"name": "2025-12-31_23-59-59",
          "manifest": _manifest([("", False)], started="2025-12-31T23:59:59", duration_s=59.9)}],
        [{"name": "2026-10-25_13-00-00",
          "manifest": _manifest([("Started elsewhere", False)],
                                started="2026-10-25T13:00:00+02:00", duration_s=12.0)}],
    ],
    "_filter": [
        [["the settings dialog is slow", "the history panel"], "History  PANEL", 2],
        [["the settings dialog is slow", "the history panel"], "nothing like this", 2],
        [["the settings dialog is slow", "the history panel"], "", 2],
        [["the settings dialog is slow", "the history panel"], "the", 2],
        [["one"], "", 1],
        [["one"], "ONE", 1],
        [[], "", 0],
        [[], "anything", 0],
        [["at 9", "at 8"], "", 3],
        [["at 9", "at 8"], "at", 3],
        [["at 9", "at 8"], "9", 3],
        [["at 9", "at 8"], "", None],
        [["café notes"], "CAFÉ", 1],
        [["two words here"], "  words\t here  ", 1],
    ],
}
