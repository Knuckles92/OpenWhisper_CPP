"""The pure half of services/history_calendar.py (M2.8 history-extras).

Zones are given as an hour offset, timezone(timedelta(hours=offset)), or None for the machine's
zone (pins.local_timezone: Pacific, America/Los_Angeles rules, which the replay passes in
explicitly). Dates and datetimes come back as isoformat() text, places as [scope, name].

    local_wall_time(value, offset)               the naive isoformat(), or None
    classify_source(source_name, entry_kind)     [kind, label]
    record_place(record)                         a dict as given
    place_label(place)                           [scope, name] or None
    shift_month(key, months), month_distance(a, b), month_key(day)
    month_weeks(year, month, first_weekday)      weeks of isoformat() days
    query_bounds(year, month)
    entry_item(row, when, also_on)               a TranscriptionHistory from the row's columns
                                                 (stored_on set when given); the result is
                                                 [kind, record_id, title, preview, seconds,
                                                 place, label, location]
    calendar_item(kind, title, preview, place, also_on)   [label, location]
    index(records, offset, today, months, queries)
        records are [id, timestamp, source_name, entry_kind, origin_device_name, seconds],
        placed as CalendarSource.load_index places History rows; the result holds the marks
        in order, places, month counts, streak, every milestone, and for each month its days,
        month stats and milestone days, then nearest_month for each [key, kinds, place] query;
        each count is given unfiltered and filtered by each query's kinds and place
"""

from datetime import date, datetime, timedelta, timezone


def _zone(offset):
    return None if offset is None else timezone(timedelta(hours=offset))


def _place(value):
    return None if value is None else (value[0], value[1])


def _local_wall_time(module, value, offset):
    moment = module.local_wall_time(value, _zone(offset))
    return None if moment is None else moment.isoformat()


def _classify(module, source_name, entry_kind):
    return list(module.classify_source(source_name, entry_kind))


def _record_place(module, record):
    return list(module.record_place(record))


def _place_label(module, place):
    return module.place_label(_place(place))


def _shift_month(module, key, months):
    return list(module.shift_month(tuple(key), months))


def _month_distance(module, a, b):
    return module.month_distance(tuple(a), tuple(b))


def _month_key(module, day):
    return list(module.month_key(date.fromisoformat(day)))


def _month_weeks(module, year, month, first_weekday):
    return [[day.isoformat() for day in week]
            for week in module.month_weeks(year, month, first_weekday)]


def _query_bounds(module, year, month):
    return list(module.query_bounds(year, month))


def _entry_item(module, row, when, also_on):
    from services.models import TranscriptionHistory

    columns = {key: value for key, value in row.items() if key != "stored_on"}
    entry = TranscriptionHistory(**columns)
    if "stored_on" in row:
        entry.stored_on = row["stored_on"]
    item = module.entry_item(entry, datetime.fromisoformat(when), also_on)
    return [item.kind, item.record_id, item.title, item.preview, item.seconds, list(item.place),
            item.label(), list(item.location())]


def _calendar_item(module, kind, title, preview, place, also_on):
    item = module.CalendarItem(kind=kind, record_id="r", when=datetime(2026, 9, 1, 9),
                               title=title, preview=preview, seconds=0.0, place=_place(place),
                               source=None, also_on=also_on)
    return [item.label(), list(item.location())]


def _summary(summary):
    return [summary.count, summary.seconds, dict(sorted(summary.by_kind.items()))]


def _filters(query):
    kinds = query.get("kinds")
    return (None if kinds is None else frozenset(kinds)), _place(query.get("place"))


def _index(module, records, offset, today, months, queries):
    tz = _zone(offset)
    marks = []
    for row_id, stamp, source_name, entry_kind, origin, seconds in records:
        when = module.local_wall_time(stamp, tz)
        if when is None:
            continue
        kind, _label = module.classify_source(source_name, entry_kind)
        place = (module.FROM, origin) if origin else module.HERE
        marks.append(module.Mark(when, kind, place, row_id, float(seconds or 0.0)))
    index = module.CalendarIndex(marks)
    filters = [(None, None)] + [_filters(query) for query in queries]
    result = {
        "marks": [[m.record_id, m.when.isoformat(), m.kind, list(m.place), m.seconds]
                  for m in index.marks],
        "places": [list(place) for place in index.places()],
        "streak": index.streak(date.fromisoformat(today)),
        "milestones": {m.record_id: index.milestone(m.record_id) for m in index.marks
                       if index.milestone(m.record_id) is not None},
        "active_days": sorted(day.isoformat() for day in index.active_days),
        "selected": [[m.record_id for m in index.select(kinds, place)]
                     for kinds, place in filters],
        "month_counts": [sorted([key[0], key[1], count]
                                for key, count in index.month_counts(kinds, place).items())
                         for kinds, place in filters],
        "months": {},
        "nearest": [],
    }
    for year, month in months:
        stats = []
        for kinds, place in filters:
            month_stats = index.month_stats(year, month, kinds, place)
            stats.append({
                "days": {day.isoformat(): _summary(summary)
                         for day, summary in index.days(year, month, kinds, place).items()},
                "count": month_stats.count,
                "seconds": month_stats.seconds,
                "active_days": month_stats.active_days,
                "busiest": None if month_stats.busiest is None
                else [month_stats.busiest[0].isoformat(), month_stats.busiest[1]],
                "by_kind": dict(sorted(month_stats.by_kind.items())),
            })
        result["months"][f"{year:04d}-{month:02d}"] = {
            "filtered": stats,
            "milestone_days": {day.isoformat(): ordinal for day, ordinal
                               in sorted(index.milestone_days(year, month).items())},
        }
    for query in queries:
        kinds, place = _filters(query)
        for key in query["keys"]:
            found = index.nearest_month(tuple(key), kinds, place)
            result["nearest"].append(None if found is None else list(found))
    return result


_STAMPS = [
    "2026-09-01T05:30:00+00:00", "2026-09-01T05:30:00Z", "2026-09-01T05:30:00",
    " 2026-09-01T05:30:00Z ", "", None, "yesterday", "2026-03-08T09:59:00+00:00",
    "2026-03-08T10:00:00+00:00", "2026-11-01T08:30:00+00:00", "2026-11-01T09:30:00+00:00",
    "2026-12-31T23:30:00-01:00", "2026-10-05T23:30:00.250000+02:00", "2026-10-05",
    "20261005T233000Z", "9999-12-31T23:00:00-05:00", "0001-01-01T00:00:00+05:00",
    "1969-12-31T23:59:59+00:00", "2026-09-01T05:30:00ZZ", "2026-W36-2T10:00",
]

# A year of synthetic records: dictations, files, meetings, rewrites, a paired computer.
_RECORDS = [
    ["d01", "2026-08-30T15:00:00+00:00", "Quick Record", None, None, 4.5],
    ["d02", "2026-08-31T07:30:00+00:00", None, "dictation", None, 10],
    ["d03", "2026-09-01T05:30:00+00:00", "Quick Record · Email", None, None, None],
    ["d04", "2026-09-01T05:30:00", "notes.wav", None, None, 12.5],
    ["d05", "2026-09-01T16:00:00+00:00", "Quick Record", "command", None, 0],
    ["d06", "2026-09-01T17:00:00+00:00", "Transform · Polish", "transform", None, 2.25],
    ["d07", "2026-09-02T18:00:00+00:00", "call.m4a", "file", "laptop", 600],
    ["d08", "2026-09-03T12:00:00+00:00", "Action · Send", "action", "Laptop", 1],
    ["d09", "2026-09-03T12:00:00+00:00", None, "walkthrough", "desk", 30],
    ["d10", "2026-09-15T12:00:00+00:00", "Quick Recordings.m4a", None, None, 0.5],
    ["d11", "2026-09-30T23:30:00+00:00", "Quick Record", None, None, 1],
    ["d12", "2026-10-01T06:59:00+00:00", None, None, None, None],
    ["d13", "2026-10-01T07:00:00+00:00", "x.mp3", "FILE", None, 3],
    ["d14", "2026-11-01T08:30:00+00:00", None, "dictation", None, 1],
    ["d15", "2026-11-01T09:30:00+00:00", None, "dictation", None, 1],
    ["d16", "2026-03-08T09:59:00+00:00", None, None, None, 1],
    ["d17", "2026-03-08T10:00:00+00:00", None, None, None, 1],
    ["d18", "2025-12-31T20:00:00+00:00", None, None, None, 1],
    ["d19", "not a time", None, None, None, 1],
    ["d20", "2026-05-02T09:00:00", "Quick Record", None, None, 7],
] + [
    [f"r{n:02d}", f"2026-07-{1 + n % 28:02d}T{n % 24:02d}:15:00+00:00", None, "dictation",
     None, n / 4] for n in range(40)
]

_MONTHS = [[2026, 7], [2026, 8], [2026, 9], [2026, 10], [2026, 11], [2026, 3], [2025, 12],
           [2026, 1]]

_QUERIES = [
    {"kinds": ["meeting"], "place": None, "keys": [[2026, 7], [2027, 3]]},
    {"kinds": ["dictation"], "place": ["here", ""], "keys": [[2026, 6], [2026, 4], [2024, 1]]},
    {"kinds": ["file", "dictation"], "place": ["from", "laptop"],
     "keys": [[2026, 9], [2026, 12], [2026, 6]]},
    {"kinds": None, "place": None, "keys": [[2026, 6], [2026, 4], [2027, 3], [2020, 1]]},
]

_ENTRY = {"id": "e1", "text": "hello there", "timestamp": "2026-09-01T05:30:00+00:00",
          "model": "parakeet"}


def _entry(**fields):
    row = dict(_ENTRY)
    row.update(fields)
    return row


CASES = {
    # In the machine's zone, Windows' localtime refuses aware moments before 1970 (OSError);
    # the C++ calendar converts with the clock's zone and does not, as dictation_stats does not.
    "local_wall_time": [[stamp, offset] for stamp in _STAMPS for offset in (None, -7, 9)
                        if not (offset is None and stamp == "1969-12-31T23:59:59+00:00")],
    "classify_source": [
        [None, None], ["", None], ["Quick Record", None], ["Quick Record · Email", None],
        ["interview.wav", None], ["3 files: a.mp3, b.mp3, c.mp3", None],
        ["Quick Recordings.m4a", None], ["Quick Record · Email", "dictation"],
        ["Quick Record", "command"], ["Transform · Polish", "transform"],
        ["Polish", "transform"], [None, "transform"], ["Quick Record", "file"],
        ["call.m4a", "file"], ["Action · Send it", "action"], ["Action · ", "action"],
        [None, "action"], ["Notes", "walkthrough"], ["  Quick Record ·  Work  ", None],
        ["Quick Record·Email", None], ["Quick Record  ·  Email", None],
        ["memo.wav", "dictation"], ["memo.wav", "  Dictation "], ["memo.wav", "meeting"],
        ["memo.wav", "failed_recording"], ["", "file"], ["Quick Record · ", None],
        ["Transform ·   Fix tone  ", "TRANSFORM"], [" Quick Record ", None],
    ],
    "record_place": [
        [{}], [{"stored_on": "jed"}], [{"stored_on": "  ", "origin_device_name": "laptop"}],
        [{"stored_on": "jed", "origin_device_name": "laptop"}],
        [{"origin_device_name": " laptop "}], [{"stored_on": None, "origin_device_name": None}],
        [{"stored_on": 0, "origin_device_name": 7}],
    ],
    "place_label": [[None], [["here", ""]], [["from", "laptop"]], [["on", "jed"]],
                    [["elsewhere", "x"]]],
    "is_milestone": [[n] for n in (-1000, 0, 1, 2, 9, 10, 11, 50, 100, 250, 251, 500, 999,
                                   1000, 1500, 2000, 3000, 10000)],
    "ordinal_suffix": [[n] for n in (0, 1, 2, 3, 4, 10, 11, 12, 13, 14, 20, 21, 22, 23, 101,
                                     111, 112, 113, 121, 500, 1000, 1001, 1011, 1234567,
                                     -1, -11, -21, -1000)],
    "milestone_text": [[n] for n in (1, 10, 50, 100, 250, 500, 1000, 2000, 1002)],
    "shift_month": [[[2026, 12], 1], [[2026, 1], -1], [[2026, 9], -21], [[2026, 9], 0],
                    [[2026, 9], 27], [[1, 1], -1], [[2026, 6], -1200]],
    "month_distance": [[[2026, 7], [2026, 5]], [[2026, 8], [2026, 9]], [[2025, 12], [2027, 1]],
                       [[2026, 1], [2026, 1]]],
    "month_key": [["2026-09-30"], ["2027-01-01"]],
    "month_weeks": [[2026, 9, 6], [2026, 9, 0], [2026, 2, 6], [2026, 2, 0], [2015, 2, 6],
                    [2015, 2, 0], [2026, 11, 2], [2026, 12, 6], [2026, 5, 13], [2026, 5, -1],
                    [2026, 13, 6], [1, 1, 6], [9999, 12, 0]],
    "query_bounds": [[2026, 9], [2026, 12], [2028, 2], [2026, 1], [2026, 13], [1, 1],
                     [9999, 12], [9999, 11]],
    "entry_item": [
        [_entry(), "2026-08-31T22:30:00", ""],
        [_entry(source_name="Quick Record · Email"), "2026-08-31T22:30:00", "jed"],
        [_entry(source_name="call.m4a", entry_kind="file", title="Call with Sam",
                audio_duration=12.5), "2026-09-02T10:00:00", ""],
        [_entry(source_name="Quick Record", entry_kind="command", text="  "),
         "2026-09-02T08:00:00", ""],
        [_entry(source_name="Transform · Polish", entry_kind="transform", title="   "),
         "2026-09-02T09:00:00", ""],
        [_entry(origin_device_id="dev-1", origin_device_name="laptop"), "2026-09-15T05:00:00",
         ""],
        [_entry(stored_on="jed", text="kept on the host", audio_duration=3.0),
         "2026-09-05T08:00:00", ""],
        [_entry(text="x" * 120), "2026-09-05T08:00:00", ""],
        [_entry(source_name="notes.wav", text=""), "2026-09-05T08:00:00", ""],
    ],
    "calendar_item": [
        ["dictation", "", "", ["here", ""], ""],
        ["dictation", "Email", "", ["here", ""], ""],
        ["dictation", "Email", "hello", ["here", ""], "jed"],
        ["file", "", "", ["from", "laptop"], ""],
        ["file", "", "transcript", ["from", "laptop"], "jed"],
        ["meeting", "", "", ["on", "jed"], "jed"],
        ["meeting", "Standup", "notes", ["here", ""], ""],
    ],
    "index": [
        [_RECORDS, None, "2026-09-03", _MONTHS, _QUERIES],
        [_RECORDS, 9, "2026-09-03", _MONTHS, _QUERIES],
        [_RECORDS, None, "2026-09-04", _MONTHS, _QUERIES],
        [_RECORDS, 9, "2026-09-05", _MONTHS, _QUERIES],
        [_RECORDS, -7, "2026-07-28", _MONTHS, _QUERIES],
        [[], None, "2026-09-03", [[2026, 9]], _QUERIES],
    ],
}

WRAPPERS = {
    "local_wall_time": _local_wall_time,
    "classify_source": _classify,
    "record_place": _record_place,
    "place_label": _place_label,
    "shift_month": _shift_month,
    "month_distance": _month_distance,
    "month_key": _month_key,
    "month_weeks": _month_weeks,
    "query_bounds": _query_bounds,
    "entry_item": _entry_item,
    "calendar_item": _calendar_item,
    "index": _index,
}

PINS = {
    "timezone_note": "a None offset is the machine's zone, Pacific (America/Los_Angeles "
                     "rules); replay it with that zone passed in explicitly",
}
