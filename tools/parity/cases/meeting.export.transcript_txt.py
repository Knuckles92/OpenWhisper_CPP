"""Curated inputs for meeting/export/transcript_txt.py (M7.11 meeting-state-extras).

    format_clock(seconds), format_mmss(seconds)
    resolve_title(meeting, state), format_meeting_date(meeting)
    participant_names(state), speaker_name(segment, participants)
    transcript_lines(segments, participants), export_transcript_txt(meeting, state, segments)

The machine's zone is pinned to America/New_York while a case runs (meeting.time_utils's
`datetime` is swapped as in meeting.time_utils's own cases); replay with a FakeClock in that
zone.
"""

from datetime import datetime, timedelta, timezone, tzinfo

ZERO = timedelta(0)
HOUR = timedelta(hours=1)
NOW_UTC = datetime(2026, 1, 1, tzinfo=timezone.utc)


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


def _pinned(name):
    def call(module, *args):
        import meeting.time_utils as time_utils

        saved = time_utils.datetime
        time_utils.datetime = _Pinned
        try:
            return getattr(module, name)(*args)
        finally:
            time_utils.datetime = saved

    return call


_PARTICIPANTS = {
    "p_2": {"id": "p_2", "display_name": "Sam", "kind": "guest"},
    "p_blank": {"id": "p_blank", "display_name": "  ", "kind": "others_cluster"},
    "p_none": {"id": "p_none", "display_name": None, "kind": "me"},
    "p_me": {"id": "p_me", "display_name": " Alex ", "kind": "me"},
    "p_3": {"id": "p_3", "display_name": "Speaker 3", "kind": "others_cluster"},
}

_SEGMENTS = [
    {"id": "sg_1", "start_s": 5.0, "end_s": 8.0, "text": " Welcome everyone ", "channel": "mic",
     "speaker_participant_id": "p_me"},
    {"id": "sg_2", "start_s": 10.4, "text": "Let's plan Q3", "channel": "loopback",
     "speaker_participant_id": "p_2"},
    {"id": "sg_3", "start_s": 3725.99, "text": "Late", "channel": "mic",
     "speaker_participant_id": "p_blank"},
    {"id": "sg_4", "start_s": None, "text": "No start", "channel": "loopback"},
    {"id": "sg_5", "start_s": "12.5", "text": "String start", "speaker_participant_id": ""},
    {"id": "sg_6", "start_s": 7, "text": "   "},
    {"id": "sg_7", "start_s": 360000, "text": "A hundred hours", "channel": "mic",
     "speaker_participant_id": "p_unknown"},
    {"id": "sg_8", "start_s": -3, "text": "Negative", "channel": "other"},
]

_MEETINGS = [
    {},
    {"title": "Planning Sync", "started_at": "2026-03-15T14:30:00"},
    {"title": "  ", "started_at": "2026-09-20T01:30:00+00:00"},
    {"title": None, "started_at": "2026-03-08T07:30:00Z"},
    {"started_at": "not a date"},
    {"started_at": 20260820},
    {"started_at": ""},
    {"title": " Kept ", "started_at": "2026-11-01T05:30:00Z"},
]

_STATES = [{}, {"title": " State title "}, {"title": "", "participants": _PARTICIPANTS},
           {"participants": None}]

CASES = {
    "format_clock": [[s] for s in (0, 5.0, 59.999, 60, 3599, 3600, 3725.9, 86399, 360000, -5,
                                   -0.5, 1e10, True)],
    "format_mmss": [[s] for s in (0, 5.9, 60, 3599, 3600, 3725.9, -1, 360000)],
    "resolve_title": [[m, s] for m in _MEETINGS for s in _STATES[:2]],
    "format_meeting_date": [[m] for m in _MEETINGS],
    "participant_names": [[s] for s in _STATES] + [[{"participants": {}}]],
    "speaker_name": [[seg, _PARTICIPANTS] for seg in _SEGMENTS] + [[{"channel": "mic"}, {}]],
    "transcript_lines": [[_SEGMENTS, _PARTICIPANTS], [[], {}], [_SEGMENTS[:2], {}]],
    "export_transcript_txt": [[m, s, _SEGMENTS] for m in _MEETINGS[:4] for s in _STATES[1:3]]
    + [[_MEETINGS[0], {}, []], [_MEETINGS[4], {}, []]],
}

WRAPPERS = {name: _pinned(name) for name in ("resolve_title", "format_meeting_date",
                                              "export_transcript_txt")}

PINS = {
    "now_utc": NOW_UTC.isoformat(),
    "timezone_note": "the machine's zone is America/New_York while a case runs (US rules since "
                     "2007, fold=0); replay with a FakeClock in that zone",
}
