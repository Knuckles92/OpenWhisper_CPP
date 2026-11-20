"""Curated inputs for services/guided_setup.py (M2.2 appcore/guided_setup_state.h).

Each case builds a folder fixture under out/parity/guided-setup/ (never a shared temp folder):
`files` lists paths relative to the folder, a trailing "/" making an empty folder, anything else
an empty file. initialize runs on a SettingsManager for settings.json in that folder, with the
file absent (None) or holding the given text, and records the state it stored, whether it
invites, and the file afterwards (parsed, or its raw text when it is not JSON). The C++ replay
builds the same folders, naming the database identity().database_file where Python names
openwhisper.db.
"""
from __future__ import annotations

import itertools
import json
import shutil
from pathlib import Path

_SCRATCH = Path(__file__).resolve().parents[3] / "out" / "parity" / "guided-setup"
_counter = itertools.count()


def _folder(files):
    root = _SCRATCH / f"case-{next(_counter)}"
    if root.exists():
        shutil.rmtree(root)
    root.mkdir(parents=True)
    for name in files:
        path = root / name
        if name.endswith("/"):
            path.mkdir(parents=True, exist_ok=True)
        else:
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text("", encoding="utf-8")
    return root


def _has_prior_data(module, files):
    return module._has_prior_data(_folder(files))


def _initialize(module, settings_text, files):
    from services.settings import SettingsManager

    root = _folder(files)
    path = root / "settings.json"
    if settings_text is not None:
        path.write_text(settings_text, encoding="utf-8")
    module.initialize(SettingsManager(str(path)))
    reopened = SettingsManager(str(path))
    result = {"invite": module.should_invite(reopened),
              "completed": module.is_completed(reopened),
              "exists": path.exists(), "settings": None, "raw": None}
    if path.exists():
        text = path.read_text(encoding="utf-8")
        try:
            result["settings"] = json.loads(text)
        except ValueError:
            result["raw"] = text
    return result


def _save_state(module, state):
    from services.settings import SettingsManager

    path = _folder([]) / "settings.json"
    manager = SettingsManager(str(path))
    module.save_state(state, manager)
    return json.loads(path.read_text(encoding="utf-8"))


WRAPPERS = {
    "_has_prior_data": _has_prior_data,
    "initialize": _initialize,
    "save_state": _save_state,
}

_FILES = [
    [],
    ["openwhisper.db"],
    ["transcription_history.json"],
    ["scratchpad.txt"],
    ["recordings/"],
    ["recordings/old.wav"],
    ["meetings/"],
    ["meetings/m_1/"],
    ["meetings/old.json"],
    ["other.txt", "settings-backup.json", "logs/"],
    ["openwhisper.db/"],
    ["recordings/", "meetings/"],
    ["recordings"],
]

_SETTINGS = [
    None,
    "{}",
    '{"selected_model": "parakeet"}',
    '{"guided_setup_state": "completed"}',
    '{"guided_setup_state": "bogus"}',
    '{"guided_setup_state": "pending", "ui_theme": "light"}',
    "broken JSON",
]

CASES = {
    "_has_prior_data": [[files] for files in _FILES],
    "initialize": [
        [settings, files]
        for settings in _SETTINGS
        for files in ([], ["openwhisper.db"], ["recordings/"], ["recordings/old.wav"],
                      ["meetings/m_1/"], ["recordings"])
    ],
    "save_state": [[state] for state in ("pending", "started", "skipped", "completed",
                                         "existing", "", "Completed", "done")],
}

PINS = {
    "note": "files: paths relative to the fixture folder, '/' ending a folder; the C++ replay "
            "maps openwhisper.db to identity().database_file",
}
