"""Curated inputs for meeting/time_utils.py (M7.3 meeting-state-model).

The machine's zone and "now" are pinned while each case runs: the module's `datetime` is
swapped for a subclass whose now() is NOW_UTC and whose astimezone() with no argument converts
to America/New_York (US rules since 2007, PEP 495 fold=0 for gaps and repeated hours, as
CPython's own astimezone() resolves them). The replay uses a FakeClock at NOW_UTC in
America/New_York. Datetimes come back as isoformat() text.

    as_local_time_at_offset(value, hours)   as_local_time(value, local_tz=timezone(hours))
"""

from datetime import datetime, timedelta, timezone, tzinfo

ZERO = timedelta(0)
HOUR = timedelta(hours=1)
NOW_UTC = datetime(2026, 3, 8, 7, 30, 0, 250000, tzinfo=timezone.utc)


def _first_sunday_on_or_after(dt):
    days_to_go = 6 - dt.weekday()
    if days_to_go:
        dt += timedelta(days_to_go)
    return dt


def _us_dst_range(year):
    if year < 2007:
        return (datetime(year, 1, 1),) * 2
    start = _first_sunday_on_or_after(datetime(year, 3, 8, 2))
    end = _first_sunday_on_or_after(datetime(year, 11, 1, 2))
    return start, end


class _NewYork(tzinfo):
    """America/New_York since 2007, from the datetime documentation's USTimeZone."""

    stdoffset = timedelta(hours=-5)

    def utcoffset(self, dt):
        return self.stdoffset + self.dst(dt)

    def dst(self, dt):
        if dt is None or dt.tzinfo is None:
            return ZERO
        start, end = _us_dst_range(dt.year)
        dt = dt.replace(tzinfo=None)
        if start + HOUR <= dt < end - HOUR:
            return HOUR
        if end - HOUR <= dt < end:
            return ZERO if dt.fold else HOUR
        if start <= dt < start + HOUR:
            return HOUR if dt.fold else ZERO
        return ZERO

    def fromutc(self, dt):
        start, end = _us_dst_range(dt.year)
        start = start.replace(tzinfo=self)
        end = end.replace(tzinfo=self)
        std_time = dt + self.stdoffset
        dst_time = std_time + HOUR
        if end <= dst_time < end + HOUR:
            return std_time.replace(fold=1)
        if std_time < start or dst_time >= end:
            return std_time
        return dst_time

    def tzname(self, dt):
        return "EDT" if self.dst(dt) else "EST"


NEW_YORK = _NewYork()


class _Pinned(datetime):
    @classmethod
    def now(cls, tz=None):
        if tz is None:
            return NOW_UTC.astimezone(NEW_YORK).replace(tzinfo=None)
        return NOW_UTC.astimezone(tz)

    def astimezone(self, tz=None):
        if tz is not None:
            return datetime.astimezone(self, tz)
        if self.tzinfo is None:
            moment = datetime.astimezone(self.replace(tzinfo=NEW_YORK), timezone.utc)
            return moment.astimezone(NEW_YORK)
        return datetime.astimezone(self, NEW_YORK)


def _pinned(module, fn, *args, **kwargs):
    saved = module.datetime
    module.datetime = _Pinned
    try:
        return fn(*args, **kwargs)
    finally:
        module.datetime = saved


def _iso(value):
    return None if value is None else value.isoformat()


def _utc_now_iso(module):
    return _pinned(module, module.utc_now_iso)


def _parse(module, value):
    return _iso(_pinned(module, module.parse_meeting_time, value))


def _as_local(module, value):
    return _iso(_pinned(module, module.as_local_time, value))


def _as_local_at_offset(module, value, hours):
    zone = timezone(timedelta(hours=hours))
    return _iso(_pinned(module, module.as_local_time, value, local_tz=zone))


def _plain(name):
    return lambda module, *args: _pinned(module, getattr(module, name), *args)


def _duration(module, meeting, running):
    return _pinned(module, module.meeting_duration_s, meeting, running=running)


_STAMPS = [
    "2026-08-20T16:55:00Z", "2026-08-20T16:55:00+00:00", "2026-08-20T16:55:00.123456Z",
    "2026-08-20T12:55:00-04:00", "2026-08-20T16:55:00+05:30", "2026-08-20T09:55:00",
    "2026-08-20T09:55:00.5", "2026-03-08T02:30:00", "2026-03-08T01:59:59", "2026-03-08T03:00:00",
    "2026-11-01T01:30:00", "2026-11-01T00:59:59", "2026-11-01T02:00:00",
    "2026-03-08T06:59:59Z", "2026-03-08T07:00:00Z", "2026-11-01T05:30:00Z",
    "2026-11-01T06:30:00Z", "2026-01-15T12:00:00Z", "2026-12-31T23:30:00-01:00",
    "2026-08-20", "20260820T165500Z", "2026-08-20 16:55", "2026-08-20T16:55:00ZZ",
    "Z", "garbage", "", None, 0, 5, 20260820, 1.5, True, False, [], ["2026-08-20T16:55:00Z"],
    {"at": "2026-08-20T16:55:00Z"}, "9999-12-31T23:00:00+00:00",
    "0001-01-01T03:00:00+00:00", "2026-08-20T00:00:00+23:59",
]

_PAIRS = [
    ["2026-08-20T16:55:00Z", "2026-08-20T16:55:40Z"],
    ["2026-08-20T09:55:00", "2026-08-20T09:55:40"],
    ["2026-08-20T09:55:00", "2026-08-20T13:56:00Z"],
    ["2026-08-20T13:55:00Z", "2026-08-20T09:56:00"],
    ["2026-03-08T02:30:00", "2026-03-08T07:30:00Z"],
    ["2026-03-08T01:30:00", "2026-03-08T03:30:00"],
    ["2026-11-01T01:30:00", "2026-11-01T05:30:00Z"],
    ["2026-11-01T01:30:00", "2026-11-01T06:30:00Z"],
    ["2026-08-20T16:55:00.000001Z", "2026-08-20T16:55:00Z"],
    ["2026-08-20T16:55:00+02:00", "2026-08-20T16:55:00-03:30"],
    ["2026-08-20T16:55:00Z", None], [None, "2026-08-20T16:55:00Z"], ["", ""],
    ["2026-08-20T16:55:00Z", "nope"], ["2025-01-01T00:00:00Z", "2026-08-20T16:55:00.5Z"],
]


def _meeting(started, ended, paused=None, **extra):
    row = {"started_at": started, "ended_at": ended}
    if paused is not None:
        row["paused_total_s"] = paused
    row.update(extra)
    return row


_MEETINGS = [
    _meeting("2026-08-20T16:55:00Z", "2026-08-20T16:55:42Z"),
    _meeting("2026-08-20T16:55:00Z", "2026-08-20T16:56:00Z"),
    _meeting("2026-08-20T16:55:00Z", "2026-08-20T17:54:59Z"),
    _meeting("2026-08-20T16:55:00Z", "2026-08-20T17:55:00Z"),
    _meeting("2026-08-20T16:55:00Z", "2026-08-20T18:00:30Z", 30),
    _meeting("2026-08-20T16:55:00Z", "2026-08-22T17:00:05Z", "45.5"),
    _meeting("2026-08-20T16:55:00Z", "2026-08-20T17:00:00Z", 600),
    _meeting("2026-08-20T16:55:00Z", "2026-08-20T17:00:00Z", "abc"),
    _meeting("2026-08-20T16:55:00Z", "2026-08-20T17:00:00Z", [1]),
    _meeting("2026-08-20T16:55:00Z", "2026-08-20T17:00:00Z", True),
    _meeting("2026-08-20T16:55:00Z", "2026-08-20T17:00:00Z", "nan"),
    _meeting("2026-08-20T16:55:00Z", "2026-08-20T17:00:00Z", -10),
    _meeting("2026-08-20T16:55:00Z", "2026-08-20T17:00:00Z", 0),
    _meeting("2026-08-20T09:55:00", "2026-08-20T10:30:00"),
    _meeting("2026-08-20T09:55:00", "2026-08-20T14:30:00Z"),
    _meeting("2026-03-08T06:00:00Z", None, 60),
    _meeting("2026-03-08T01:00:00", None),
    _meeting("2026-03-08T07:30:00.25Z", ""),
    _meeting(None, "2026-08-20T17:00:00Z"),
    _meeting("never", "2026-08-20T17:00:00Z"),
    _meeting("2026-08-20T16:55:00Z", "2026-08-20T16:54:00Z"),
    {},
    {"started_at": "2026-08-20T16:55:00Z"},
]

CASES = {
    "utc_now_iso": [[]],
    "parse_meeting_time": [[value] for value in _STAMPS],
    "as_local_time": [[value] for value in _STAMPS],
    "as_local_time_at_offset": [[value, hours] for value in _STAMPS[:22] for hours in (-7, 0, 9)],
    "elapsed_seconds": _PAIRS,
    "seconds_since": [[value] for value in _STAMPS],
    "format_meeting_started_at": [[value] for value in _STAMPS],
    "meeting_duration_s": [[meeting, running] for meeting in _MEETINGS for running in (False, True)],
    "format_meeting_duration": [[meeting] for meeting in _MEETINGS],
    "format_meeting_identity_meta": [[meeting] for meeting in _MEETINGS],
}

WRAPPERS = {
    "utc_now_iso": _utc_now_iso,
    "parse_meeting_time": _parse,
    "as_local_time": _as_local,
    "as_local_time_at_offset": _as_local_at_offset,
    "elapsed_seconds": _plain("elapsed_seconds"),
    "seconds_since": _plain("seconds_since"),
    "format_meeting_started_at": _plain("format_meeting_started_at"),
    "meeting_duration_s": _duration,
    "format_meeting_duration": _plain("format_meeting_duration"),
    "format_meeting_identity_meta": _plain("format_meeting_identity_meta"),
}

PINS = {
    "now_utc": NOW_UTC.isoformat(),
    "timezone_note": "the machine's zone is America/New_York while a case runs (US rules since "
                     "2007, fold=0); replay with a FakeClock at now_utc in that zone",
}
