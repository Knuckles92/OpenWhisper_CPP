"""The counting rules of services/dictation_stats.py (M1.13 history-core).

    local_time(timestamp, offset_hours)   tz = timezone(timedelta(hours=offset_hours)); the
                                          result is the naive isoformat(), or None
    _stat(fields)                         the row's columns (id excluded), or None
"""

from datetime import timedelta, timezone

_STAT_COLUMNS = (
    "entry_id", "timestamp", "entry_kind", "app_name", "app_category", "cleanup_level",
    "spoken_words", "final_words", "words_edited", "audio_seconds", "language", "target",
    "snippets", "snippet_words", "corrections", "shots",
)
_AT = "2026-10-06T19:00:00+00:00"


def _local_time(module, timestamp, offset_hours):
    moment = module.local_time(timestamp, timezone(timedelta(hours=offset_hours)))
    return None if moment is None else moment.isoformat()


def _stat(module, fields):
    row = module._stat(fields)
    return None if row is None else {name: getattr(row, name) for name in _STAT_COLUMNS}


_TEXTS = [
    [None], [""], ["Hello, world."], ["  so — anyway ...  "], ["我喜欢你"],
    ["今日はgood day"], ["안녕 하세요"], ["--- *** !!!"],
    ["don't stop_me now"], ["café naïve"], ["tab\tsep\nnewline　ideographic"],
    ["カタカナ。"], ["mixed中文words"], ["豈㐀"],
    [" nbsp "], ["é combining"], ["\U0001F600 smile"], ["12 34.5"],
]

CASES = {
    "words": _TEXTS,
    "count_words": _TEXTS,
    "words_edited": [
        ["a b c", "a b c"], ["a b c", "a x c"], ["a b c", "a c"], ["a c", "a b c"],
        ["um so I think", "I think"], ["one two", "three four five"], [None, "a b"],
        ["a b", None], [None, None], ["Hello world", "hello, world!"],
        ["the cat sat on the mat", "the dog sat on a mat today"],
        ["我喜欢你", "我爱你"],
        [" ".join(["w"] * 250), " ".join(["w"] * 240 + ["x"] * 20)],
    ],
    "next_milestone": [[0], [999], [1_000], [4_999], [57_224], [999_999], [1_000_000],
                       [2_400_000], [3_000_000], [-5]],
    "local_time": [
        ["2026-10-05T23:30:00+00:00", -7], ["2026-10-05T23:30:00+00:00", 9],
        ["2026-10-05T23:30:00Z", 9], ["2026-10-05T23:30:00", 9], ["not a time", 0],
        [None, 0], ["", 0], ["2026-10-05T23:30:00.250000+02:00", -7],
        ["2026-12-31T23:30:00-01:00", 1], ["2026-10-05", 5], ["2026-10-05T23:30Z", -10],
    ],
    "_stat": [
        [{"text": "Hello, world.", "raw_text": "um hello world", "audio_duration": 3.0,
          "entry_kind": "dictation", "app_name": "Slack", "app_category": "work",
          "cleanup_level": "medium", "language": "en", "source_name": "Quick Record",
          "id": "e1", "timestamp": _AT}],
        [{"text": "a talk", "entry_kind": "file", "audio_duration": 90.0, "timestamp": _AT}],
        [{"text": "upload", "source_name": "talk.mp3", "timestamp": _AT}],
        [{"text": "theirs", "entry_kind": "dictation", "origin_device_id": "laptop",
          "timestamp": _AT}],
        [{"text": "said before", "entry_kind": "file", "timestamp": _AT,
          "stats_counts": {"retranscription": True}}],
        [{"text": "Make it formal, please.", "raw_text": "make it formal pls",
          "entry_kind": "command", "audio_duration": 4.0, "timestamp": _AT}],
        [{"text": "rewrite", "raw_text": "draft", "entry_kind": "transform", "timestamp": _AT}],
        [{"text": "sent", "entry_kind": "action", "timestamp": _AT,
          "stats_counts": {"target": "Email", "snippets": 2, "snippet_words": 30.7,
                           "corrections": -3, "shots": True}}],
        [{"entry_kind": "dictation", "text": " ".join(f"word{i}" for i in range(120)),
          "spoken_text": "my intro", "audio_duration": 1.2, "timestamp": _AT}],
        [{"text": "steps", "entry_kind": "walkthrough", "audio_duration": 60, "timestamp": _AT,
          "stats_counts": {"shots": 4}}],
        [{"text": "x", "entry_kind": "meeting", "timestamp": _AT}],
        [{"text": "x", "entry_kind": "failed_recording", "timestamp": _AT}],
        [{"text": "Hello.", "raw_text": "hello", "cleaned_text": "Hello there.",
          "entry_kind": "dictation", "timestamp": _AT, "app_name": "", "language": ""}],
        [{"text": "", "raw_text": "", "source_name": "Quick Record · Email",
          "timestamp": _AT}],
        [{"text": "file", "entry_kind": "file", "timestamp": _AT,
          "stats_counts": {"retranscription": False, "target": 0}}],
        [{"text": "action", "entry_kind": "action", "timestamp": _AT,
          "stats_counts": {"target": 7}}],
        [{"text": "a b", "raw_text": "a b c", "entry_kind": "dictation", "id": "",
          "timestamp": _AT, "audio_duration": None}],
    ],
}

WRAPPERS = {
    "local_time": _local_time,
    "_stat": _stat,
}


# ---- load_summary (M2.8 history-extras) ----
#
#     load_summary(records, today, offset_hours)
#         a fresh database (out/parity/stats-summary-cases); after reset(), each record's fields
#         go through record(fields, None); load_summary(today=date.fromisoformat(today),
#         tz=timezone(timedelta(hours=offset_hours)), or the machine's zone for None: Pacific,
#         America/Los_Angeles rules). Dates come back as isoformat() text; activity as its
#         length, first and last day and the days with words.

import itertools as _itertools  # noqa: E402
import sys as _sys  # noqa: E402
from datetime import date as _date, datetime as _datetime  # noqa: E402
from pathlib import Path as _Path  # noqa: E402

_sys.path.insert(0, str(_Path(__file__).resolve().parents[1]))
import ow_isolation as _ow_isolation  # noqa: E402

_SUMMARY_WORK = _ow_isolation.SCRATCH / "stats-summary-cases"
_summary_counter = _itertools.count()


def _summary_database():
    from services.database import DatabaseManager

    _SUMMARY_WORK.mkdir(parents=True, exist_ok=True)
    path = _SUMMARY_WORK / f"case-{next(_summary_counter)}.db"
    for suffix in ("", "-wal", "-shm", "-journal"):
        _Path(str(path) + suffix).unlink(missing_ok=True)
    return DatabaseManager(db_path=str(path))


def _pairs(items):
    return [[key.isoformat() if hasattr(key, "isoformat") else key, value]
            for key, value in items]


def _period_dict(period):
    return {
        "key": period.key, "days": period.days, "words": period.words,
        "previous_words": period.previous_words, "dictations": period.dictations,
        "rewrites": period.rewrites, "average_wpm": period.average_wpm,
        "words_cleaned_up": period.words_cleaned_up, "top_apps": _pairs(period.top_apps),
        "bars": _pairs(period.bars), "hours": list(period.hours),
        "weekdays": list(period.weekdays), "grid": [list(day) for day in period.grid],
        "categories": _pairs(period.categories), "cleanup_levels": _pairs(period.cleanup_levels),
        "languages": _pairs(period.languages), "minutes_saved": period.minutes_saved,
        "features": [{"key": f.key, "count": f.count, "seconds": f.seconds, "words": f.words,
                      "shots": f.shots, "top": _pairs(f.top), "trend": list(f.trend)}
                     for f in period.features],
    }


def _load_summary(module, records, today, offset_hours):
    database = _summary_database()
    saved = module.db
    module.db = database
    try:
        module.reset()
        for fields in records:
            module.record(fields, None)
        tz = None if offset_hours is None else timezone(timedelta(hours=offset_hours))
        summary = module.load_summary(today=_date.fromisoformat(today), tz=tz)
    finally:
        module.db = saved
        database.close()
    return {
        "words_all_time": summary.words_all_time, "entries": summary.entries,
        "day_streak": summary.day_streak, "longest_streak": summary.longest_streak,
        "dictated_today": summary.dictated_today,
        "first_day": None if summary.first_day is None else summary.first_day.isoformat(),
        "activity": {
            "length": len(summary.activity),
            "first": summary.activity[0][0].isoformat() if summary.activity else None,
            "last": summary.activity[-1][0].isoformat() if summary.activity else None,
            "words": {day.isoformat(): words for day, words in summary.activity if words},
        },
        "periods": [_period_dict(period) for period in summary.periods],
    }


def _summary_at(day, hour=12, offset=-7):
    year, month, dom = map(int, day.split("-"))
    local = _datetime(year, month, dom, hour, tzinfo=timezone(timedelta(hours=offset)))
    return local.astimezone(timezone.utc).isoformat()


def _rec(text, day="2026-10-06", hour=12, kind="dictation", **fields):
    row = {"text": text, "entry_kind": kind, "timestamp": _summary_at(day, hour)}
    row.update(fields)
    return row


_SUMMARY_ROWS = [
    _rec("one two three four five six", audio_duration=3.0, app_name="Slack"),
    _rec("one two three", day="2026-10-05", audio_duration=0.5, app_name="Outlook"),
    _rec("one two", day="2026-10-04", audio_duration=60.0, app_name="Slack", raw_text="1 2 3 4"),
    _rec("old words here", day="2026-09-29", app_name="Notion"),
    _rec("older", day="2026-09-16", app_name="Word"),
    _rec("rewrite", kind="transform", raw_text="draft"),
]

_FEATURE_ROWS = [
    _rec("eight words said aloud for the real count",
         raw_text="eight words said aloud for the real count",
         stats_counts={"snippets": 2, "snippet_words": 40, "corrections": 3},
         app_category="work", cleanup_level="light", language="en", hour=9),
    _rec("a walkthrough", kind="walkthrough", audio_duration=120.0, stats_counts={"shots": 5}),
    _rec("to claude", kind="action", stats_counts={"target": "Claude"}),
    _rec("to claude again", kind="action", day="2026-10-05", stats_counts={"target": "claude"}),
    _rec("to the hook", kind="action", stats_counts={"target": "Webhook"}),
    _rec("to the zed", kind="action", stats_counts={"target": "zed"}),
    _rec("Formal now.", kind="command", raw_text="formal nw"),
    _rec("a talk of five words", kind="file", audio_duration=600.0),
    _rec("one two", app_category="email", language="it", hour=15, day="2026-09-20",
         stats_counts={"snippets": 1, "snippet_words": 3}),
    _rec("one", day="2026-10-05", hour=9, stats_counts={"corrections": 1}),
    _rec("old file", kind="file", day="2025-11-03", audio_duration=30.5),
]

_ZONE_ROWS = [
    {"text": "late night words", "entry_kind": "dictation",
     "timestamp": "2026-10-06T06:00:00+00:00", "audio_duration": 2.0},
    {"text": "spring forward", "entry_kind": "dictation",
     "timestamp": "2026-03-08T09:59:00+00:00", "audio_duration": 240.0},
    {"text": "fall back", "entry_kind": "dictation",
     "timestamp": "2026-11-01T08:30:00Z", "audio_duration": 1.0},
    {"text": "legacy naive", "source_name": "Quick Record",
     "timestamp": "2026-10-05T23:30:00", "audio_duration": 1.0},
    {"text": "bad stamp", "entry_kind": "dictation", "timestamp": "yesterday"},
]

_TIE_ROWS = [
    _rec("w w", app_name="beta"), _rec("w w", app_name="Alpha"), _rec("w w", app_name="alpha"),
    _rec("w w w", app_name="Zed"), _rec("w", app_name="gamma"), _rec("w", app_name="Delta"),
    _rec("w", app_name="epsilon"), _rec("no app here at all"),
    _rec("w", app_category="", language="", cleanup_level=""),
]

_LONG_AGO_ROWS = [
    _rec("words here", day="2025-06-15"), _rec("more", day="2026-09-01"),
    _rec("long ago", day="2025-09-30", audio_duration=1.0),
    _rec("a b c d e f g h i j", day="2026-10-06", audio_duration=40.0),
]

_STREAK_ROWS = [_rec("words", day=f"2026-10-{d:02d}") for d in (5, 4, 3, 1)] + [
    _rec("words", day=f"2026-09-{d:02d}") for d in (30, 29, 28, 27, 26)
] + [_rec("x", kind="command", day="2026-09-25", raw_text="y")]

CASES["load_summary"] = [
    [_SUMMARY_ROWS, "2026-10-06", -7],
    [_SUMMARY_ROWS, "2026-10-07", -7],
    [_FEATURE_ROWS, "2026-10-06", -7],
    [_ZONE_ROWS, "2026-10-06", -7],
    [_ZONE_ROWS, "2026-10-06", 9],
    [_ZONE_ROWS, "2026-10-06", None],
    [_TIE_ROWS, "2026-10-06", -7],
    [_LONG_AGO_ROWS, "2026-10-06", -7],
    [[_rec("fresh start")], "2026-10-06", -7],
    [[_rec("only a file", kind="file")], "2026-10-06", -7],
    [_STREAK_ROWS, "2026-10-06", -7],
    [_STREAK_ROWS, "2026-10-02", -7],
    [[], "2026-10-06", -7],
]
WRAPPERS["load_summary"] = _load_summary
