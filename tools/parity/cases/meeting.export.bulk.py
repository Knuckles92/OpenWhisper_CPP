"""Curated inputs for meeting/export/bulk.py (M7.11 meeting-state-extras).

    meeting_file_stem(meeting, state)
    filter_export_meetings(meetings, from_iso, to_iso, only_with_transcript)
        from_dt and to_dt are datetime.fromisoformat of the text (None: unbounded); the result
        is the ids kept, in order.
    render_meeting_document(entry, fmt, include_transcript, include_intelligence)
    render_export_document(entries, fmt, include_transcript, include_intelligence)
    fixture_export(meeting_id, fmt, include_transcript, include_intelligence)
        collect_meeting_export over a scratch copy of tests/fixtures/meeting_state/
        openwhisper-meetings.db, then render_meeting_document (None for an unknown id).

The machine's zone is pinned to America/New_York and now to 2026-01-01T00:00:00Z while a case
runs (meeting.time_utils's and bulk's `datetime` are swapped as in meeting.time_utils's own
cases); replay with a FakeClock at now_utc in that zone.
"""

import shutil
from datetime import datetime, timedelta, timezone, tzinfo
from pathlib import Path

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

    def timestamp(self):
        if self.tzinfo is None:
            return datetime.timestamp(self.replace(tzinfo=NEW_YORK))
        return datetime.timestamp(self)


def _pinned(module, fn, *args, **kwargs):
    import meeting.time_utils as time_utils

    saved = time_utils.datetime, module.datetime
    time_utils.datetime = module.datetime = _Pinned
    try:
        return fn(*args, **kwargs)
    finally:
        time_utils.datetime, module.datetime = saved


def _stem(module, meeting, state):
    return _pinned(module, module.meeting_file_stem, meeting, state)


def _filter(module, meetings, from_iso, to_iso, only_with_transcript):
    from_dt = None if from_iso is None else datetime.fromisoformat(from_iso)
    to_dt = None if to_iso is None else datetime.fromisoformat(to_iso)
    kept = _pinned(module, module.filter_export_meetings, meetings, from_dt=from_dt, to_dt=to_dt,
                   only_with_transcript=only_with_transcript)
    return [meeting.get("id") for meeting in kept]


def _render_one(module, entry, fmt, include_transcript, include_intelligence):
    return _pinned(module, module.render_meeting_document, entry, fmt,
                   include_transcript=include_transcript,
                   include_intelligence=include_intelligence)


def _render_all(module, entries, fmt, include_transcript, include_intelligence):
    return _pinned(module, module.render_export_document, entries, fmt,
                   include_transcript=include_transcript,
                   include_intelligence=include_intelligence)


_FIXTURE = (Path(__file__).resolve().parents[3] / "tests" / "fixtures" / "meeting_state"
            / "openwhisper-meetings.db")
_FIXTURE_REPOSITORY = []


def _fixture_repository():
    """OpenWhisper's repository over a scratch copy of M7.7's fixture database."""
    if not _FIXTURE_REPOSITORY:
        import ow_isolation
        from meeting.persist.repository import SqlMeetingRepository
        from services.database import DatabaseManager

        work = ow_isolation.SCRATCH / "meeting-export-fixture"
        shutil.rmtree(work, ignore_errors=True)
        work.mkdir(parents=True)
        copy = work / _FIXTURE.name
        shutil.copyfile(_FIXTURE, copy)
        _FIXTURE_REPOSITORY.append(SqlMeetingRepository(DatabaseManager(db_path=str(copy))))
    return _FIXTURE_REPOSITORY[0]


def _fixture_export(module, meeting_id, fmt, include_transcript, include_intelligence):
    """collect_meeting_export of a fixture meeting, rendered as the export dialog renders it."""
    entry = module.collect_meeting_export(_fixture_repository(), meeting_id)
    if entry is None:
        return None
    return _render_one(module, entry, fmt, include_transcript, include_intelligence)


def _row(meeting_id, status="ended", started="2026-03-15T14:30:00", summary=None, **extra):
    row = {"id": meeting_id, "title": "Planning", "status": status, "started_at": started}
    if summary is not None:
        row["content_summary"] = summary
    row.update(extra)
    return row


_ROWS = [
    _row("m_old", started="2026-01-01T10:00:00"),
    _row("m_mid", started="2026-03-15T14:30:00", summary={"has_transcript": True}),
    _row("m_utc_edge", started="2026-04-01T02:30:00Z"),
    _row("m_new", started="2026-06-01T09:00:00"),
    _row("m_silent", started="2026-03-20T09:00:00", summary={"has_transcript": False}),
    _row("m_falsy", started="2026-03-21T09:00:00", summary={"has_transcript": 0}),
    _row("m_unknown", started="2026-03-22T09:00:00", summary="not a dict"),
    _row("m_live", status="active"),
    _row("m_paused", status="PAUSED"),
    _row("m_ending", status="ending"),
    _row("m_none", status=None),
    _row("m_undated", started=""),
    _row("m_bad", started="never"),
]


def _entry(meeting_id, title, started="2026-03-15T14:30:00", text="Hello", **state):
    doc = {"title": title, "participants": {}, "cards": {}}
    doc.update(state)
    return {
        "meeting": {"id": meeting_id, "title": title, "started_at": started,
                    "ended_at": "2026-03-15T15:00:00", "host_token": "secret"},
        "state": doc,
        "segments": [{"id": "sg_1", "start_s": 1.0, "end_s": 2.0, "text": text,
                      "channel": "mic"}],
    }


_DECISION = {"decisions": [{"id": "it_1", "card": "decisions", "text": "Ship it",
                            "status": "confirmed", "data": {}, "pinned": False,
                            "evidence": []}]}

_ENTRIES = [
    _entry("m_1", "Planning", cards=_DECISION, rolling_summary="Aligned."),
    _entry("m_2", "Retro", started="2026-07-04T18:05:00Z", text="Café ☕ talk"),
    _entry("m_3", "", started=None, text="  "),
]

_STEM_MEETINGS = [
    [{"title": "Q3 / Roadmap?", "started_at": "2026-03-15T14:30:00"}, {"title": "Q3 / Roadmap?"}],
    [{"title": "  Spaced   out\ttitle  "}, {}],
    [{"title": "", "started_at": "2026-11-01T05:30:00Z"}, {"title": ""}],
    [{"title": "--__..Dots and dashes..__--", "started_at": "2026-03-08T07:30:00Z"}, {}],
    [{"title": "Résumé & naïve: ü/ß 😀"}, {}],
    [{"title": "x" * 70 + " tail"}, {}],
    [{"title": "a" * 59 + "-.b"}, {}],
    [{"started_at": "garbage"}, {"title": "From state"}],
    [{}, {}],
    [{"title": "!!!", "started_at": "2026-03-15T14:30:00.999999"}, {}],
]

CASES = {
    "meeting_file_stem": _STEM_MEETINGS,
    "filter_export_meetings": [
        [_ROWS, None, None, False],
        [_ROWS, None, None, True],
        [_ROWS, "2026-03-01T00:00:00", "2026-03-31T23:59:59", True],
        [_ROWS, "2026-03-15T14:30:00", None, False],
        [_ROWS, None, "2026-03-31T22:30:00", False],
        [_ROWS, None, "2026-03-31T22:29:59", False],
        [_ROWS, "2026-03-15T14:30:00.000001", "2026-12-31T00:00:00", False],
    ],
    "render_meeting_document": [
        [_ENTRIES[0], fmt, t, i] for fmt in ("markdown", "txt", "json", "md")
        for t, i in ((True, True), (False, False))
    ] + [[_ENTRIES[1], "markdown", True, True], [_ENTRIES[2], "txt", True, True]],
    "render_export_document": [
        [_ENTRIES, fmt, t, i] for fmt in ("markdown", "txt", "json")
        for t, i in ((True, True), (False, True))
    ] + [[[], "markdown", True, True], [[], "txt", True, True], [[], "json", True, True]],
    # The acceptance: every meeting of tests/fixtures/meeting_state/openwhisper-meetings.db (an
    # ended one, a paired computer's, an interrupted one) in every format, byte for byte.
    "fixture_export": [
        [meeting_id, fmt, t, i]
        for meeting_id in ("m_fixture_ended", "m_fixture_paired", "m_fixture_interrupted")
        for fmt in ("markdown", "txt", "json")
        for t, i in ((True, True), (False, False))
        if fmt == "markdown" or (t, i) == (True, True)
    ] + [["m_fixture_missing", "markdown", True, True]],
}

WRAPPERS = {
    "meeting_file_stem": _stem,
    "filter_export_meetings": _filter,
    "render_meeting_document": _render_one,
    "render_export_document": _render_all,
    "fixture_export": _fixture_export,
}

PINS = {
    "now_utc": NOW_UTC.isoformat(),
    "timezone_note": "the machine's zone is America/New_York while a case runs (US rules since "
                     "2007, fold=0); replay with a FakeClock at now_utc in that zone",
}
