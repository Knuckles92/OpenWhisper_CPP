"""services/history_export.py (M2.8 history-extras).

History rows are given as dicts of TranscriptionHistory attributes (missing ones are absent, as
on the SimpleNamespace rows of tests/test_history_export.py). Aware timestamps are converted
with astimezone(), the machine's zone (pins.local_timezone: Pacific, America/Los_Angeles
rules); the replay passes that zone in explicitly.

    serialize_history_entry(row)                 the export dict
    render_entry_document(row, fmt, cleaned, raw)
        over serialize_history_entry(row)
    render_export_document(rows, fmt, cleaned, raw)
        over the serialized rows; datetime.now() is pinned to 2026-03-15 14:30:05 local
    entry_file_stem(entry)                       an export dict as given (not serialized)
    filter_export_entries(entries, from, to, only_with_audio)
        entries are export dicts as given; from and to are naive isoformat() text or None;
        the result is the ids kept, in order
    write_per_entry_files(rows, fmt, existing)
        over the serialized rows into a fresh folder (out/parity/history-export) already
        holding the files named in `existing` ("keep me"); the result is
        {"written": [file names in order], "files": {name: bytes as UTF-8 text}}
    _cleanup_label(entry)                        an export dict as given
"""

import itertools
import shutil
import sys
from datetime import datetime
from pathlib import Path
from types import SimpleNamespace

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import ow_isolation  # noqa: E402

WORK = ow_isolation.SCRATCH / "history-export"
_counter = itertools.count()
_NOW = datetime(2026, 3, 15, 14, 30, 5)


class _PinnedDatetime(datetime):
    @classmethod
    def now(cls, tz=None):
        return _NOW if tz is None else _NOW.astimezone(tz)


def _row(fields):
    return SimpleNamespace(**fields)


def _serialize(module, fields):
    return module.serialize_history_entry(_row(fields))


def _render_entry(module, fields, fmt, include_cleaned, include_raw):
    return module.render_entry_document(_serialize(module, fields), fmt,
                                        include_cleaned=include_cleaned,
                                        include_raw=include_raw)


def _render_export(module, rows, fmt, include_cleaned, include_raw):
    entries = [_serialize(module, fields) for fields in rows]
    saved = module.datetime
    module.datetime = _PinnedDatetime
    try:
        return module.render_export_document(entries, fmt, include_cleaned=include_cleaned,
                                             include_raw=include_raw)
    finally:
        module.datetime = saved


def _filter(module, entries, from_text, to_text, only_with_audio):
    from_dt = None if from_text is None else datetime.fromisoformat(from_text)
    to_dt = None if to_text is None else datetime.fromisoformat(to_text)
    kept = module.filter_export_entries(entries, from_dt=from_dt, to_dt=to_dt,
                                        only_with_audio=only_with_audio)
    return [entry.get("id") for entry in kept]


def _write(module, rows, fmt, existing):
    folder = WORK / f"case-{next(_counter)}"
    shutil.rmtree(folder, ignore_errors=True)
    folder.mkdir(parents=True)
    try:
        for name in existing:
            (folder / name).write_bytes(b"keep me")
        written = module.write_per_entry_files([_serialize(module, f) for f in rows], fmt,
                                               str(folder))
        return {
            "written": [Path(path).name for path in written],
            "files": {path.name: path.read_bytes().decode("utf-8")
                      for path in sorted(folder.iterdir())},
        }
    finally:
        shutil.rmtree(folder, ignore_errors=True)


_BASE = {
    "id": "h_1", "timestamp": "2026-03-15T14:30:00+00:00",
    "model": "parakeet (tdt 0.6b v3)", "text": "Hello from history", "raw_text": None,
    "cleanup_provider": None, "cleanup_model": None, "audio_file": None,
    "transcription_time": 1.2, "audio_duration": 3.4, "file_size": 2048, "source_name": None,
}


def _with(**fields):
    row = dict(_BASE)
    row.update(fields)
    return row


_LONG = ("This is a long transcript that keeps going well past the hundred character "
         "preview limit so the preview is cut at a space")

_ROWS = [
    _BASE,
    _with(id="h_cleaned", text="Fixed hello", raw_text="um hello", cleanup_provider="openai",
          cleanup_model="gpt-4o"),
    _with(id="h_kept", text="um hi", raw_text="um hi", cleaned_text="Hi.",
          cleanup_provider="openrouter", cleanup_model="m"),
    _with(id="h_model_only", text="Fixed", raw_text="Raw  \n\n", cleanup_model="gpt-4o"),
    _with(id="h_raw_only", text="  ", raw_text="spoken words", timestamp=""),
    _with(id="h_full", text=_LONG, title="Quarterly plan", source_name="Quick Record · Email",
          origin_device_id="dev-1", origin_device_name="laptop",
          audio_file="recording_20260315_143000.wav", audio_duration=125.5,
          file_size=5 * 1024 ** 3, app_id="com.example.mail", app_name="Mail",
          app_category="email", cleanup_level="medium", entry_kind="dictation",
          language="en"),
    _with(id="h_unicode", text="Café naïve — 中文 words 😀\n", timestamp="2026-03-08T09:59:00Z",
          audio_duration=59.95, file_size=1023, model=""),
    _with(id="h_naive", timestamp="2026-11-01T01:30:00", audio_duration=None, file_size=None,
          text="", audio_file=""),
    _with(id="h_offset", timestamp="2026-08-20T16:46:00.123456+05:30", audio_duration=0.0,
          file_size=0, transcription_time=None, source_name="talk.mp3", entry_kind="file"),
    _with(id="h_bad_stamp", timestamp="not a date", title="", source_name=""),
    {"id": "h_sparse", "timestamp": "2026-01-02T03:04:05+00:00", "text": "sparse row"},
]

_TOGGLES = [(True, True), (True, False), (False, True), (False, False)]

_STEMS = [
    {"timestamp": "2026-03-15T14:30:00+00:00", "preview_text": "Hello from history"},
    {"timestamp": "2026-03-15T14:30:00Z", "preview_text": "  Hello,   world!  How's it going? "},
    {"timestamp": "2026-03-15T14:30:00", "preview_text": "Empty transcript"},
    {"timestamp": "", "preview_text": "Café naïve — 中文 words 😀"},
    {"timestamp": None, "text": "--leading and trailing--"},
    {"timestamp": "2026-03-15T14:30:00", "preview_text": "", "text": "fallback to text"},
    {"timestamp": "2026-03-15T14:30:00", "preview_text": "!!! ??? ***"},
    {"timestamp": "2026-03-15T14:30:00", "preview_text": "a" * 59 + " bcdef"},
    {"timestamp": "2026-03-15T14:30:00", "preview_text": "x" * 58 + "._-y"},
    {"timestamp": "2026-03-15T14:30:00", "preview_text": _LONG[:100].rsplit(" ", 1)[0] + "..."},
    {"timestamp": "2026-03-15T14:30:00", "preview_text": "under_score.dots-dash\ttab\nline"},
    {"timestamp": "2026-11-01T08:30:00+00:00", "preview_text": "fall back"},
    {"timestamp": "2026-11-01T09:30:00+00:00", "preview_text": "fall back"},
    {"timestamp": "0005-01-02T03:04:00", "preview_text": "old"},
    {"timestamp": "1969-12-31T23:59:59+00:00", "preview_text": "before the epoch"},
    {"timestamp": "0001-01-01T00:00:00+05:00", "preview_text": "out of range"},
    {"timestamp": "2026-03-15T14:30:00+00:00ZZ", "preview_text": "two zeds"},
    {"timestamp": 0, "preview_text": "zero"},
    {"preview_text": "Ünïcödé ÄÖÜ ß ǅ"},
]


def _entries(*rows):
    return [dict(row) for row in rows]


_FILTER_ROWS = _entries(
    {"id": "h_old", "timestamp": "2026-01-01T10:00:00+00:00", "has_audio": False},
    {"id": "h_mid", "timestamp": "2026-03-15T14:30:00+00:00", "has_audio": False},
    {"id": "h_new", "timestamp": "2026-06-01T09:00:00+00:00", "has_audio": False},
    {"id": "h_audio", "timestamp": "2026-03-20T09:00:00+00:00", "has_audio": True},
    {"id": "h_undated", "timestamp": "", "has_audio": True},
    {"id": "h_naive", "timestamp": "2026-03-31T23:59:59", "has_audio": False},
    {"id": "h_edge", "timestamp": "2026-04-01T06:59:59Z", "has_audio": True},
    {"id": "h_after", "timestamp": "2026-04-01T07:00:00+00:00", "has_audio": False},
    {"id": "h_bad", "timestamp": "yesterday", "has_audio": True},
    {"id": "h_micro", "timestamp": "2026-03-01T00:00:00.000001", "has_audio": False},
)

CASES = {
    "serialize_history_entry": [[row] for row in _ROWS],
    "render_entry_document": [
        [row, fmt, cleaned, raw]
        for row in _ROWS for fmt in ("markdown", "txt", "json") for cleaned, raw in _TOGGLES
    ] + [[_BASE, "md", True, True], [_BASE, "", False, True]],
    "render_export_document": [
        [_ROWS[:3], fmt, cleaned, raw] for fmt in ("markdown", "txt", "json")
        for cleaned, raw in _TOGGLES
    ] + [
        [[], "markdown", True, True], [[], "txt", True, True], [[], "json", True, True],
        [_ROWS, "markdown", True, True], [_ROWS, "txt", True, True], [_ROWS, "json", True, True],
    ],
    "entry_file_stem": [[entry] for entry in _STEMS],
    "filter_export_entries": [
        [_FILTER_ROWS, "2026-03-01T00:00:00", "2026-03-31T23:59:59", False],
        [_FILTER_ROWS, None, None, True],
        [_FILTER_ROWS, None, None, False],
        [_FILTER_ROWS, "2020-01-01T00:00:00", None, False],
        [_FILTER_ROWS, None, "2026-03-15T07:30:00", False],
        [_FILTER_ROWS, "2026-03-15T07:30:00", "2026-03-15T07:30:00", False],
        [_FILTER_ROWS, "2026-03-01T00:00:00", "2026-03-31T23:59:59", True],
        [_FILTER_ROWS, "2026-03-31T23:59:59.000001", None, False],
        [_FILTER_ROWS, "2026-03-01T00:00:00.000001", "2026-03-01T00:00:00.000001", False],
        [[], "2026-03-01T00:00:00", None, True],
    ],
    "write_per_entry_files": [
        [[_with(id="h_a", text="Same title"), _with(id="h_b", text="Same title")], "txt", []],
        [[_with(text="Existing title")], "txt", ["20260315_073000_Existing-title.txt"]],
        [[_with(text="Existing title")], "markdown",
         ["20260315_073000_Existing-title.markdown",
          "20260315_073000_Existing-title-2.markdown"]],
        [[_with(text="One"), _with(text="One"), _with(text="One")], "json",
         ["20260315_073000_One-2.json"]],
        [[_ROWS[1], _ROWS[5], _ROWS[6], _ROWS[7]], "markdown", []],
        [[_ROWS[2], _ROWS[4], _ROWS[9]], "txt", []],
        [[_ROWS[5], _ROWS[6]], "json", []],
        [[], "txt", []],
    ],
    "_cleanup_label": [
        [{"text": "um hi", "raw_text": "um hi", "cleaned_text": "Hi.",
          "cleanup_provider": "openrouter", "cleanup_model": "m"}],
        [{"text": "Hi.", "raw_text": "um hi", "cleaned_text": "Hi.",
          "cleanup_provider": "openrouter", "cleanup_model": "m"}],
        [{"text": "x", "cleanup_model": "gpt-4o"}],
        [{"text": "x", "raw_text": "y"}],
        [{"text": "x", "raw_text": "x", "cleanup_provider": "openai"}],
        [{"text": "", "raw_text": "", "cleanup_model": ""}],
        [{}],
    ],
}

WRAPPERS = {
    "serialize_history_entry": _serialize,
    "render_entry_document": _render_entry,
    "render_export_document": _render_export,
    "filter_export_entries": _filter,
    "write_per_entry_files": _write,
}

PINS = {
    "timezone_note": "aware timestamps convert with astimezone(): the machine's zone, Pacific "
                     "(America/Los_Angeles rules); replay with that zone passed in explicitly",
    "now": "render_export_document's datetime.now() is 2026-03-15T14:30:05 local",
}
