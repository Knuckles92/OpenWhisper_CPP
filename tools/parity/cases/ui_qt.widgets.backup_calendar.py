"""Curated inputs for the pure parts of ui_qt/widgets/backup_calendar.py (M3.9).

Days are ISO dates, months [year, month] pairs.

    _month_counts(latest, next_due, interval_days, today)   [[year, month, count], ...] by month
    is_planned(day, next_due, interval_days)                BackupMonthGrid.is_planned
    events(day, latest, next_due, interval_days)            BackupMonthGrid.events, [[short, long]]
"""
from datetime import date
from types import SimpleNamespace


def _day(value):
    return None if value is None else date.fromisoformat(value)


def _month_counts(module, latest, next_due, interval_days, today):
    counts = module._month_counts(_day(latest), _day(next_due), interval_days, _day(today))
    return [[year, month, counts[(year, month)]] for year, month in sorted(counts)]


def _grid(module, latest, next_due, interval_days):
    grid = SimpleNamespace(latest=_day(latest), next_due=_day(next_due),
                           interval_days=interval_days)
    grid.is_planned = lambda day: module.BackupMonthGrid.is_planned(grid, day)
    return grid


def is_planned(module, day, next_due, interval_days):
    return module.BackupMonthGrid.is_planned(_grid(module, None, next_due, interval_days),
                                             _day(day))


def events(module, day, latest, next_due, interval_days):
    grid = _grid(module, latest, next_due, interval_days)
    return [list(entry) for entry in module.BackupMonthGrid.events(grid, _day(day))]


TODAY = "2026-10-04"
PLANNED_DAYS = ["2026-10-03", "2026-10-04", "2026-10-05", "2026-10-06", "2026-10-11",
                "2026-10-12", "2026-10-18", "2026-10-19", "2026-11-01", "2027-10-05",
                "2027-10-06", "2026-09-27", "2026-09-28"]

CASES = {
    "_month_counts": [
        [None, None, 0, TODAY],
        ["2026-10-04", None, 0, TODAY],
        ["2026-10-04", "2026-10-05", 1, TODAY],
        ["2026-10-04", "2026-10-11", 7, TODAY],
        [None, "2026-10-05", 1, TODAY],
        [None, "2026-10-05", 7, TODAY],
        [None, "2026-10-05", 0, TODAY],
        [None, "2026-10-05", -1, TODAY],
        [None, "2026-10-05", 1, "2026-02-27"],
        ["2025-12-31", "2026-02-28", 7, "2026-02-28"],
        ["2026-02-28", "2026-03-01", 1, "2026-02-28"],
        ["2026-10-04", "2027-10-05", 1, TODAY],
        ["2026-10-04", "2027-10-06", 1, TODAY],
        ["2026-10-04", "2027-10-07", 1, TODAY],
        ["2024-02-29", "2024-03-01", 1, "2024-02-29"],
        ["2026-10-04", "2026-09-01", 7, TODAY],
        ["2026-10-04", "2026-10-04", 7, TODAY],
    ],
    "is_planned": (
        [[day, "2026-10-05", 1] for day in PLANNED_DAYS]
        + [[day, "2026-10-05", 7] for day in PLANNED_DAYS]
        + [[day, None, 7] for day in PLANNED_DAYS[:3]]
        + [[day, "2026-10-05", 0] for day in PLANNED_DAYS[:3]]
        + [["2026-10-12", "2026-10-05", -7], ["2026-10-05", "2026-10-05", 3],
           ["2026-10-08", "2026-10-05", 3], ["2026-10-09", "2026-10-05", 3]]
    ),
    "events": [
        ["2026-10-04", "2026-10-04", "2026-10-11", 7],
        ["2026-10-11", "2026-10-04", "2026-10-11", 7],
        ["2026-10-18", "2026-10-04", "2026-10-11", 7],
        ["2026-10-12", "2026-10-04", "2026-10-11", 7],
        ["2026-10-05", "2026-10-04", "2026-10-05", 1],
        ["2026-10-04", "2026-10-04", "2026-10-04", 1],
        ["2026-10-04", None, "2026-10-04", 7],
        ["2026-10-04", "2026-10-04", None, 0],
        ["2026-10-03", None, None, 0],
        ["2026-10-04", "2026-10-04", "2026-10-11", 0],
    ],
}

WRAPPERS = {"_month_counts": _month_counts, "is_planned": is_planned, "events": events}
