"""services/backup_manager.py's schedule rules (M3.4 backup-restore).

    next_backup_at(preferences, now, retry_after)   BackupCoordinator.snapshot()["next_backup_at"]
                                                     on a stub coordinator holding `preferences`
    _load_preferences(text, now)                     the preferences read from a
                                                     backup-preferences.json holding `text` as
                                                     UTF-8 (None: no file)
    _load_preferences_bytes(hex, now)                the same for raw bytes
    _prune_automatic_backups(scheduled, files, keep) {"removed": [files deleted], "scheduled_archives":
                                                     [what is left]}: files are made below a
                                                     scratch root R ("x/" makes a folder), the
                                                     destination is R/backups, each scheduled
                                                     entry is str(R / entry), or R + "/" + entry
                                                     for an entry starting with "@"; paths come
                                                     back with R as "<root>"
    _auto_name(name)                                 _AUTO_NAME.fullmatch(name) is not None
"""

import shutil
import tempfile
from pathlib import Path
from types import SimpleNamespace

REPO = Path(__file__).resolve().parents[3]
SCRATCH = REPO / "out" / "parity" / "backup_manager"


def _scratch() -> Path:
    SCRATCH.mkdir(parents=True, exist_ok=True)
    return Path(tempfile.mkdtemp(dir=SCRATCH)).resolve()


def _next_backup_at(module, preferences, now, retry_after):
    stub = SimpleNamespace(_service=None, data_dir=Path("."), _preferences=dict(preferences),
                           _clock=lambda: now, _retry_after=retry_after, _busy=False,
                           _operation="", _last_error="")
    return module.BackupCoordinator.snapshot(stub)["next_backup_at"]


def _load_from_bytes(module, data, now):
    folder = _scratch()
    try:
        if data is not None:
            (folder / module.PREFERENCES_FILE).write_bytes(data)
        stub = SimpleNamespace(data_dir=folder, _clock=lambda: now)
        return module.BackupCoordinator._load_preferences(stub)
    finally:
        shutil.rmtree(folder)


def _load_preferences(module, text, now):
    return _load_from_bytes(module, None if text is None else text.encode("utf-8"), now)


def _load_preferences_bytes(module, hex_bytes, now):
    return _load_from_bytes(module, bytes.fromhex(hex_bytes), now)


def _prune_automatic_backups(module, scheduled, files, keep):
    root = _scratch()
    try:
        (root / "backups").mkdir()
        made = []
        for rel in files:
            path = root / rel
            if rel.endswith("/"):
                path.mkdir(parents=True, exist_ok=True)
                continue
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(b"x")
            made.append(rel)
        entries = [str(root) + "/" + entry[1:] if entry.startswith("@") else str(root / entry)
                   for entry in scheduled]
        stub = SimpleNamespace(_preferences={"destination": str(root / "backups"),
                                             "scheduled_archives": list(entries),
                                             "retention_count": keep},
                               _save_preferences=lambda: None)
        module.BackupCoordinator._prune_automatic_backups(stub)
        removed = sorted(rel for rel in made if not (root / rel).exists())
        left = [entry.replace(str(root), "<root>")
                for entry in stub._preferences["scheduled_archives"]]
        return {"removed": removed, "scheduled_archives": left}
    finally:
        shutil.rmtree(root)


def _auto_name(module, name):
    return module._AUTO_NAME.fullmatch(name) is not None


WRAPPERS = {
    "next_backup_at": _next_backup_at,
    "_load_preferences": _load_preferences,
    "_load_preferences_bytes": _load_preferences_bytes,
    "_prune_automatic_backups": _prune_automatic_backups,
    "_auto_name": _auto_name,
}


def _prefs(frequency="off", last_auto_at=0, retention_count=5):
    return {"frequency": frequency, "retention_count": retention_count, "destination": "",
            "last_backup": None, "last_auto_at": last_auto_at, "scheduled_archives": []}


A1 = "OpenWhisper-auto-20261001T000000Z-aaaaaaaaaaaa.owbackup"
A2 = "OpenWhisper-auto-20261002T000000Z-bbbbbbbbbbbb.owbackup"
A3 = "OpenWhisper-auto-20261003T000000Z-cccccccccccc.owbackup"
A4 = "OpenWhisper-auto-20261004T000000Z-dddddddddddd.owbackup"

CASES = {
    "next_backup_at": [
        [_prefs(), 86401, 0],
        [_prefs("daily"), 86401, 0],
        [_prefs("daily", 86401), 86401, 0],
        [_prefs("daily", 86401.5), 90000.25, 0],
        [_prefs("weekly", 86401), 86401, 0],
        [_prefs("weekly", 86401), 86401 + 8 * 86400, 0],
        [_prefs("daily", 86401), 2 * 86400 + 86401, 2 * 86400 + 86401 + 600],
        [_prefs("daily", 0), 86401, 100],
        [_prefs("daily", 0.0), 5.0, 0],
        [_prefs("daily", True), 5, 0],
        [_prefs("daily", False), 5, 0],
        [_prefs("monthly", 86401), 86401, 0],
        [_prefs("daily", 1_700_000_000), 1_700_000_100.5, 1_700_000_050],
    ],
    "_load_preferences": [
        [None, 86401],
        ["{}", 86401],
        ["[]", 86401],
        ["not json", 86401],
        ["", 86401],
        ["\ufeff{}", 86401],
        ['{"frequency": "daily", "retention_count": 3, "destination": "D:\\\\Backups",'
         ' "last_backup": {"path": "D:\\\\Backups\\\\x.owbackup", "file_count": 2},'
         ' "last_auto_at": 86000.5, "scheduled_archives": ["a", 1, null, "b"], "extra": 1}',
         86401],
        ['{"frequency": "Daily", "retention_count": 0}', 86401],
        ['{"frequency": "weekly", "retention_count": 100}', 86401],
        ['{"frequency": ["daily"], "retention_count": 101}', 86401],
        ['{"retention_count": true}', 86401],
        ['{"retention_count": 5.0}', 86401],
        ['{"retention_count": -3}', 86401],
        ['{"destination": 5}', 86401],
        ['{"last_backup": {"path": 5}}', 86401],
        ['{"last_backup": {"created_at": "x"}}', 86401],
        ['{"last_backup": ["path"]}', 86401],
        ['{"last_auto_at": -1}', 86401],
        ['{"last_auto_at": 172801}', 86401],
        ['{"last_auto_at": 172802}', 86401],
        ['{"last_auto_at": true}', 86401],
        ['{"last_auto_at": "86400"}', 86401],
        ['{"last_auto_at": null}', 86401],
        ['{"last_auto_at": NaN}', 86401],
        ['{"last_auto_at": Infinity}', 86401],
        ['{"scheduled_archives": "a"}', 86401],
        ['{"scheduled_archives": [' + ",".join(f'"p{i}"' for i in range(1003)) + "]}", 86401],
        ['{"frequency": "daily", "frequency": "weekly"}', 86401],
        ['{\n  "frequency": "daily",\r\n  "retention_count": 7\r\n}', 86401],
        ['{"destination": "C:\\\\Users\\\\dana\\\\B\u00e4ckups"}', 86401],
    ],
    "_load_preferences_bytes": [
        ["7b2266726571756572637922", 86401],
        ["7b2264657374696e6174696f6e223a2022ff227d", 86401],
        ["fffe7b007d00", 86401],
    ],
    "_prune_automatic_backups": [
        [[f"backups/{A1}", f"backups/{A2}", f"backups/{A3}"],
         [f"backups/{A1}", f"backups/{A2}", f"backups/{A3}"], 2],
        [[f"backups/{A1}", f"backups/{A2}", f"backups/{A3}"],
         [f"backups/{A1}", f"backups/{A2}", f"backups/{A3}"], 1],
        [[f"backups/{A1}", f"backups/{A2}"], [f"backups/{A1}", f"backups/{A2}"], 5],
        [[f"backups/{A1}", "backups/my-manual.owbackup", f"backups/{A2}", f"backups/{A3}"],
         [f"backups/{A1}", "backups/my-manual.owbackup", f"backups/{A2}", f"backups/{A3}",
          f"backups/{A4}", "backups/notes.txt"], 1],
        [[f"backups/sub/{A1}", f"backups/{A2}", f"backups/{A3}"],
         [f"backups/sub/{A1}", f"backups/{A2}", f"backups/{A3}"], 1],
        [[f"other/{A1}", f"backups/{A2}"], [f"other/{A1}", f"backups/{A2}"], 1],
        [[f"backups/{A1}", f"backups/{A2}", f"backups/{A3}"], [f"backups/{A3}"], 1],
        [[f"backups/{A1}", f"backups/{A2}", f"backups/{A3}"],
         [f"backups/{A1}/", f"backups/{A2}", f"backups/{A3}"], 1],
        [[f"@backups/{A1}", f"backups/{A2}", f"backups/{A3}"],
         [f"backups/{A1}", f"backups/{A2}", f"backups/{A3}"], 1],
        [[f"backups/../backups/{A1}", f"backups/{A2}"],
         [f"backups/{A1}", f"backups/{A2}"], 1],
        [[f"backups/{A1.lower()}", f"backups/{A2}", f"backups/{A3}"],
         [f"backups/{A1.lower()}", f"backups/{A2}", f"backups/{A3}"], 1],
        [[f"backups/{A1}", f"backups/{A1}", f"backups/{A2}"],
         [f"backups/{A1}", f"backups/{A2}"], 1],
        [[f"backups/{A1}x", f"backups/{A2}.bak", f"backups/{A3}"],
         [f"backups/{A1}x", f"backups/{A2}.bak", f"backups/{A3}"], 1],
        [[], [f"backups/{A1}"], 1],
    ],
    "_auto_name": [[name] for name in [
        A1, A2,
        "OpenWhisper-auto-20261001T000000Z-aaaaaaaaaaa.owbackup",
        "OpenWhisper-auto-20261001T000000Z-aaaaaaaaaaaaa.owbackup",
        "OpenWhisper-auto-20261001T000000Z-AAAAAAAAAAAA.owbackup",
        "OpenWhisper-auto-2026100T000000Z-aaaaaaaaaaaa.owbackup",
        "OpenWhisper-auto-20261001T000000-aaaaaaaaaaaa.owbackup",
        "OpenWhisper-auto-20261001T000000Z-aaaaaaaaaaaa.owbackup\n",
        "OpenWhisper-auto-20261001T000000Z-aaaaaaaaaaaa.OWBACKUP",
        "openwhisper-auto-20261001T000000Z-aaaaaaaaaaaa.owbackup",
        "OpenWhisper-auto-20261001T000000Z-aaaaaaaaaaaa.owbackup.tmp",
        "xOpenWhisper-auto-20261001T000000Z-aaaaaaaaaaaa.owbackup",
        "OpenWhisper-auto-\u0662\u0660\u0662\u0666\u0661\u0660\u0660\u0661T000000Z-aaaaaaaaaaaa.owbackup",
        "OpenWhisper-auto-20261001T000000Z-aaaaaaaaaaaaxowbackup",
        "",
    ]],
}

PINS = {"note": "Windows paths: the prune cases make files under a resolved scratch root and "
                "compare paths case-insensitively, as on the owner's machine; the replay runs "
                "them on Windows only."}
