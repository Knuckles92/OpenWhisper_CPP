"""Curated inputs for services/walkthrough/storage.py (fv_walkthrough's storage.h).

The module's captures_root() is pointed at a scratch folder under out/parity for each call.

    new_capture_folder(started_iso, existing, count)
    new_screenshot_path(started_iso, existing, count, save_each)

`existing` lists [relative path, "dir" | "file"] made under the captures root first; the
function is then called `count` times and the names it gives come back in order (a screenshot
path is written to when save_each is true, as a caller saving the image would).
"""
from __future__ import annotations

import shutil
import tempfile
from datetime import datetime
from pathlib import Path

REPO = Path(__file__).resolve().parents[3]
SCRATCH = REPO / "out" / "parity" / "walkthrough_storage"


def _with_root(module, existing, call):
    SCRATCH.mkdir(parents=True, exist_ok=True)
    root = Path(tempfile.mkdtemp(dir=SCRATCH)) / "captures"
    saved = module.captures_root
    module.captures_root = lambda: root
    try:
        for relative, kind in existing:
            path = root / relative
            path.parent.mkdir(parents=True, exist_ok=True)
            if kind == "dir":
                path.mkdir()
            else:
                path.write_text("someone else's", encoding="utf-8")
        return call()
    finally:
        module.captures_root = saved
        shutil.rmtree(root.parent)


def _new_capture_folder(module, started, existing, count):
    when = datetime.fromisoformat(started)
    return _with_root(module, existing,
                      lambda: [module.new_capture_folder(when).name for _ in range(count)])


def _new_screenshot_path(module, started, existing, count, save_each):
    when = datetime.fromisoformat(started)

    def call():
        names = []
        for _ in range(count):
            path = module.new_screenshot_path(when)
            if save_each:
                path.write_bytes(b"png")
            names.append(path.name)
        return names

    return _with_root(module, existing, call)


WRAPPERS = {
    "new_capture_folder": _new_capture_folder,
    "new_screenshot_path": _new_screenshot_path,
}

STARTED = "2026-10-21T14:32:05"

CASES = {
    "new_capture_folder": [
        [STARTED, [], 1],
        [STARTED, [], 3],
        [STARTED, [["2026-10-21_14-32-05", "file"]], 1],
        [STARTED, [["2026-10-21_14-32-05", "dir"], ["2026-10-21_14-32-05-2", "file"],
                   ["2026-10-21_14-32-05-4", "dir"]], 3],
        [STARTED, [["2026-10-21_14-32-05-2", "dir"]], 2],
        ["2027-01-02T03:04:05", [], 2],
    ],
    "new_screenshot_path": [
        [STARTED, [], 3, True],
        [STARTED, [], 2, False],
        [STARTED, [["screenshots/2026-10-21_14-32-05.png", "file"],
                   ["screenshots/2026-10-21_14-32-05-2.png", "dir"]], 2, True],
        [STARTED, [["screenshots/2026-10-21_14-32-05-2.png", "file"]], 3, True],
        ["2027-01-02T03:04:05", [], 1, False],
    ],
}
