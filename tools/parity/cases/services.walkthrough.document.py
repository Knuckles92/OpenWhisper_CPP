"""Curated inputs for the Qt-free half of services/walkthrough/document.py (fv_walkthrough's
document.h).

Times are naive local isoformat() text; the manifest's offset is the machine's zone
(pins.local_timezone: Pacific, America/Los_Angeles rules, which the replay passes in explicitly).
Paths are given relative to a scratch root under out/parity and come back with that root written
as "<root>" (the platform's separators after it), so the replay puts its own root in its place.

    clock(seconds), _when(started_iso), _escape_markers(words), _paragraphs(words), _tidy(text)
    _image_target(path, folder, absolute)       path and folder relative to the root
    markdown(walkthrough, absolute, format)     the folder is <root>/capture
    write_manifest(walkthrough)                 the manifest's text

A walkthrough is {"started_at": iso, "duration_s": seconds, "sections": [[text, shot, kept_at_s,
omitted, notes]]} with shot and omitted relative to the root (or None); a format is [title,
details, about, captions].
"""
from __future__ import annotations

import json
import os
import shutil
import tempfile
from datetime import datetime
from pathlib import Path

REPO = Path(__file__).resolve().parents[3]
SCRATCH = REPO / "out" / "parity" / "walkthrough_document"


def _root() -> Path:
    SCRATCH.mkdir(parents=True, exist_ok=True)
    return Path(tempfile.mkdtemp(dir=SCRATCH))


def _rooted(text: str, root: Path) -> str:
    return text.replace(str(root), "<root>")


def _json_rooted(text: str, root: Path) -> str:
    # Inside JSON text the root's backslashes are escaped.
    return text.replace(json.dumps(str(root), ensure_ascii=False)[1:-1], "<root>")


def _walkthrough(module, value, root: Path):
    def path(relative):
        return None if relative is None else root / relative

    sections = tuple(
        module.Section(text, path(shot), kept, path(omitted), tuple(notes))
        for text, shot, kept, omitted, notes in value["sections"]
    )
    return module.Walkthrough(datetime.fromisoformat(value["started_at"]), value["duration_s"],
                              sections)


def _format(module, value):
    if value is None:
        return module.DocumentFormat()
    title, details, about, captions = value
    return module.DocumentFormat(title, details, about, captions)


def _when(module, started):
    return module._when(datetime.fromisoformat(started))


def _image_target(module, path, folder, absolute):
    root = _root()
    try:
        return _rooted(module._image_target(root / path, root / folder, absolute), root)
    finally:
        shutil.rmtree(root)


def _markdown(module, walkthrough, absolute, document_format):
    root = _root()
    try:
        text = module.markdown(_walkthrough(module, walkthrough, root), folder=root / "capture",
                               absolute_images=absolute,
                               document_format=_format(module, document_format))
        return _rooted(text, root)
    finally:
        shutil.rmtree(root)


def _write_manifest(module, walkthrough):
    root = _root()
    try:
        folder = root / "capture"
        folder.mkdir()
        path = module.write_manifest(_walkthrough(module, walkthrough, root), folder)
        return _json_rooted(path.read_bytes().decode("utf-8"), root)
    finally:
        shutil.rmtree(root)


WRAPPERS = {
    "_when": _when,
    "_image_target": _image_target,
    "markdown": _markdown,
    "write_manifest": _write_manifest,
}

STARTED = "2026-10-20T14:32:05"


def _three_sections():
    return {"started_at": STARTED, "duration_s": 136.4, "sections": [
        ["Okay, the update card is grayed out.", "capture/shot-1.png", 41.3, None, []],
        ["Next, this label is stale.", "capture/shot-2.png", 72.6, None, []],
        ["That's it.", None, None, None, []],
    ]}


ODD = {"started_at": "2027-01-04T00:05:59", "duration_s": 3725.9, "sections": [
    ["   ", None, None, None, []],
    ["", "capture/shot-a.png", 5.0, None, []],
    ["\n", None, None, None, ["held for the agent"]],
    ["Then this.\r\n# not a heading\r\r1) not a list", "capture/shot b.png", None, None, []],
    ["", "capture/sub dir/shot (3).png", 3599.9, None, []],
    ["Outside.", "elsewhere/shot-4.png", 61.0, None, []],
    ["", None, None, "capture/shot-9.png", []],
    ["Café — naïve “quotes”", None, None, None, ["one", "two"]],
]}

SHOTLESS = {"started_at": "2026-12-31T12:00:00", "duration_s": 9.99, "sections": [
    ["Just words.", None, None, None, []],
]}

EMPTY = {"started_at": "2026-02-08T02:30:00", "duration_s": 0.0, "sections": [
    ["", None, None, None, []],
]}

CASES = {
    "clock": [[0], [9.99], [136.4], [3599.9], [3600], [3725], [-1], [59.999], [60.0], [0.5],
              [86399.5], [86400], [360000.0], [-0.5], [1e9]],
    "_when": [[STARTED], ["2026-10-20T00:00:00"], ["2026-10-20T12:00:59"],
              ["2026-10-20T09:05:00"], ["2026-01-01T23:59:00"], ["2027-02-28T10:10:10"],
              ["2026-11-15T01:01:01"], ["2026-03-08T02:30:00"], ["2026-11-01T01:30:00"],
              ["2026-05-17T13:00:00"], ["2026-06-03T11:59:00"], ["2026-07-25T22:45:30"],
              ["2026-08-09T15:15:15"], ["2026-09-12T06:06:06"], ["2026-04-14T17:30:00"]],
    "_escape_markers": [
        ["# one thing"], ["#hashtag"], ["> quoted"], ["- dash"], ["+ plus"], ["* star"],
        ["1. First"], ["12) Twelfth"], ["1.5 seconds in"],
        ["Okay # not a heading - not a list"],
        ["Line one\n- line two\n  3. line three"],
        ["\t> tabbed quote"], ["123456789. nine digits"], ["1234567890. ten digits"],
        ["7."], ["7)"], ["7.x"], ["--- rule"], ["***"], ["١. Arabic-Indic digit"],
        ["a\n#b\n\n> c\n"], ["   - indented\n\t\t* tabs"], ["2. no-break space"],
        ["1)\ttab after"], ["- "], [""],
    ],
    "_paragraphs": [
        ["one"], ["one\n\ntwo"], ["one\n \t\ntwo\n\n\n\nthree"], ["\n\n"], [" a \n b \n\n c "],
        ["x\n \ny"], [""],
    ],
    "_tidy": [
        ["  plain  "], ["a\r\nb\rc\nd"], ["\r\n\r\n"], ["　wide space　"], [""],
        ["x\r\r\ny"],
    ],
    "_image_target": [
        ["capture/shot-1.png", "capture", False],
        ["capture/shot-1.png", "capture", True],
        ["capture/shots/shot 1.png", "capture", False],
        ["capture/shot(1).png", "capture", False],
        ["capture/shot\t1.png", "capture", False],
        ["elsewhere/shot-1.png", "capture", False],
        ["capture/../capture/shot-1.png", "capture", False],
        ["capture/sub/../shot-2.png", "capture/", False],
        ["CAPTURE/shot-1.png", "capture", False],
        ["capture2/shot-1.png", "capture", False],
        ["Big D/captures/2026-10-20_14-32-05/shot-1.png", "Big D/captures/2026-10-20_14-32-05",
         True],
        ["capture/naïve shot.png", "capture", False],
        ["capture/naïve.png", "capture", False],
    ],
    "markdown": [
        [_three_sections(), False, None],
        [_three_sections(), True, None],
        [_three_sections(), False, ["", False, False, False]],
        [_three_sections(), False, ["Bug report", True, False, True]],
        [_three_sections(), False, ["Bug report", False, True, True]],
        [_three_sections(), False, ["Notes", False, False, False]],
        [ODD, False, None],
        [ODD, True, ["Spaced  Title", True, True, False]],
        [SHOTLESS, False, None],
        [EMPTY, False, None],
        [EMPTY, False, ["", False, False, True]],
    ],
    "write_manifest": [
        [_three_sections()],
        [ODD],
        [SHOTLESS],
        [EMPTY],
        [{"started_at": "2027-01-15T08:00:00", "duration_s": 12.5, "sections": [
            ["  Okay, look here. ", "capture/shot-1.png", 41.3, None, []],
            ["", None, None, None, []],
            ["That's it.", None, None, None, []],
        ]}],
        [{"started_at": "2026-11-01T01:30:00", "duration_s": 1e-05, "sections": [
            ["", "capture/shot-1.png", 0.1, "capture/shot-2.png", ["a note"]],
            ["", None, None, None, ["only a note"]],
        ]}],
    ],
}
