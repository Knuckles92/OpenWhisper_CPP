"""services/history_manager.py and the history search of services/database.py (M1.13).

Entries are given as dicts of TranscriptionHistory columns (missing ones are None).

    kind_of, entry_version, ai_text, is_local_entry   over such dicts (stored_on included)
    _describe_retention(max_recordings, max_bytes)
    recordings_over_limit(recordings, max_recordings, max_bytes)
        recordings: [filename, size_bytes] newest first; the result is the filenames deleted
    use_version(entry, version)
        a fresh database (out/parity/history-cases) holding the entry; the result is
        {"returned": [text, raw_text, cleaned_text] or None, "stored": [...], "edited": bool}
    search_history_entries(rows, query, limit)
        a fresh database holding `rows`; the result is the ids found, in order
"""

import itertools
import sys
from pathlib import Path
from types import SimpleNamespace

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import ow_isolation  # noqa: E402

WORK = ow_isolation.SCRATCH / "history-cases"
_COLUMNS = (
    "id", "text", "raw_text", "timestamp", "model", "audio_file", "transcription_time",
    "audio_duration", "file_size", "cleanup_provider", "cleanup_model", "source_name", "title",
    "origin_device_id", "origin_device_name", "app_id", "app_name", "app_category",
    "cleanup_level", "entry_kind", "cleaned_text", "language",
)
_counter = itertools.count()


def _entry(fields):
    values = {name: fields.get(name) for name in _COLUMNS}
    entry = SimpleNamespace(**values)
    if "stored_on" in fields:
        entry.stored_on = fields["stored_on"]
    return entry


def _fresh_database():
    from services.database import DatabaseManager

    WORK.mkdir(parents=True, exist_ok=True)
    path = WORK / f"case-{next(_counter)}.db"
    for suffix in ("", "-wal", "-shm", "-journal"):
        Path(str(path) + suffix).unlink(missing_ok=True)
    return DatabaseManager(db_path=str(path))


def _stored(entry):
    return None if entry is None else [entry.text, entry.raw_text, entry.cleaned_text]


def _use_version(module, fields, version):
    from services.models import TranscriptionHistory

    database = _fresh_database()
    edited = []
    sync = SimpleNamespace(record_edited=lambda kind, record_id: edited.append(record_id))
    saved_db, saved_sync = module.db, module._record_sync
    module.db, module._record_sync = database, (lambda: sync)
    try:
        row = {"timestamp": "2026-01-01T09:00:00+00:00", "model": "base"}
        row.update(fields)
        database.put_history_entry(TranscriptionHistory(**row))
        manager = module.HistoryManager(str(WORK / "recordings"), max_recordings=None)
        returned = manager.use_version(row["id"], version)
        return {"returned": _stored(returned),
                "stored": _stored(database.get_history_entry_by_id(row["id"])),
                "edited": bool(edited)}
    finally:
        module.db, module._record_sync = saved_db, saved_sync
        database.close()


def _search(module, rows, query, limit):
    from services.models import TranscriptionHistory

    database = _fresh_database()
    try:
        for row in rows:
            values = {"model": "base"}
            values.update(row)
            database.put_history_entry(TranscriptionHistory(**values))
        return [entry.id for entry in database.search_history_entries(query, limit)]
    finally:
        database.close()


def _over_limit(module, recordings, max_recordings, max_bytes):
    infos = [module.RecordingInfo(filename=name, timestamp="", file_path=name, size_bytes=size)
             for name, size in recordings]
    stub = SimpleNamespace(get_recordings=lambda: list(infos))
    expired = module.HistoryManager.recordings_over_limit(stub, max_recordings, max_bytes)
    return [rec.filename for rec in expired]


_ENTRIES = [
    {},
    {"entry_kind": "command", "source_name": "Quick Record"},
    {"entry_kind": "", "source_name": "Quick Record · Email"},
    {"source_name": "Quick Record"},
    {"source_name": "quick record"},
    {"source_name": "talk.mp3"},
    {"source_name": ""},
    {"text": "Hello.", "raw_text": "hello"},
    {"text": "hello", "raw_text": "hello"},
    {"text": "hello", "raw_text": "hello", "cleaned_text": "Hello."},
    {"text": "hello", "raw_text": "hello", "cleaned_text": "hello"},
    {"text": "Hello.", "raw_text": "", "cleaned_text": "Hello."},
    {"text": "", "raw_text": "hello", "cleaned_text": ""},
    {"origin_device_id": "device-1"},
    {"origin_device_id": ""},
    {"stored_on": "devbox"},
    {"stored_on": ""},
    {"stored_on": None},
]

_SEARCH_ROWS = [
    {"id": "entry-old", "text": "ordinary notes", "timestamp": "2026-01-01T00:00:00+00:00",
     "raw_text": "um ordinary notes", "source_name": "Quick Record", "app_name": "Slack"},
    {"id": "entry-mid", "text": "literal 100% result", "timestamp": "2026-02-01T00:00:00+00:00",
     "title": "Budget review", "origin_device_name": "Laptop"},
    {"id": "entry-new", "text": "another 100% result", "timestamp": "2026-03-01T00:00:00+00:00",
     "app_name": "Outlook"},
    {"id": "entry-slash", "text": "path a\\b and 100x", "timestamp": "2026-03-02T00:00:00",
     "source_name": "talk_final.mp3"},
    {"id": "entry-accent", "text": "Café Éclair", "timestamp": "2026-03-03T00:00:00+00:00"},
]

CASES = {
    "kind_of": [[fields] for fields in _ENTRIES],
    "entry_version": [[fields] for fields in _ENTRIES],
    "ai_text": [[fields] for fields in _ENTRIES],
    "is_local_entry": [[fields] for fields in _ENTRIES],
    "_describe_retention": [[None, None], [20, None], [None, 1024 ** 3], [5, 250 * 1024 ** 2],
                            [1, 1023], [None, 1536]],
    "recordings_over_limit": [
        [[["c", 100], ["b", 100], ["a", 100]], None, None],
        [[["c", 100], ["b", 100], ["a", 100]], 2, None],
        [[["c", 100], ["b", 100], ["a", 100]], 0, None],
        [[["c", 100], ["b", 100], ["a", 100]], 5, None],
        [[["d", 100], ["c", 100], ["b", 100], ["a", 100]], None, 250],
        [[["c", 100], ["b", 100], ["a", 100]], None, 300],
        [[["b", 200], ["a", 10]], None, 50],
        [[["e", 100], ["d", 100], ["c", 100], ["b", 100], ["a", 100]], 3, 1000],
        [[["e", 100], ["d", 100], ["c", 100], ["b", 100], ["a", 100]], 4, 150],
        [[["c", 0], ["b", 0], ["a", 500]], None, 0],
        [[], 3, 10],
        [[["c", 100], ["b", 100], ["a", 100]], -1, None],
    ],
    "use_version": [
        [{"id": "e", "text": "Hello, world.", "raw_text": "hello world um",
          "cleaned_text": "Hello, world."}, "original"],
        [{"id": "e", "text": "hello world um", "raw_text": "hello world um",
          "cleaned_text": "Hello, world."}, "ai"],
        [{"id": "e", "text": "Hello, world.", "raw_text": "hello world um",
          "cleaned_text": "Hello, world."}, "ai"],
        [{"id": "e", "text": "Cleaned long ago.", "raw_text": "cleaned long ago"}, "original"],
        [{"id": "e", "text": "as heard", "raw_text": None}, "original"],
        [{"id": "e", "text": "Theirs.", "raw_text": "theirs", "origin_device_id": "d"},
         "original"],
        [{"id": "e", "text": "plain"}, "raw"],
        [{"id": "e", "text": "Hello.", "raw_text": "hello", "cleaned_text": "Hello."}, "AI"],
    ],
    "search_history_entries": [
        [_SEARCH_ROWS, "100%", 1],
        [_SEARCH_ROWS, "100%", 10],
        [_SEARCH_ROWS, "100_", 10],
        [_SEARCH_ROWS, "100", None],
        [_SEARCH_ROWS, "  LITERAL  ", None],
        [_SEARCH_ROWS, "a\\b", None],
        [_SEARCH_ROWS, "\\", None],
        [_SEARCH_ROWS, "_final", None],
        [_SEARCH_ROWS, "2026-02", None],
        [_SEARCH_ROWS, "+00:00", 2],
        [_SEARCH_ROWS, "budget", None],
        [_SEARCH_ROWS, "laptop", None],
        [_SEARCH_ROWS, "slack", None],
        [_SEARCH_ROWS, "UM ORD", None],
        [_SEARCH_ROWS, "quick record", None],
        [_SEARCH_ROWS, "café", None],
        [_SEARCH_ROWS, "CAFÉ", None],
        [_SEARCH_ROWS, "éclair", None],
        [_SEARCH_ROWS, "   ", 3],
        [_SEARCH_ROWS, "", 0],
        [_SEARCH_ROWS, "%", None],
    ],
}

WRAPPERS = {
    "kind_of": lambda module, fields: module.kind_of(_entry(fields)),
    "entry_version": lambda module, fields: module.entry_version(_entry(fields)),
    "ai_text": lambda module, fields: module.ai_text(_entry(fields)),
    "is_local_entry": lambda module, fields: module.is_local_entry(_entry(fields)),
    "_describe_retention": lambda module, count, size: module._describe_retention(count, size),
    "recordings_over_limit": _over_limit,
    "use_version": _use_version,
    "search_history_entries": _search,
}
