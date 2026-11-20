"""Curated inputs for meeting/recall.py (M8.3 recall-and-knowledge-folder).

    _date_label, _title, _start_s, _clip, _sanitize, _clamp_limit   direct
    _format_hits(rows, limit, current_meeting_id)                    keyword arguments as Python
                                                                     passes them
    _slice_meeting(meetings, segments, meeting_id, current, limit)   over a stub repository:
                                                                     {id: meeting row} and
                                                                     {id: [segment rows]}
    search_past_meetings(enabled, rows, meetings, segments, query, current_meeting_id,
                         meeting_id, limit)
                                     recall switched by `enabled`; rows null stands for no
                                     repository; the stub answers search_transcripts with `rows`.
                                     Semantic search is off (the scratch settings' default), so
                                     the search is the keyword path.

The C++ replay is tests/meeting_intel/test_golden.cpp.
"""

from unittest.mock import patch


class _Repo:
    def __init__(self, rows, meetings, segments):
        self.rows = list(rows)
        self.meetings = dict(meetings)
        self.segments = dict(segments)

    def search_transcripts(self, query, *, exclude_meeting_id=None, limit=200):
        return list(self.rows)

    def get_meeting(self, meeting_id):
        row = self.meetings.get(meeting_id)
        return dict(row) if row else None

    def get_segments(self, meeting_id, after_start_s=-1.0, limit=None):
        rows = list(self.segments.get(meeting_id, []))
        return rows[:limit] if limit else rows


def _format_hits(module, rows, limit, current_meeting_id):
    return module._format_hits(rows, limit=limit, current_meeting_id=current_meeting_id)


def _slice_meeting(module, meetings, segments, meeting_id, current_meeting_id, limit):
    return module._slice_meeting(_Repo([], meetings, segments), meeting_id=meeting_id,
                                 current_meeting_id=current_meeting_id, limit=limit)


def _search(module, enabled, rows, meetings, segments, query, current_meeting_id, meeting_id,
            limit):
    repo = None if rows is None else _Repo(rows, meetings, segments)
    with patch("services.settings.resolve_meeting_past_recall_enabled", return_value=enabled):
        return module.search_past_meetings(repo, query=query,
                                           current_meeting_id=current_meeting_id,
                                           meeting_id=meeting_id, limit=limit)


def _hit(meeting_id="m_past", text="we adopted the budget", title="Q1 review",
         started_at="2026-03-01T10:00:00", start_s=12.0, snippet=None, segment_id="sg_abc123def"):
    return {"segment_id": segment_id, "meeting_id": meeting_id, "text": text,
            "snippet": text if snippet is None else snippet, "title": title,
            "started_at": started_at, "start_s": start_s}


LONG = "budget " + "word " * 80

ROWS = [
    _hit("m_live", "live budget talk"),
    _hit("m_past", "we adopted the budget", segment_id="sg_deadbeef9999"),
    _hit("m_past", "budget again", start_s=73.25),
    _hit("m_other", "see sg_abcdef123456 from last week", title="sg_abcdef123456 sync"),
    _hit("", "no meeting id"),
    _hit("m_blank", "", snippet=""),
    _hit("m_snip", "full text", snippet="[budget] … snippet"),
    _hit("m_untitled", "untitled one", title=None, started_at=None, start_s=None),
    _hit("m_odd", "odd date", started_at="March 1st", start_s="7.5"),
    _hit("m_short", "short date", started_at="2026-3-1", start_s=0.000012),
    _hit("m_big", "big time", start_s=1234567.0),
]

MEETINGS = {
    "m_past": {"id": "m_past", "title": "Prior sync", "started_at": "2026-02-02T09:00:00"},
    "m_empty": {"id": "m_empty", "title": "", "started_at": ""},
    "m_live": {"id": "m_live", "title": "Now"},
    "m_falsy": {},
}
SEGMENTS = {
    "m_past": [
        {"id": "sg_aaa111222333", "text": "Ship Friday", "start_s": 3.5},
        {"id": "sg_bbb111222333", "text": "Agreed", "start_s": 8.0},
        {"id": "sg_ccc111222333", "text": "", "start_s": 9.0},
        {"id": "sg_ddd111222333", "text": "see sg_abcdef1234 again", "start_s": "11"},
        {"id": "sg_eee111222333", "text": LONG, "start_s": 12.0},
    ],
    "m_empty": [{"id": "sg_x", "text": "   ", "start_s": 1.0}],
}
MANY = {"m_many": {"id": "m_many", "title": "Long", "started_at": "2026-01-01T00:00:00"}}
MANY_SEGMENTS = {"m_many": [{"text": LONG + str(index), "start_s": index * 10.0}
                            for index in range(60)]}

CASES = {
    "_date_label": [[v] for v in (
        "2026-03-01T10:00:00", "2026-03-01", "2026-3-1", "March 1st", "", "   ", None,
        "  2026-03-01T10:00:00  ", "20260301T1000", 5, "abcd-efghij")],
    "_title": [[v] for v in ("Q1 review", "", "   ", None, "sg_abcdef123456", "  Spaced  ",
                             7, "Review sg_00aa11bb")],
    "_start_s": [[v] for v in (12.0, 12, "7.5", "abc", None, True, 0.000012, 1e16, "nan")],
    "_clip": [[t, m] for t in ("", "  padded  ", "abcdef", LONG, "sg_abcdef123456 x", None, 3)
              for m in (5, 6, 180)],
    "_sanitize": [[t] for t in ("sg_abcdef", "sg_abcde", "x sg_ABCDEF123 y", "")],
    "_clamp_limit": [[v] for v in (10, 0, -5, 20, 21, 99, "7", " 7 ", "abc", None, 3.9, True,
                                   10 ** 30, -(10 ** 30), [], {})],
    "_format_hits": [
        [ROWS, 10, "m_live"], [ROWS, 2, "m_live"], [ROWS, 20, ""], [[], 10, "m_live"],
        [[_hit(f"m_{i}", LONG, title=f"Meet {i}") for i in range(30)], 20, "m_live"],
    ],
    "_slice_meeting": [
        [MEETINGS, SEGMENTS, "m_past", "m_live", 10],
        [MEETINGS, SEGMENTS, "m_past", "m_live", 2],
        [MEETINGS, SEGMENTS, "m_live", "m_live", 10],
        [MEETINGS, SEGMENTS, "m_unknown", "m_live", 10],
        [MEETINGS, SEGMENTS, "m_falsy", "m_live", 10],
        [MEETINGS, SEGMENTS, "m_empty", "m_live", 10],
        [MANY, MANY_SEGMENTS, "m_many", "m_live", 20],
    ],
    "search_past_meetings": [
        [False, ROWS, {}, {}, "budget", "m_live", None, 10],
        [True, None, {}, {}, "budget", "m_live", None, 10],
        [True, ROWS, {}, {}, "   ", "m_live", None, 10],
        [True, ROWS, {}, {}, "budget", "m_live", None, 10],
        [True, ROWS, {}, {}, "budget", "  m_live  ", None, "3"],
        [True, ROWS, MEETINGS, SEGMENTS, "", "m_live", "m_live", 10],
        [True, ROWS, MEETINGS, SEGMENTS, "", "m_live", "  m_past ", 10],
        [True, ROWS, MEETINGS, SEGMENTS, "", "m_live", "m_unknown", 10],
        [True, ROWS, MEETINGS, SEGMENTS, "ignored", "m_live", "m_past", None],
    ],
}

WRAPPERS = {
    "_format_hits": _format_hits,
    "_slice_meeting": _slice_meeting,
    "search_past_meetings": _search,
}

PINS = {
    "repository": "a stub repository (search_transcripts returns the case's rows; get_meeting "
                  "and get_segments read its dicts)",
    "semantic": "semantic history search is off in the scratch settings, so recall's search "
                "is the keyword path",
}
