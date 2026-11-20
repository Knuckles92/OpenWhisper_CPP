"""The text and tint helpers of ui_qt/widgets/history_calendar.py (M2.22 history-calendar).

    plural(count, one, many)                    many may be None
    kind_breakdown(by_kind)                     a {kind: count} object (a Counter in Python)
    clock(when)                                 an isoformat() datetime
    pick_lines(items, capacity)                 items are [record_id, kind]; the result is the
                                                picked record ids, in order
    day_tooltip(day, summary, milestone)        day is an isoformat() date; summary is
                                                [count, seconds, {kind: count}] or None
    _heat(count, peak, theme)                   the colour as QColor.name(HexArgb), "#aarrggbb",
                                                with the "dark" or "light" palette current

strftime runs in the C locale, as it does on Windows, so names and AM/PM are English.
"""

from collections import Counter
from datetime import date, datetime


def _items(pairs):
    from services.history_calendar import HERE, CalendarItem

    return [CalendarItem(kind=kind, record_id=record_id, when=datetime(2026, 9, 1, 8, 0),
                         title="", preview="", seconds=0.0, place=HERE, source=None)
            for record_id, kind in pairs]


def _pick_lines(module, pairs, capacity):
    return [item.record_id for item in module.pick_lines(_items(pairs), capacity)]


def _summary(value):
    from services.history_calendar import DaySummary

    if value is None:
        return None
    count, seconds, by_kind = value
    return DaySummary(count=count, seconds=seconds, by_kind=Counter(by_kind))


def _day_tooltip(module, day, summary, milestone):
    return module.day_tooltip(date.fromisoformat(day), _summary(summary), milestone)


def _clock(module, when):
    return module.clock(datetime.fromisoformat(when))


def _kind_breakdown(module, by_kind):
    return module.kind_breakdown(Counter(by_kind))


def _heat(module, count, peak, theme):
    from PyQt6.QtGui import QColor

    from ui_qt.utils import palette

    previous = palette.current_palette()
    palette.set_current_palette(palette.palette_for(theme))
    try:
        return module._heat(count, peak).name(QColor.NameFormat.HexArgb)
    finally:
        palette.set_current_palette(previous)


WRAPPERS = {
    "pick_lines": _pick_lines,
    "day_tooltip": _day_tooltip,
    "clock": _clock,
    "kind_breakdown": _kind_breakdown,
    "_heat": _heat,
}

_CROWDED = [[f"d{n}", "dictation"] for n in range(6)] + [["m", "meeting"], ["f", "file"]]
_MIXED = [["f0", "file"], ["d0", "dictation"], ["f1", "file"], ["m0", "meeting"],
          ["d1", "dictation"], ["m1", "meeting"], ["d2", "dictation"]]

CASES = {
    "plural": [
        [0, "recording", None], [1, "recording", None], [2, "recording", None],
        [1000, "recording", None], [1234567, "word", None], [1, "dictation", "dictations"],
        [3, "file", "files"], [-1, "day", None], [-2000, "day", None], [1, "", None],
        [2, "", "many"], [11, "recording", None], [99, "day", None], [100, "day", None],
        [12345, "recording", None], [-12, "day", None], [-123456, "day", None],
    ],
    "kind_breakdown": [
        [{}], [{"dictation": 4}], [{"dictation": 4, "file": 1, "meeting": 1}],
        [{"meeting": 2, "dictation": 1}], [{"file": 1}], [{"dictation": 0, "file": 2}],
        [{"dictation": 1200, "meeting": 1}], [{"walkthrough": 3, "dictation": 1}],
        [{"dictation": 12, "file": 45, "meeting": 10}],
    ],
    "clock": [
        ["2026-09-27T09:02:00"], ["2026-09-27T00:00:00"], ["2026-09-27T00:30:59"],
        ["2026-09-27T12:00:00"], ["2026-09-27T12:59:00"], ["2026-09-27T13:05:00"],
        ["2026-09-27T23:59:59.999999"], ["2026-09-27T10:10:00"], ["2026-03-15T15:04:00"],
    ],
    "pick_lines": [
        [_CROWDED, 3], [_CROWDED, 4], [_CROWDED, 8], [_CROWDED, 9], [_CROWDED, 1],
        [_CROWDED, 0], [_CROWDED[:2], 3], [[], 2], [_MIXED, 1], [_MIXED, 2], [_MIXED, 3],
        [_MIXED, 4], [_MIXED, 6], [_MIXED, -1],
    ],
    "day_tooltip": [
        ["2026-09-27", None, None],
        ["2026-09-27", [0, 0.0, {}], None],
        ["2026-09-27", [0, 0.0, {}], 10],
        ["2026-09-27", [1, 0.0, {"dictation": 1}], None],
        ["2026-09-27", [3, 125.0, {"dictation": 2, "meeting": 1}], None],
        ["2026-09-27", [3, 0.99, {"dictation": 3}], 10],
        ["2026-09-27", [3, 1.0, {"dictation": 3}], 1],
        ["2026-02-01", [1500, 7322.5, {"file": 500, "dictation": 1000}], 1000],
        ["2025-12-31", [2, 59.95, {"file": 1, "meeting": 1}], 250],
        ["2026-03-15", [4, 3600.0, {"dictation": 4}], 0],
        ["2024-02-29", [2, 30.25, {"walkthrough": 1, "dictation": 1}], None],
        ["2026-03-11", [23, 840.0, {"dictation": 21, "file": 2}], 50],
    ],
    "_heat": [
        [count, peak, theme]
        for theme in ("dark", "light")
        for count, peak in ((0, 0), (0, 5), (1, 1), (1, 3), (2, 3), (3, 3), (1, 10), (7, 10),
                            (12, 12), (5, 0), (1, 1000))
    ],
}
