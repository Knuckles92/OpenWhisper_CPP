"""Curated inputs for ui_qt/widgets/walkthrough_review.py (fv_ui_walkthrough's review_text and
WalkthroughReview's header, version picker and notices).

    _day(when, today)            when and today as isoformat text
    _time(when)
    _shots(count)
    _started(folder_name, document_started, draft_text)
        a folder named folder_name in a new temporary folder, holding draft.json with draft_text
        when it is not None; document_started (isoformat or None) stands for a loaded document's
        started_at. The result is the wall time %Y-%m-%dT%H:%M:%S, or None.
    _written_at(modified, today)
        write-up.md of a temporary folder last written at the naive local time `modified` (None:
        no such file), read with date.today() pinned to `today`.
    header(folder_name, started, shots, duration_s, draft, written, spoken)
        _show_header of a WalkthroughReview for a stand-in document whose sections have a
        screenshot where `shots` says true (None: no document loaded), its folder named
        folder_name (None: no folder); what the header then shows.
    versions(earlier, version, spoken, loaded, written)
        _show_versions with those earlier write-ups, `written` giving _written_at's answer per
        version ("None" for the latest); the picker's visibility and text and the menu's rows.
    copied(copy_format)          the notice after a copy (_COPIED); KeyError for one with none
    constant(name)               SPOKEN_HINT, WRITE_UP_HINT, SAVED, VERSIONS

Widgets are built on an offscreen QApplication and never shown.
"""

import os
import tempfile
from datetime import date, datetime
from pathlib import Path
from types import SimpleNamespace

_APP = []


def _app():
    os.environ["QT_QPA_PLATFORM"] = "offscreen"
    from PyQt6.QtWidgets import QApplication
    if not _APP:
        _APP.append(QApplication.instance() or QApplication([]))
    return _APP[0]


def _wall(when):
    return None if when is None else when.strftime("%Y-%m-%dT%H:%M:%S")


def _day(module, when, today):
    return module._day(datetime.fromisoformat(when), date.fromisoformat(today))


def _time(module, when):
    return module._time(datetime.fromisoformat(when))


def _started(module, folder_name, document_started, draft_text):
    with tempfile.TemporaryDirectory() as scratch:
        folder = Path(scratch) / folder_name
        folder.mkdir()
        if draft_text is not None:
            (folder / "draft.json").write_text(draft_text, encoding="utf-8")
        document = None
        if document_started is not None:
            document = SimpleNamespace(started_at=datetime.fromisoformat(document_started))
        return _wall(module._started(folder, document))


def _written_at(module, modified, today):
    pinned = date.fromisoformat(today)

    class _Today(date):
        @classmethod
        def today(cls):
            return pinned

    with tempfile.TemporaryDirectory() as scratch:
        folder = Path(scratch)
        if modified is not None:
            target = folder / "write-up.md"
            target.write_text("# x", encoding="utf-8")
            stamp = datetime.fromisoformat(modified).timestamp()
            os.utime(target, (stamp, stamp))
        review = SimpleNamespace(_folder=folder)
        saved = module.date
        module.date = _Today
        try:
            return module.WalkthroughReview._written_at(review, None)
        finally:
            module.date = saved


def _review(module):
    _app()
    return module.WalkthroughReview()


def _header(module, folder_name, started, shots, duration_s, draft, written, spoken):
    review = _review(module)
    try:
        with tempfile.TemporaryDirectory() as scratch:
            review._folder = Path(scratch) / folder_name if folder_name is not None else None
            review._written = written
            review._spoken = spoken
            review._earlier = []
            walkthrough = None
            if shots is not None:
                walkthrough = SimpleNamespace(
                    started_at=datetime.fromisoformat(started) if started else None,
                    duration_s=duration_s,
                    sections=[SimpleNamespace(shot=Path("shot.png") if shot else None)
                              for shot in shots])
            review._show_header(walkthrough, draft)
            return {
                "title": review.title.text(),
                "title_shown": not review.title.isHidden(),
                "meta": review.meta.text(),
                "pill": review.kind.text(),
                "pill_tone": review.kind.property("tone"),
                "pill_shown": not review.kind.isHidden(),
                "version_bar_shown": not review.version_bar.isHidden(),
                "version_bar_index": review.version_bar.currentIndex(),
                "retry": review.retry_button.text(),
                "style_menu": review.style_menu.title(),
                "prompt": review.prompt_button.text(),
                "notice": review.notice.text(),
            }
    finally:
        review.deleteLater()


def _versions(module, earlier, version, spoken, loaded, written):
    review = _review(module)
    try:
        review._earlier = list(earlier)
        review._version = version
        review._spoken = spoken
        review._written_at = lambda chosen: written[str(chosen)]
        review._show_versions(loaded)
        return {
            "shown": not review.version_picker.isHidden(),
            "text": review.version_picker.text(),
            "rows": [[action.text(), action.isChecked()]
                     for action in review.version_menu.actions()],
        }
    finally:
        review.deleteLater()


def _copied(module, copy_format):
    return module._COPIED[copy_format]


def _constant(module, name):
    value = getattr(module, name)
    return [list(pair) for pair in value] if name == "VERSIONS" else value


WRAPPERS = {
    "_day": _day,
    "_time": _time,
    "_started": _started,
    "_written_at": _written_at,
    "header": _header,
    "versions": _versions,
    "copied": _copied,
    "constant": _constant,
}

_TODAY = "2026-10-21"

CASES = {
    "_day": [
        ["2026-10-21T14:32:05", _TODAY],
        ["2026-10-21T00:00:00", _TODAY],
        ["2026-10-20T23:59:59", _TODAY],
        ["2026-10-19T09:05:00", _TODAY],
        ["2026-01-01T12:00:00", _TODAY],
        ["2026-10-01T08:00:00", _TODAY],
        ["2025-10-21T14:32:05", _TODAY],
        ["2025-12-31T18:00:00", "2026-01-01"],
        ["2025-12-30T18:00:00", "2026-01-01"],
        ["2024-02-29T07:15:00", _TODAY],
        ["2026-10-22T10:00:00", _TODAY],
        ["2026-10-21T14:32:05+02:00", _TODAY],
    ],
    "_time": [
        ["2026-10-21T00:00:00"],
        ["2026-10-21T00:05:59"],
        ["2026-10-21T09:05:00"],
        ["2026-10-21T10:30:00"],
        ["2026-10-21T12:00:00"],
        ["2026-10-21T12:59:00"],
        ["2026-10-21T13:01:00"],
        ["2026-10-21T14:32:05"],
        ["2026-10-21T23:59:59"],
        ["2026-10-21T21:07:00-05:00"],
    ],
    "_shots": [[0], [1], [2], [3], [12], [101]],
    "_started": [
        ["2026-10-21_14-32-05", "2026-10-20T09:15:00", None],
        ["2026-10-21_14-32-05", "2026-10-20T09:15:00+02:00", None],
        ["2026-10-21_14-32-05", None, None],
        ["2026-10-21_14-32-05-2", None, None],
        ["2026-10-21_14-32-05", None, '{"started_at": "2026-10-19T08:00:01"}'],
        ["2026-10-21_14-32-05", None, '{"started_at": "2026-10-19T08:00:01.250000+05:30"}'],
        ["2026-10-21_14-32-05", None, '{"started_at": "not a time"}'],
        ["2026-10-21_14-32-05", None, '{"started_at": 12}'],
        ["2026-10-21_14-32-05", None, '{"version": 1}'],
        ["2026-10-21_14-32-05", None, "not json"],
        ["2026-10-21_14-32-05", None, "[1, 2]"],
        ["notes", None, None],
        ["2026-13-21_14-32-05", None, None],
        ["2026-02-30_14-32-05", None, None],
        ["2026-1-5_4-3-2", None, None],
        ["2026-10-21 14-32-05", None, None],
        ["2026-10-21_24-00-00", None, None],
        ["2026-10-21_23-59-60", None, None],
    ],
    "_written_at": [
        [None, _TODAY],
        ["2026-10-21T09:05:00", _TODAY],
        ["2026-10-21T15:40:30", _TODAY],
        ["2026-10-20T15:40:30", _TODAY],
        ["2026-01-02T08:00:00", _TODAY],
        ["2025-12-31T23:10:00", _TODAY],
    ],
    "header": [
        # No folder: nothing to say.
        [None, None, None, 0, False, False, True],
        # Spoken, as finished.
        ["2026-10-21_14-32-05", "2026-10-21T14:32:05", [True, True, False], 3.0, False, False,
         True],
        ["2026-10-21_14-32-05", "2026-10-20T09:05:00", [True], 72.0, False, False, True],
        ["2026-10-21_14-32-05", "2026-10-20T09:05:00", [False], 3723.0, False, False, True],
        # Written up: its write-up, or the words as spoken.
        ["2026-10-21_14-32-05", "2026-10-21T14:32:05", [True, True, False], 75.0, False, True,
         False],
        ["2026-10-21_14-32-05", "2026-10-21T14:32:05", [True, True, False], 75.0, False, True,
         True],
        # A draft: with its manifest, and without one (the folder's name says when).
        ["2026-10-21_14-32-05", "2026-10-21T14:32:05", [True, False], 12.0, True, False, True],
        ["2026-10-21_14-32-05", None, None, 0, True, False, True],
        # A folder that can't be opened: the name, if it is no time.
        ["notes", None, None, 0, False, False, True],
        ["2026-10-21_14-32-05", None, None, 0, False, False, True],
    ],
    "versions": [
        [[], None, False, True, {"None": "3:05 PM"}],
        [[1], None, False, True, {"None": "3:05 PM", "1": "Oct 20, 9:00 AM"}],
        [[1], 1, False, True, {"None": "3:05 PM", "1": "Oct 20, 9:00 AM"}],
        [[1, 2], 2, False, True, {"None": "", "1": "2:00 PM", "2": ""}],
        [[1, 3], None, False, True, {"None": "3:05 PM", "1": "2:00 PM", "3": "2:30 PM"}],
        [[1], None, True, True, {"None": "3:05 PM", "1": "2:00 PM"}],
        [[1], None, False, False, {"None": "3:05 PM", "1": "2:00 PM"}],
    ],
    "copied": [["pdf"], ["agent_task"], ["agent"], ["word"], ["both"]],
    "constant": [["SPOKEN_HINT"], ["WRITE_UP_HINT"], ["SAVED"], ["VERSIONS"]],
}
