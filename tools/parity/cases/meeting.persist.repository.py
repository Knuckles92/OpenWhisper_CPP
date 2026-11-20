"""Curated inputs for meeting/persist/repository.py (M7.7 meeting-state-store).

New transcript rows are given as dicts of TranscriptSegment fields; old rows as the dicts
_old_segment_items makes. id maps go in and come out as [[old, new], ...] (dict order).

    interval_iou(a_start, a_end, b_start, b_end)
    match_live_to_offline(old_items, new_items)   {"id_map", "used_old", "new"}: the pairing and
                                                  each new row's speaker fields afterwards
    remap_state_evidence(state, id_map, old_items, new_items, retained_ids)   the state after
    content_summary(audio_count, has_loopback, segment_count, preview)
    search_filter(needle, meetings)               ids _search_filter keeps, sorted, over an
                                                  in-memory database of the given meetings
"""

import copy


def _segments(module, items):
    from meeting.interfaces import TranscriptSegment

    return [TranscriptSegment(**item) for item in items]


def _match(module, old_items, new_items):
    segments = _segments(module, new_items)
    id_map, used_old = module._match_live_to_offline(copy.deepcopy(old_items), segments)
    return {
        "id_map": [[old, new] for old, new in id_map.items()],
        "used_old": sorted(used_old),
        "new": [{"segment_id": seg.segment_id,
                 "speaker_participant_id": seg.speaker_participant_id,
                 "speaker_source": seg.speaker_source,
                 "speaker_pinned": seg.speaker_pinned} for seg in segments],
    }


def _remap(module, state, id_map, old_items, new_items, retained_ids):
    state = copy.deepcopy(state)
    module._remap_state_evidence(state, dict(id_map), old_items, _segments(module, new_items),
                                 frozenset(retained_ids))
    return state


def _search(module, needle, meetings):
    from sqlalchemy import create_engine
    from sqlalchemy.orm import Session
    from services.models import Base, MeetingSegment, MeetingSession

    engine = create_engine("sqlite://")
    Base.metadata.create_all(engine)
    try:
        with Session(engine) as session:
            for meeting in meetings:
                session.add(MeetingSession(
                    id=meeting["id"], title=meeting.get("title", ""),
                    status=meeting.get("status", "ended"),
                    started_at=meeting.get("started_at", "2026-01-01T09:00:00+00:00"),
                    host_token="h", guest_token="g", spool_dir="spool",
                    state_json=meeting.get("state_json")))
                for index, text in enumerate(meeting.get("segments", [])):
                    session.add(MeetingSegment(
                        id=f"sg_{meeting['id']}_{index}", meeting_id=meeting["id"], channel="mic",
                        start_s=float(index), end_s=float(index) + 1.0, text=text,
                        created_at="2026-01-01T09:00:00Z"))
            session.flush()
            kept = module._search_filter(session, session.query(MeetingSession), needle).all()
            return sorted(row.id for row in kept)
    finally:
        engine.dispose()


def _seg(segment_id, start, end, channel="mic", speaker=None, source="channel", pinned=False,
         meeting="m_1"):
    return {"segment_id": segment_id, "meeting_id": meeting, "chunk_id": None,
            "channel": channel, "start_s": start, "end_s": end, "text": segment_id,
            "speaker_participant_id": speaker, "speaker_source": source,
            "speaker_pinned": pinned}


def _old(segment_id, start, end, channel="mic", speaker=None, source="channel", pinned=False):
    return {"id": segment_id, "start_s": start, "end_s": end, "pinned": pinned,
            "speaker_participant_id": speaker, "speaker_source": source, "channel": channel}


_OLD = [
    _old("sg_old1", 0.0, 2.0, pinned=True, speaker="p_me", source="human"),
    _old("sg_old2", 2.0, 4.0),
    _old("sg_old3", 4.0, 6.0, channel="loopback", speaker="p_live", source="diarizer"),
    _old("sg_old4", 6.0, 8.0, channel="loopback", speaker="p_live", source="diarizer"),
    _old("sg_pin", 50.0, 52.0, pinned=True, speaker="p_me", source="human"),
]
_NEW = [
    _seg("sg_new1", 0.1, 2.1),
    _seg("sg_new2", 2.1, 4.1),
    _seg("sg_new3", 4.0, 6.0, channel="loopback", speaker="p_offline", source="diarizer"),
    _seg("sg_new4", 6.0, 8.0, channel="loopback"),
    _seg("sg_new5", 3.0, 5.0),
]

_STATE = {
    "meeting_id": "m_1",
    "cards": {
        "key_points": [
            {"id": "it_human", "text": "kept", "status": "edited", "pinned": False,
             "evidence": ["sg_old1", "sg_gone", "sg_old1", 7, "not_sg"],
             "citation_check": {"ok": True}},
            {"id": "it_agent", "text": "stale", "status": "proposed", "evidence": ["sg_old2"]},
            "junk",
        ],
        "decisions": [{"id": "it_pin", "evidence": ["sg_pin"]}],
        "risks": "not a list",
        "user_notes": [], "action_items": [], "timeline": [],
        "unknown_card": [{"id": "it_x", "evidence": ["sg_old1"]}],
    },
    "questions": [
        {"id": "q_1", "evidence": ["sg_old3", "sg_old4"]},
        "junk",
        {"id": "q_2"},
    ],
    "live_highlights": [
        {"label": "Decision", "segment_id": "sg_old1"},
        {"label": "Risk", "segment_id": "sg_gone"},
        {"label": "Other", "segment_id": None},
    ],
    "rolling_summary_evidence": ["sg_old2", "sg_old1", "sg_old2"],
}

_SEARCH_MEETINGS = [
    {"id": "m_percent", "title": "100% done"},
    {"id": "m_plain", "title": "100 percent"},
    {"id": "m_under", "title": "a_b test"},
    {"id": "m_other", "title": "aXb test"},
    {"id": "m_slash", "title": "back\\slash"},
    {"id": "m_budget", "title": "Budget Review", "status": "failed"},
    {"id": "m_ecole", "title": "École visit"},
    {"id": "m_state", "title": "Untitled",
     "state_json": '{"topic": {"current": "Capacity planning"}}'},
    {"id": "m_transcript", "title": "Sync", "segments": ["quarterly CAPACITY planning", "x"]},
    {"id": "m_date", "title": "Dated", "started_at": "2026-02-14T09:00:00+00:00",
     "status": "active"},
]

CASES = {
    "interval_iou": [
        [0, 10, 5, 15], [0, 5, 5, 10], [2, 2, 2, 2], [0.0, 2.0, 0.1, 2.1], [5, 1, 0, 10],
        [0, 10, 0, 10], [1, 3, 2, 8], [0, 1, 2, 3], [-1.5, 1.5, 0.0, 2.25], [0, 10, 2, 4],
        [0.1, 0.2, 0.15, 0.3],
    ],
    "match_live_to_offline": [
        [_OLD, _NEW],
        [_OLD, []],
        [[], _NEW],
        # Ties: two old rows equally over one new row; the later old row wins.
        [[_old("sg_a", 0.0, 2.0), _old("sg_b", 0.0, 2.0)], [_seg("sg_n", 0.0, 2.0)]],
        [[_old("sg_a", 0.0, 2.0, speaker="p_a")],
         [_seg("sg_n1", 0.0, 2.0), _seg("sg_n2", 0.0, 2.0)]],
        # An unpinned live label fills only an unlabeled offline row.
        [[_old("sg_a", 0.0, 2.0, speaker="p_live", source="diarizer"),
          _old("sg_b", 2.0, 4.0, speaker="", source="diarizer")],
         [_seg("sg_n1", 0.0, 2.0, speaker=""), _seg("sg_n2", 2.0, 4.0, speaker="p_off")]],
        # Below the IoU floor nothing pairs.
        [[_old("sg_a", 0.0, 2.0)], [_seg("sg_n", 1.6, 4.0)]],
        [[_old("sg_a", 0.0, 2.0)], [_seg("sg_n", 1.5, 3.5)]],
    ],
    "remap_state_evidence": [
        [_STATE, [["sg_old1", "sg_new1"], ["sg_old3", "sg_new3"]], _OLD, _NEW, ["sg_pin"]],
        [_STATE, [], _OLD, _NEW, []],
        [_STATE, [], _OLD, [], ["sg_pin", "sg_old2"]],
        [{"meeting_id": "m_1"}, [], _OLD, _NEW, []],
        [{"cards": None, "questions": None, "live_highlights": None,
          "rolling_summary_evidence": "sg_old1"}, [], _OLD, _NEW, []],
        [{"cards": [], "questions": {"q": 1}}, [], _OLD, _NEW, []],
        # Ambiguous: two new rows over one old row give no unique match.
        [{"rolling_summary_evidence": ["sg_a"]}, [], [_old("sg_a", 0.0, 2.0)],
         [_seg("sg_n1", 0.0, 2.0), _seg("sg_n2", 0.0, 2.0)], []],
        # Channel must match.
        [{"rolling_summary_evidence": ["sg_a"]}, [], [_old("sg_a", 0.0, 2.0)],
         [_seg("sg_n1", 0.0, 2.0, channel="loopback")], []],
    ],
    "content_summary": [
        [0, False, 0, ""],
        [3, True, 2, "hello"],
        [1, False, 0, ""],
        [0, False, 5, "x" * 100],
        [0, False, 5, "x" * 101],
        [2, True, 9, ("word " * 30).strip()],
        [1, False, 1, "é" * 120],
        [1, False, 1, "a" * 99 + " " + "b" * 10],
        [1, False, 1, "\U0001F600" * 101],
    ],
    "search_filter": [
        [needle, _SEARCH_MEETINGS] for needle in (
            "100%", "%", "_", "a_b", "back\\slash", "\\", "budget", "BUDGET", "review",
            "école", "École", "capacity", "planning", "2026-02", "failed", "active",
            "Sync", "nothing matches", "\"quoted\"", "m_percent",
        )
    ],
}

WRAPPERS = {
    "match_live_to_offline": _match,
    "remap_state_evidence": _remap,
    "content_summary": lambda module, *args: module._content_summary(*args),
    "search_filter": _search,
}
