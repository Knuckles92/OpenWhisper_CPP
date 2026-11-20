"""Curated inputs for services/walkthrough/library.py (fv_walkthrough's library.h).

Folders are made in a scratch root under out/parity and removed afterwards; names come back
without the root.

    write_up_parts(text), join_write_up(parts)
    _write_up_shot(files, line)              the shot's file name, or None; `files` are made empty
    _earlier_write_ups(files)                `files` made empty; a name ending in "/" is a folder
    _load_document(manifest, files)          walkthrough.json holds `manifest` (text), `files` are
                                             made empty; the summary, or {"error": [type, str]}
    _retention_days(text)                    captures/retention.json holds `text` (None: absent)
    _expired_entries(retention, folders)     retention.json holds `retention` (JSON); each folder
                                             is [name, manifest age in seconds or None, draft];
                                             expired_entries(now=NOW), the names

A load_document summary is {"sections": [[text, shot, kept_at_s, omitted, notes]],
"duration_s", "started_utc"}: shot and omitted are file names, started_utc the start as UTC
isoformat() when it has an offset, else None.
"""
from __future__ import annotations

import json
import os
import shutil
import tempfile
from datetime import timezone
from pathlib import Path

REPO = Path(__file__).resolve().parents[3]
SCRATCH = REPO / "out" / "parity" / "walkthrough_library"
#: The pinned "now" of expired_entries, seconds since the epoch.
NOW = 1_800_000_000.0


def _root() -> Path:
    SCRATCH.mkdir(parents=True, exist_ok=True)
    return Path(tempfile.mkdtemp(dir=SCRATCH))


def _make(folder: Path, files) -> None:
    folder.mkdir(parents=True, exist_ok=True)
    for name in files:
        if name.endswith("/"):
            (folder / name).mkdir()
        else:
            (folder / name).write_bytes(b"")


def _write_up_shot(module, files, line):
    root = _root()
    try:
        _make(root, files)
        found = module.write_up_shot(root, line)
        return None if found is None else found.name
    finally:
        shutil.rmtree(root)


def _earlier_write_ups(module, files):
    root = _root()
    try:
        _make(root, files)
        return module.earlier_write_ups(root)
    finally:
        shutil.rmtree(root)


def _load_document(module, manifest, files):
    root = _root()
    try:
        _make(root, files)
        (root / "walkthrough.json").write_text(manifest, encoding="utf-8")
        try:
            walkthrough = module.load_document(root)
        except Exception as exc:  # the type and message are the vector
            return {"error": [type(exc).__name__, str(exc)]}
        started = walkthrough.started_at
        return {
            "sections": [[s.text, s.shot.name if s.shot else None, s.kept_at_s,
                          s.omitted.name if s.omitted else None, list(s.notes)]
                         for s in walkthrough.sections],
            "duration_s": walkthrough.duration_s,
            "started_utc": (started.astimezone(timezone.utc).isoformat()
                            if started.tzinfo is not None else None),
        }
    finally:
        shutil.rmtree(root)


def _retention_days(module, text):
    root = _root()
    try:
        if text is not None:
            (root / "retention.json").write_text(text, encoding="utf-8")
        return module.retention_days(root)
    finally:
        shutil.rmtree(root)


def _expired_entries(module, retention, folders):
    root = _root()
    try:
        (root / "retention.json").write_text(json.dumps({"days": retention}), encoding="utf-8")
        for name, age, draft in folders:
            folder = root / name
            folder.mkdir()
            if age is not None:
                manifest = folder / "walkthrough.json"
                manifest.write_text("{}", encoding="utf-8")
                os.utime(manifest, (NOW - age, NOW - age))
            if draft:
                (folder / "draft.json").write_text("{}", encoding="utf-8")
        return [folder.name for folder in module.expired_entries(root, now=NOW)]
    finally:
        shutil.rmtree(root)


WRAPPERS = {
    "_write_up_shot": _write_up_shot,
    "_earlier_write_ups": _earlier_write_ups,
    "_load_document": _load_document,
    "_retention_days": _retention_days,
    "_expired_entries": _expired_entries,
}

WRITE_UP = (
    "# Two slow spots\n\n"
    "1. Billing stalls.\n\n"
    "   ![Billing](shot-1.png)\n\n"
    "2. Invoices hang.\n\n"
    "![Invoices](shot-2.png)\n\n"
    "Both happen after sign-in."
)

STARTED = "2026-10-21T14:32:05-07:00"


def _manifest(sections, *, version=1, started=STARTED, duration=3.0, **extra):
    value = {"version": version, "started_at": started, "duration_s": duration,
             "sections": sections}
    value.update(extra)
    return json.dumps(value)


def _section(text="words", shot="shot-1.png", kept=1.0, **extra):
    value = {"text": text, "shot": shot, "kept_at_s": kept}
    value.update(extra)
    return value


SHOTS = ["shot-1.png", "shot-2.png", "shot-10.png"]

CASES = {
    "write_up_parts": [
        [WRITE_UP],
        [""],
        ["Just words.\n\nNo pictures."],
        ["![only](shot-1.png)"],
        ["![a](shot-1.png)\n![b](shot-2.png)\n"],
        ["Before\r\n![a](shot-1.png)\r\nAfter"],
        ["Words\n\n\n![a](<shot 1.png> \"Title\")\n\n\n"],
        ["Text with ![inline](shot-1.png) image\n\nMore"],
        ["\n\n  \n![a](shot-1.png)\n  \n"],
        ["One\u2028Two\n![a](shot-1.png)\x0cThree"],
        ["- item\n  ![a](shot-1.png)\n- next"],
    ],
    "join_write_up": [
        [[["# Two slow spots\n\n1. Billing stalls.", "   ![Billing](shot-1.png)"],
          ["2. Invoices hang.", "![Invoices](shot-2.png)"],
          ["Both happen after sign-in.", ""]]],
        [[["2. Invoices hang.", "![Invoices](shot-2.png)"],
          ["# Two slow spots\n\n1. Billing stalls.", "   ![Billing](shot-1.png)"],
          ["Both happen after sign-in.", ""]]],
        [[]],
        [[["", ""]]],
        [[["  \n", "![a](shot-1.png)"], ["\n\nwords\n\n", ""]]],
        [[["", "![a](shot-1.png)"], ["", "![b](shot-2.png)"]]],
    ],
    "_write_up_shot": [
        [SHOTS, "![x](shot-2.png)"],
        [SHOTS, "  ![x](shot-2.png)  "],
        [SHOTS, "![x](<shot-10.png>)"],
        [SHOTS, "![x](shot-2.png \"Two\")"],
        [SHOTS, "![x](elsewhere.png)"],
        [SHOTS, "![x](shot-9.png)"],
        [SHOTS, "![x](./shot-1.png)"],
        [SHOTS, "![x](shot-01.png)"],
        [SHOTS, "words ![x](shot-1.png)"],
        [SHOTS, "![x](shot-1.png) words"],
        [SHOTS, ""],
        [["shot-1.png/"], "![x](shot-1.png)"],
    ],
    "_earlier_write_ups": [
        [[]],
        [["write-up-1.md", "write-up-1.pdf"]],
        [["write-up.md", "write-up.pdf"]],
        [["write-up.md", "write-up.pdf", "write-up-1.md", "write-up-1.pdf", "write-up-3.md",
          "write-up-3.pdf", "write-up-2.md"]],
        [["write-up.md", "write-up.pdf", "write-up-10.md", "write-up-10.pdf", "write-up-9.md",
          "write-up-9.pdf"]],
        [["write-up.md", "write-up.pdf", "write-up-0.md", "write-up-0.pdf", "write-up-01.md",
          "write-up-01.pdf", "write-up-2.docx", "write-up-2.pdf"]],
        [["write-up.md", "write-up.pdf", "write-up-4.md/", "write-up-4.pdf"]],
        [["write-up.md", "write-up-1.md", "write-up-1.pdf"]],
        [["write-up.pdf/", "write-up.md", "write-up-1.md", "write-up-1.pdf"]],
    ],
    "_load_document": [
        [_manifest([_section("first", "shot-1.png", 1.0), _section("last", None, None)]),
         ["shot-1.png"]],
        [_manifest([_section("a", "shot-2.png", 2, notes=["n1", "n2"]),
                    _section("", None, 2.5, omitted_shot="shot-1.png")]),
         ["shot-1.png", "shot-2.png"]],
        [_manifest([_section("", None, None, omitted_shot="shot-3.png")]), []],
        [_manifest([_section("both", "shot-1.png", 1.0, omitted_shot="shot-2.png")]),
         ["shot-1.png", "shot-2.png"]],
        [_manifest([_section(7, None, None)]), []],
        [_manifest([_section(None, None, None)]), []],
        [_manifest([_section(True, None, None)]), []],
        [_manifest([_section(1.5, None, None)]), []],
        [_manifest([], started="2026-10-21T14:32:05"), []],
        [_manifest([], started="2026-10-21T21:32:05Z", duration="12.5"), []],
        [_manifest([], started="2026-10-21T14:32:05.250000+05:30", duration=True), []],
        [_manifest([], version=2), []],
        [_manifest([], version=True), []],
        [_manifest([], version=1.0), []],
        [_manifest([], version="1"), []],
        [json.dumps({"started_at": STARTED, "duration_s": 1.0, "sections": []}), []],
        [_manifest([_section("x", "../outside.png")]), []],
        [_manifest([_section("x", "C:\\outside.png")]), []],
        [_manifest([_section("x", "walkthrough.pdf")]), ["walkthrough.pdf"]],
        [_manifest([_section("x", "shot-0.png")]), ["shot-0.png"]],
        [_manifest([_section("x", 5)]), []],
        [_manifest([_section("x", "")]), []],
        [_manifest([_section("x", "shot-1.png")]), []],
        [_manifest([_section("x", "shot-1.png")]), ["shot-1.png/"]],
        [_manifest([_section("x", None, None, omitted_shot="notes.txt")]), ["notes.txt"]],
        [_manifest([_section("x", None, None, notes="purple")]), []],
        [_manifest([_section("x", None, None, notes=["a", 7])]), []],
        [_manifest([_section("x", None, None, notes=None)]), []],
        [_manifest([_section("x", None, -1)]), []],
        [_manifest([_section("x", None, "NaN")]), []],
        [_manifest([_section("x", None, "soon")]), []],
        [_manifest([_section("x", None, [1])]), []],
        [_manifest([], duration=-0.5), []],
        [_manifest([], duration=None), []],
        [_manifest([], started="yesterday"), []],
        [_manifest([], started=20261021), []],
        [_manifest([{"shot": None, "kept_at_s": None}]), []],
        [_manifest(["not a section"]), []],
        [json.dumps({"version": 1, "started_at": STARTED, "duration_s": 1.0}), []],
        [json.dumps([1, 2]), []],
        ["{not json", []],
    ],
    "_retention_days": [
        [None], ['{"days": 30}'], ['{"days": 0}'], ['{"days": 3650}'], ['{"days": 3651}'],
        ['{"days": -1}'], ['{"days": true}'], ['{"days": "30"}'], ['{"days": 30.0}'],
        ['{"weeks": 3}'], ["[30]"], ["not json"], ['{"days": null}'],
        ['{"days": 18446744073709551616}'],
    ],
    "_expired_entries": [
        [30, [["2026-10-21_14-32-05", 100 * 86400, False]]],
        [30, [["2026-10-21_14-32-05", 29 * 86400, False]]],
        [30, [["2026-10-21_14-32-05", 30 * 86400, False]]],
        [30, [["2026-10-21_14-32-05", 30 * 86400 + 1, False]]],
        [0, [["2026-10-21_14-32-05", 100 * 86400, False]]],
        [30, [["2026-10-21_14-32-05", 100 * 86400, True]]],
        [30, [["2026-10-21_14-32-05", None, True]]],
        [30, [["2026-10-21_14-32-05", None, False]]],
        [1, [["2026-10-21_14-32-05", 2 * 86400, False], ["2026-10-22_08-00-00", 3 * 86400, False],
             ["2026-10-22_08-00-00-2", 0, False], ["notes", 9 * 86400, False],
             ["2026-10-23_08-00-00", 5 * 86400, True]]],
        [True, [["2026-10-21_14-32-05", 100 * 86400, False]]],
        [3651, [["2026-10-21_14-32-05", 100 * 86400, False]]],
    ],
}
