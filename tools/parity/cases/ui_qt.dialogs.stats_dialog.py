"""The text helpers of ui_qt/dialogs/stats_dialog.py and the chart helpers it imports from
ui_qt/widgets/stats_charts.py (M2.27 history-export-stats).

    _days(count), _count(count, one[, many]), _duration(minutes)
    _short_day(day), _long_day(day)             day is an isoformat() date
    _trend(key, days, words, previous, first)   a PeriodStats of that key, span (None for all
                                                time), words and previous_words; first is the
                                                summary's first_day (isoformat or None). The
                                                result is [text, tone].
    _streak_detail(today, streak, longest)      a summary's dictated_today, day_streak and
                                                longest_streak
    _feature_text(key, count, seconds, words, shots, top)
                                                a FeatureStats; top is [[name, times], ...]. The
                                                result is [name, detail].
    trend_text(key, count)                      _TREND_TEXT[key](count)
    words_text(count), hour_text(hour)          stats_charts' (stats_dialog imports both)
    level(days, words)                          ActivityMap.set_days(days) then level(words); days
                                                is [[isoformat date, words], ...]
    peak(grid)                                  HourGrid.peak() of a 7 x 24 grid; [weekday, hour]
                                                or None

The charts' methods run on a stand-in self, so no QApplication is needed. strftime runs in the C
locale, as it does on Windows, so day and month names are English.
"""

from datetime import date
from types import SimpleNamespace


def _short_day(module, day):
    return module._short_day(date.fromisoformat(day))


def _long_day(module, day):
    return module._long_day(date.fromisoformat(day))


def _trend(module, key, days, words, previous, first):
    period = SimpleNamespace(key=key, days=days, words=words, previous_words=previous)
    return list(module._trend(period, date.fromisoformat(first) if first else None))


def _streak_detail(module, today, streak, longest):
    summary = SimpleNamespace(dictated_today=today, day_streak=streak, longest_streak=longest)
    return module._streak_detail(summary)


def _feature_text(module, key, count, seconds, words, shots, top):
    feature = SimpleNamespace(key=key, count=count, seconds=seconds, words=words, shots=shots,
                              top=tuple(tuple(pair) for pair in top))
    return list(module._feature_text(feature))


def _trend_text(module, key, count):
    return module._TREND_TEXT[key](count)


class _Chart:
    """What ActivityMap.set_days and HourGrid.peak reach on self, without a widget."""

    def __init__(self):
        self._days = ()
        self._grid = ()
        self._thresholds = ()

    def setAccessibleName(self, _name):
        pass

    def setAccessibleDescription(self, _text):
        pass

    def update(self):
        pass


def _level(module, days, words):
    chart = _Chart()
    module.ActivityMap.set_days(chart, [(date.fromisoformat(day), count) for day, count in days])
    return module.ActivityMap.level(chart, words)


def _peak(module, grid):
    chart = _Chart()
    chart._grid = tuple(tuple(day) for day in grid)
    peak = module.HourGrid.peak(chart)
    return list(peak) if peak is not None else None


WRAPPERS = {
    "_short_day": _short_day,
    "_long_day": _long_day,
    "_trend": _trend,
    "_streak_detail": _streak_detail,
    "_feature_text": _feature_text,
    "trend_text": _trend_text,
    "level": _level,
    "peak": _peak,
}


def _grid(*cells):
    grid = [[0] * 24 for _weekday in range(7)]
    for weekday, hour, words in cells:
        grid[weekday][hour] = words
    return grid


_MONTH = [[f"2026-10-{day:02d}", words] for day, words in
          ((1, 0), (2, 3), (3, 0), (4, 7), (5, 4), (6, 12), (7, 1), (8, 40), (9, 0))]

CASES = {
    "_days": [[0], [1], [2], [7], [30], [1000]],
    "_count": [
        [0, "meeting"], [1, "meeting"], [2, "meeting"], [1234, "file"], [1, "walkthrough"],
        [5, "screenshot"], [1, "correction"], [12, "correction"], [2, "child", "children"],
        [1, "child", "children"], [1234567, "word"],
    ],
    "_duration": [
        [0], [0.0001], [0.4], [0.999], [1], [1.5], [2.5], [3.5], [12.2], [59.4], [59.5], [60],
        [61], [90.5], [119.5], [120], [125], [150.5], [599.9], [5999.4], [5999.6], [6000],
        [6059], [6061], [12345.6], [-3],
    ],
    "_short_day": [["2026-10-21"], ["2026-01-01"], ["2025-12-31"], ["2026-05-09"]],
    "_long_day": [["2026-10-06"], ["2024-02-29"], ["2026-12-25"]],
    "_trend": [
        ["week", 7, 120, 100, None], ["week", 7, 95, 100, None], ["week", 7, 100, 100, None],
        ["month", 30, 50, 200, None], ["month", 30, 300, 0, None], ["week", 7, 10, None, None],
        ["week", 7, 1005, 1000, None], ["week", 7, 1015, 1000, None], ["week", 7, 0, 3, None],
        ["month", 30, 2, 8, None], ["month", 30, 5, 8, None], ["all", None, 500, None, "2026-10-06"],
        ["all", None, 0, None, None],
    ],
    "_streak_detail": [
        [True, 1, 1], [True, 3, 3], [True, 3, 9], [False, 2, 2], [False, 2, 5], [False, 0, 0],
        [False, 0, 4], [True, 1, 6],
    ],
    "_feature_text": [
        ["meetings", 1, 1800.0, 0, 0, []], ["meetings", 3, 7500.0, 120, 0, []],
        ["meetings", 2, 0.5, 0, 0, []], ["meetings", 2, 59.0, 0, 0, []],
        ["files", 1, 600.0, 2, 0, []], ["files", 2, 0.0, 0, 0, []],
        ["files", 1234, 360000.0, 1000000, 0, []],
        ["walkthroughs", 1, 120.0, 0, 3, []], ["walkthroughs", 2, 0.0, 0, 0, []],
        ["walkthroughs", 1, 0.0, 0, 1, []],
        ["actions", 1, 0.0, 0, 0, [["Claude", 1]]],
        ["actions", 12, 0.0, 0, 0, [["Slack", 8], ["Notes", 4]]], ["actions", 3, 0.0, 0, 0, []],
        ["actions", 2000, 0.0, 0, 0, [["Webhook", 1500], ["", 500]]],
        ["commands", 5, 0.0, 300, 0, []], ["commands", 1, 0.0, 0, 0, []],
        ["snippets", 9, 0.0, 200, 0, []], ["snippets", 1, 0.0, 1, 0, []],
        ["snippets", 4, 0.0, 0, 0, []],
        ["dictionary", 3, 0.0, 0, 0, []], ["dictionary", 1, 0.0, 0, 0, []],
    ],
    "trend_text": [
        ["meetings", 1], ["meetings", 2], ["files", 0], ["files", 1], ["walkthroughs", 3],
        ["actions", 1], ["actions", 1500], ["commands", 2], ["snippets", 4], ["dictionary", 1],
        ["dictionary", 1001],
    ],
    "words_text": [[0], [1], [2], [999], [1000], [1234567]],
    "hour_text": [[0], [1], [6], [11], [12], [13], [18], [23], [24]],
    "level": [
        [[], 0], [[], 5],
        [[["2026-10-06", 7]], 0], [[["2026-10-06", 7]], 7], [[["2026-10-06", 7]], 8],
        [[["2026-10-03", 3], ["2026-10-04", 4], ["2026-10-06", 7]], 3],
        [[["2026-10-03", 3], ["2026-10-04", 4], ["2026-10-06", 7]], 4],
        [[["2026-10-03", 3], ["2026-10-04", 4], ["2026-10-06", 7]], 7],
        [_MONTH, 1], [_MONTH, 3], [_MONTH, 4], [_MONTH, 5], [_MONTH, 7], [_MONTH, 11],
        [_MONTH, 12], [_MONTH, 13], [_MONTH, 41],
    ],
    "peak": [
        [_grid()], [_grid((3, 10, 12), (0, 9, 4))], [_grid((2, 5, 6), (1, 7, 6))],
        [_grid((4, 23, 1))], [_grid((6, 0, 9), (6, 1, 9), (5, 2, 8))],
    ],
}
