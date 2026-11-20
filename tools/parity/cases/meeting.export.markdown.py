"""Curated inputs for meeting/export/markdown.py (M7.11 meeting-state-extras).

    export_markdown(meeting, state, segments, include_transcript, include_intelligence)
    demote_headings(markdown, offset)

The machine's zone is pinned to America/New_York while a case runs (meeting.time_utils's
`datetime` is swapped as in meeting.time_utils's own cases, and a naive datetime's timestamp()
reads it in that zone); replay with a FakeClock in that zone.
"""

import copy
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

    def timestamp(self):
        if self.tzinfo is None:
            return datetime.timestamp(self.replace(tzinfo=NEW_YORK))
        return datetime.timestamp(self)


def _export(module, meeting, state, segments, include_transcript, include_intelligence):
    import meeting.time_utils as time_utils

    saved = time_utils.datetime
    time_utils.datetime = _Pinned
    try:
        return module.export_markdown(meeting, state, segments,
                                      include_transcript=include_transcript,
                                      include_intelligence=include_intelligence)
    finally:
        time_utils.datetime = saved


MEETING = {"id": "m_export", "title": "Planning Sync", "status": "ended",
           "started_at": "2026-03-15T14:30:00", "ended_at": "2026-03-15T15:00:00",
           "paused_total_s": 0.0, "host_token": "SECRET_HOST", "guest_token": "SECRET_GUEST",
           "spool_dir": "/tmp/spool"}

STATE = {
    "meeting_id": "m_export", "seq": 9, "title": "Planning Sync", "status": "ended",
    "topic": {"current": "Q3 roadmap", "history": [
        {"text": "Kickoff", "ts": "2026-03-15T14:30:00"},
        {"text": "  ", "ts": "2026-03-15T14:35:00"},
        {"text": "Budget", "ts": "2026-07-04T18:05:00Z"},
        {"text": "No stamp"},
        {"text": "Q3 roadmap", "ts": "2026-03-15T14:45:00"}]},
    "rolling_summary": "  Team aligned on milestones.  ",
    "cards": {
        "key_points": [
            {"id": "it_1", "card": "key_points", "text": "Ship beta in June",
             "status": "proposed", "data": {}, "pinned": False, "evidence": ["sg_2", "sg_1"]},
            {"id": "it_1b", "card": "key_points", "text": " Unanchored ", "status": "proposed",
             "evidence": ["sg_missing", "sg_bad"]},
            {"id": "it_1c", "card": "key_points", "text": "", "status": "proposed"}],
        "decisions": [
            {"id": "it_2", "card": "decisions", "text": "Use FastAPI", "status": "confirmed",
             "data": {}, "pinned": True, "evidence": [], "review": {"state": "unsupported"},
             "revision": 2, "citation_check": {"status": "supported", "revision": 2}},
            {"id": "it_2b", "card": "decisions", "text": "Spoken choice", "status": "proposed",
             "author_type": "system", "author_id": "voice_command",
             "review": {"state": "provisional"}, "revision": 1,
             "citation_check": {"status": "nonsense", "revision": 1}}],
        "action_items": [
            {"id": "it_3", "card": "action_items", "text": "Draft RFC", "status": "proposed",
             "data": {"owner_participant_id": "p_me", "deadline": " Friday "}},
            {"id": "it_3b", "card": "action_items", "text": "Book venue", "status": "edited",
             "data": {"owner_participant_id": "p_2", "due_date": "Monday"}},
            {"id": "it_3c", "card": "action_items", "text": "Mystery", "status": "proposed",
             "data": {"owner_participant_id": "p_blank"}},
            {"id": "it_3d", "card": "action_items", "text": "Orphan", "status": "proposed",
             "data": {"owner_participant_id": 7, "deadline": "   "}},
            {"id": "it_3e", "card": "action_items", "text": "Nobody", "status": "proposed",
             "data": None}],
        "risks": [
            {"id": "it_4", "card": "risks", "text": "Vendor delay", "status": "proposed",
             "data": {"severity": "high"}},
            {"id": "it_4b", "card": "risks", "text": "Numbers", "status": "proposed",
             "data": {"severity": 3}},
            {"id": "it_4c", "card": "risks", "text": "Plain", "status": "proposed"}],
        "timeline": [
            {"id": "it_5", "card": "timeline", "text": "Kickoff done", "status": "proposed",
             "data": {"start_s": 90}},
            {"id": "it_5b", "card": "timeline", "text": "Flagged", "status": "proposed",
             "data": {"start_s": True}},
            {"id": "it_5c", "card": "timeline", "text": "Worded", "status": "proposed",
             "data": {"start_s": "12"}},
            {"id": "it_5d", "card": "timeline", "text": "Long", "status": "proposed",
             "data": {"start_s": 3725.7}}],
        "live_notes": [
            {"id": "n_late", "card": "live_notes", "text": "Late   block\nwith  lines",
             "status": "proposed", "data": {"heading": "Wrap", "start_s": 300.0},
             "created_at": "2026-03-15T14:40:00Z"},
            {"id": "n_untimed", "card": "live_notes", "text": "No stamp", "status": "proposed",
             "data": {"heading": " Aside "}, "created_at": "2026-03-15T14:31:00Z"},
            {"id": "n_untimed_early", "card": "live_notes", "text": "Earlier untimed",
             "status": "proposed", "data": {}, "created_at": "2026-03-15T14:30:30Z"},
            {"id": "n_naive", "card": "live_notes", "text": "Naive stamp", "status": "proposed",
             "data": {}, "created_at": "2026-03-15T10:30:45"},
            {"id": "n_bad", "card": "live_notes", "text": "Bad stamp", "status": "proposed",
             "data": {"start_s": False}, "created_at": "never"},
            {"id": "n_first", "card": "live_notes", "text": "Opening", "status": "proposed",
             "data": {"start_s": 5}},
            {"id": "n_spoken", "card": "live_notes", "text": "Spoken", "status": "proposed",
             "author_type": "system", "author_id": "voice_command", "data": {"start_s": 6.0}}],
        "user_notes": [
            {"id": "it_gone", "card": "user_notes", "text": "scratch", "status": "removed"},
            {"id": "it_note", "card": "user_notes", "text": "A host note",
             "status": "edited", "review": {"state": "human"}}],
    },
    "participants": {
        "p_2": {"id": "p_2", "display_name": "Sam", "kind": "guest"},
        "p_blank": {"id": "p_blank", "display_name": "  ", "kind": "others_cluster"},
        "p_me": {"id": "p_me", "display_name": "Alex", "kind": "me"},
    },
    "questions": [
        {"id": "q_1", "text": "Deadline firm?", "status": "open",
         "suggested_answer": "Yes, June 30", "suggested_confidence": 0.6},
        {"id": "q_1b", "text": "Rounded?", "status": "open", "suggested_answer": "Half",
         "suggested_confidence": 0.125},
        {"id": "q_1c", "text": "Whole?", "status": "open", "suggested_answer": "One",
         "suggested_confidence": 1},
        {"id": "q_1d", "text": "Flag?", "status": "open", "suggested_answer": "Bool",
         "suggested_confidence": True},
        {"id": "q_1e", "text": "Text?", "status": "open", "suggested_answer": "Str",
         "suggested_confidence": "0.5"},
        {"id": "q_1f", "text": "No suggestion", "status": "open", "suggested_answer": "  "},
        {"id": "q_2", "text": "Who owns RFC?", "status": "resolved", "answer": "Alex",
         "answer_source": "audio", "resolved_by": "p_me"},
        {"id": "q_2b", "text": "Who books?", "status": "resolved", "answer": "Sam",
         "answer_source": "user", "resolved_by": "p_2"},
        {"id": "q_2c", "text": "Who else?", "status": "resolved", "answer": "Someone",
         "resolved_by": None},
        {"id": "q_2d", "text": "Unanswered?", "status": "resolved", "answer": ""},
        {"id": "q_3", "text": "Ignored", "status": "dismissed"},
        {"id": "q_4", "text": "   ", "status": "open"}],
    "insight_review": {"questions": [
        {"correction": "The RFC is due Friday.", "status": "answered"},
        {"correction": "OUTDATED", "superseded": True},
        {"correction": "", "status": "answered"},
        {"status": "open"}]},
    "live_highlights": [
        {"id": "late", "kind": "number", "start_s": 90, "text": "Budget is 5000"},
        {"id": "lesson", "kind": "takeaway", "start_s": 45.9, "text": " Feedback "},
        {"id": "early", "kind": "decision", "start_s": 5, "text": "Choose June"},
        {"id": "odd", "kind": "gossip", "start_s": 6, "text": "Not a pulse"},
        {"id": "flag", "kind": "decision", "start_s": True, "text": "Bool start"},
        {"id": "word", "kind": "decision", "start_s": "7", "text": "String start"},
        {"id": "blank", "kind": "commitment", "start_s": 8, "text": "  "},
        {"id": "dup", "kind": "commitment", "start_s": 5, "text": "Same start, later"}],
    "intent": {"text": "  Decide the vendor.\nCapture objections.   \n\n  Keep it short  "},
    "custom_reports": [
        {"id": "rep_1", "request": "  the   commitments ", "status": "ready",
         "title": "Commitments",
         "markdown": "\n# Commitments\n\n## Owners\n\n```\n# code\n```\n- Priya by Friday"},
        {"id": "rep_2", "request": "", "status": "ready", "title": "",
         "markdown": "# Different\n\n#Not a heading\n###### Deep"},
        {"id": "rep_3", "request": "Second ask", "status": "ready", "title": " ",
         "markdown": "Plain body"},
        {"id": "rep_4", "request": "Failed ask", "status": "failed", "markdown": "partial"},
        {"id": "rep_5", "request": "Empty", "status": "ready", "markdown": "  "},
        "not a dict"],
}

SEGMENTS = [
    {"id": "sg_1", "start_s": 5.0, "end_s": 8.0, "text": "Welcome everyone", "channel": "mic",
     "speaker_participant_id": "p_me"},
    {"id": "sg_2", "start_s": 10.0, "end_s": 14.0, "text": " Let's plan Q3 ",
     "channel": "loopback", "speaker_participant_id": "p_2"},
    {"id": "sg_bad", "start_s": "soon", "end_s": 15, "text": "", "channel": "mic"},
    {"id": "sg_3", "start_s": 3725, "end_s": 3730.5, "text": "Late words", "channel": "mic",
     "speaker_participant_id": "p_blank"},
    {"id": "sg_4", "start_s": None, "end_s": None, "text": "Unknown speaker",
     "channel": "loopback", "speaker_participant_id": "p_ghost"},
]

_MINIMAL_STATES = [
    {},
    {"participants": {}, "cards": {}, "questions": []},
    {"title": "From the state", "topic": {"current": "Only a topic", "history": []}},
]

_MEETINGS = [
    {},
    {"title": "  ", "started_at": "2026-09-20T01:30:00+00:00",
     "ended_at": "2026-09-20T02:00:05Z", "paused_total_s": 65},
    {"started_at": "not a date"},
    {"started_at": "2026-09-19T09:00:00", "ended_at": "2026-09-19T13:30:00+00:00"},
    {"title": "Weekly", "started_at": None, "status": "ended"},
]

_ZERO_SEGMENTS = [{"id": "z", "start_s": 0, "end_s": 0, "text": "Zero"}]


def _recent():
    state = copy.deepcopy(STATE)
    state["cards"] = {"action_items": state["cards"]["action_items"]}
    return state


CASES = {
    "export_markdown": (
        [[MEETING, STATE, SEGMENTS, t, i] for t in (True, False) for i in (True, False)]
        + [[m, s, [], True, True] for m in _MEETINGS for s in _MINIMAL_STATES]
        + [[_MEETINGS[1], _MINIMAL_STATES[0], _ZERO_SEGMENTS, True, True],
           [MEETING, _recent(), SEGMENTS, True, True]]
    ),
    "demote_headings": [
        ["# a\n\n```\n# not a heading\n```\n\n###### deep", 3],
        ["# a\n## b\n### c\n#### d", 3],
        ["#nospace\n  # indented\n#\ttab\n####### seven\n#", 3],
        ["~~~\n# in tilde\n```\n# still in\n~~~\n# out", 3],
        ["  ```python\n# comment\n  ```\n# Heading", 2],
        ["# a\r\n## b\rlast\n", 0],
        ["# a", 5],
        ["", 3],
        ["## 　wide space", 1],
    ],
}

WRAPPERS = {"export_markdown": _export}

PINS = {
    "now_utc": NOW_UTC.isoformat(),
    "timezone_note": "the machine's zone is America/New_York while a case runs (US rules since "
                     "2007, fold=0); replay with a FakeClock in that zone",
}
