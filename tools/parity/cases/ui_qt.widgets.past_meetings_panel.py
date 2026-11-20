"""Curated inputs for the pure logic of ui_qt/widgets/past_meetings_panel.py (M7.15).

    _format_duration(meeting)        as it is (the module's alias of meeting.time_utils)
    load_meetings(rows)              PastMeetingsPanel._load_meetings(query, limit, repository,
        provider) with a provider of `rows`: the rows that are history, a row without a
        content_summary given the fake repository's, and whether the source filtered
    filter_matches(meetings, query)  ids of the meetings PastMeetingsPanel._filter_meetings keeps
    list_view(meetings, query, source_filtered, has_more)   what _rebuild_list shows for the rows
        held and the search text: the count line and whether it shows, the placeholders (the one
        that replaces the list or the one after the cards) and the ids of the cards
    card(meeting)                    the texts and visibility of a PastMeetingItem
    has_transcript(meeting), meeting_has_audio(meeting), meeting_location(meeting)

Every timestamp is naive (wall time in any zone) or aware only where the duration needs no zone.
The panel and the cards are built offscreen (QT_QPA_PLATFORM=offscreen) and never shown, so
"visible" is not isHidden().
"""

import os
import types

_app = None


def _application():
    global _app
    os.environ.setdefault("QT_QPA_PLATFORM", "offscreen")
    from PyQt6.QtWidgets import QApplication

    _app = QApplication.instance() or QApplication([])
    return _app


def _visible(widget):
    return not widget.isHidden()


def _panel(module, meetings=(), query="", source_filtered=False, has_more=False):
    from PyQt6.QtWidgets import QWidget  # noqa: F401  (the application must exist first)

    _application()
    panel = module.PastMeetingsPanel(meeting_provider=lambda: [])
    panel._meetings = [dict(meeting) for meeting in meetings]
    panel._source_filtered = source_filtered
    panel._has_more = has_more
    panel.search_input.blockSignals(True)
    panel.search_input.setText(query)
    panel.search_input.blockSignals(False)
    return panel


def _load_meetings(module, rows):
    repository = types.SimpleNamespace(
        get_meeting_content_summary=lambda meeting_id: {"has_audio": True,
                                                        "has_transcript": False,
                                                        "from_repository": meeting_id})
    meetings, source_filtered = module.PastMeetingsPanel._load_meetings(
        "", 101, repository, lambda: rows)
    return [meetings, source_filtered]


def _filter_matches(module, meetings, query):
    panel = _panel(module, meetings, query)
    ids = [meeting["id"] for meeting in panel._filter_meetings(panel._meetings)]
    panel.deleteLater()
    return ids


def _list_view(module, meetings, query, source_filtered, has_more):
    from PyQt6.QtWidgets import QLabel

    panel = _panel(module, meetings, query, source_filtered, has_more)
    panel._rebuild_list()
    layout = panel.meetings_list_layout
    placeholders = []
    cards = []
    for index in range(layout.count()):
        widget = layout.itemAt(index).widget()
        if isinstance(widget, module.PastMeetingItem):
            cards.append(widget.meeting_id)
        elif isinstance(widget, QLabel):
            placeholders.append(widget.text())
    view = {"count": panel.section_header.text(),
            "count_visible": _visible(panel.section_header),
            "placeholders": placeholders,
            "cards": cards}
    panel.deleteLater()
    return view


def _card(module, meeting):
    _application()
    item = module.PastMeetingItem(meeting)
    pill = item.insights_pill
    card = {
        "id": item.meeting_id,
        "title": item.title_label.text(),
        "date": item.date_label.text(),
        "preview": item.preview_label.text(),
        "preview_visible": _visible(item.preview_label),
        "note": item.content_label.text(),
        "note_visible": _visible(item.content_label),
        "detail": item.detail_label.text(),
        "pill": pill.text(),
        "pill_visible": _visible(pill),
        "pill_tone": pill.property("pillTone"),
        "location": item.location_pill.text(),
        "location_visible": _visible(item.location_pill),
        "location_tip": item.location_pill.toolTip(),
        "stored_on": item.stored_on,
    }
    item.deleteLater()
    return card


def _has_transcript(module, meeting):
    _application()
    item = module.PastMeetingItem(meeting)
    value = item._has_transcript()
    item.deleteLater()
    return value


def _meeting_has_audio(module, meeting):
    return module.PastMeetingsPanel._meeting_has_audio(meeting)


def _meeting_location(module, meeting):
    return list(module._meeting_location(meeting))


def _meeting(meeting_id, title="Review", status="ended", started="2026-03-14T09:30:00",
             minutes=42, paused=120, **extra):
    from datetime import datetime, timedelta

    began = datetime.fromisoformat(started)
    row = {"id": meeting_id, "title": title, "status": status, "started_at": started,
           "ended_at": (began + timedelta(minutes=minutes)).isoformat(),
           "paused_total_s": paused}
    row.update(extra)
    return row


def _state(**fields):
    import json

    return json.dumps(fields)


def _many(count):
    return [_meeting(f"m_{index:03d}", f"Meeting {index}") for index in range(count)]


_PLANNING = _meeting("m_plan", "Planning review", content_summary={
    "has_transcript": True, "preview_text": "We should ship the sidebar polish next."},
    state_json=_state(finalization={"status": "completed"}))
_STANDUP = _meeting("m_stand", "Standup", started="2026-03-10T08:30:00", minutes=15, paused=0,
                    content_summary={"has_transcript": False, "preview_text": ""})
_SAVED = _meeting("m_saved", "Deferred review", started="2026-03-13T15:00:00", minutes=25,
                  paused=0, content_summary={"has_transcript": True, "preview_text": "Notes."},
                  state_json=_state(finalization={"status": "failed", "card_deferred": True}))
_FAILED = _meeting("m_failed", "", status="failed", minutes=0, paused=0,
                   content_summary={"is_empty": True, "has_audio": False,
                                    "has_transcript": False})
_TOPIC = _meeting("m_topic", "", state_json=_state(title="", topic={"current": "Roadmap"}),
                  content_summary={"has_transcript": True, "preview_text": "First numbers."})
_LIVE = _meeting("m_live", "Running", status="active")
_PAUSED = _meeting("m_paused", "On hold", status="paused")
_ENDING = _meeting("m_ending", "Wrapping up", status="Ending")
_UPPER = _meeting("m_upper", "Done", status="ENDED")
_ODD_STATUS = _meeting("m_odd", "Odd", status="finalizing_report")
_NO_STATUS = {"id": "m_bare"}
_THREE = [_PLANNING, _STANDUP, _SAVED]
_MIX = [_PLANNING, _STANDUP, _SAVED, _FAILED, _TOPIC]

_CARDS = [
    _PLANNING, _STANDUP, _SAVED, _FAILED, _TOPIC, _ODD_STATUS, _NO_STATUS,
    _meeting("m_a", "Empty", content_summary={"is_empty": True}),
    _meeting("m_b", "No transcript", content_summary={"has_transcript": False}),
    _meeting("m_c", "No audio", content_summary={"has_audio": False, "has_transcript": True}),
    _meeting("m_d", "Truthy empty", content_summary={"is_empty": 1}),
    _meeting("m_e", "Failed with summary", status="failed",
             content_summary={"is_empty": True}),
    _meeting("m_f", "Cancelled", status="cancelled", minutes=3, paused=0),
    _meeting("m_g", "Ended upper", status="ENDED"),
    _meeting("m_h", "Blank status", status=""),
    _meeting("m_i", "Stored there", stored_on="Studio PC"),
    _meeting("m_j", "Also there", also_on="Studio PC"),
    _meeting("m_k", "From a friend", origin_device_name="Laptop"),
    _meeting("m_l", "Both", stored_on="Host A", also_on="Host B", origin_device_name="Laptop"),
    {"id": "m_m", "title": "Short", "status": "ended", "started_at": "2026-08-20T16:55:00",
     "ended_at": "2026-08-20T16:55:40", "paused_total_s": 0},
    {"id": "m_n", "title": "Hours", "status": "ended", "started_at": "2026-03-14T09:00:00",
     "ended_at": "2026-03-14T10:05:00", "paused_total_s": 0},
    {"id": "m_o", "title": "No times", "status": "ended"},
    {"id": "m_p", "status": "ended", "started_at": "bad", "ended_at": "2026-03-14T10:05:00"},
]

CASES = {
    "_format_duration": [
        [{"started_at": "2026-08-20T16:55:00Z", "ended_at": "2026-08-20T16:55:40Z",
          "paused_total_s": 0}],
        [{"started_at": "2026-08-20T16:55:00Z", "ended_at": "2026-08-20T17:07:00Z",
          "paused_total_s": 0}],
        [{"started_at": "2026-08-20T16:55:00Z", "ended_at": "2026-08-20T18:00:00Z",
          "paused_total_s": 0}],
        [{"started_at": "2026-03-14T09:30:00", "ended_at": "2026-03-14T10:12:00",
          "paused_total_s": 120}],
        [{"started_at": "2026-03-14T09:30:00", "ended_at": "2026-03-14T09:31:00",
          "paused_total_s": 600}],
        [{"started_at": "2026-03-14T09:30:00"}],
        [{}],
    ],
    "load_meetings": [
        [[]],
        [[_PLANNING, _LIVE, _PAUSED, _ENDING, _UPPER]],
        [[_meeting("m_x", "No summary"), _meeting("m_y", "Has summary",
                                                  content_summary={"has_transcript": True})]],
        [[_meeting("m_z", "Null summary", content_summary=None), _NO_STATUS, _ODD_STATUS]],
    ],
    "filter_matches": [
        [_MIX, query] for query in (
            "", "   ", "planning", "PLANNING", "  planning  ", "no-such-meeting", "sidebar",
            "mar 14", "2026", "09:30 am", "40 min", "25 min", "ready", "saved for later",
            "failed", "failed start", "ended", "untitled", "roadmap", "failed meeting", "empty",
            "polish next", "ship the", "m_plan", "review standup", "deferred", "·",
        )
    ] + [[[_PLANNING, _LIVE, _ODD_STATUS, _NO_STATUS], query]
         for query in ("finalizing", "report", "running", "untitled meeting")],
    "list_view": [
        [[], "", False, False],
        [[], "planning", True, False],
        [[], "planning", False, False],
        [[_PLANNING], "", False, False],
        [_THREE, "", False, False],
        [_THREE, "planning", False, False],
        [_THREE, "no-such-meeting", False, False],
        [_THREE, "  ", False, False],
        [_THREE, "planning", True, False],
        [_THREE, "no-such-meeting", True, False],
        [_MIX, "", True, False],
        [_MIX, "failed", False, False],
        [_many(99), "", False, False],
        [_many(100), "", False, False],
        [_many(100), "", False, True],
        [_many(101), "", False, True],
        [_many(101), "", True, True],
        [_many(101), "meeting 1", False, True],
        [_many(150), "", False, False],
        [_many(150), "", True, False],
        [_many(150), "meeting 14", False, False],
        [_many(150), "meeting 14", False, True],
        [_many(150), "meeting 149", True, True],
    ],
    "card": [[meeting] for meeting in _CARDS],
    "has_transcript": [
        [_PLANNING], [_STANDUP], [_meeting("m_q", "No summary")],
        [_meeting("m_r", "Topic only", state_json=_state(topic={"current": "Roadmap"}))],
        [_meeting("m_s", "Null flag", content_summary={"has_transcript": None})],
        [_meeting("m_t", "Zero flag", content_summary={"has_transcript": 0})],
    ],
    "meeting_has_audio": [
        [None], [{}], [_PLANNING], [_FAILED], [_meeting("m_u", "Spool", spool_dir="meetings/m_u")],
        [_meeting("m_v", "Blank spool", spool_dir="")],
        [_meeting("m_w", "Flag beats spool", spool_dir="meetings/m_w",
                  content_summary={"has_audio": False})],
        [_meeting("m_x", "Null flag", content_summary={"has_audio": None})],
        [{"id": "m_y"}],
    ],
    "meeting_location": [
        [{}], [{"stored_on": "Studio PC"}], [{"also_on": "Studio PC"}],
        [{"origin_device_name": "Laptop"}],
        [{"stored_on": "Host A", "also_on": "Host B", "origin_device_name": "Laptop"}],
        [{"stored_on": None, "also_on": "Host B"}], [{"stored_on": 5}],
        [{"stored_on": "", "also_on": "", "origin_device_name": ""}],
    ],
}

WRAPPERS = {
    "load_meetings": _load_meetings,
    "filter_matches": _filter_matches,
    "list_view": _list_view,
    "card": _card,
    "has_transcript": _has_transcript,
    "meeting_has_audio": _meeting_has_audio,
    "meeting_location": _meeting_location,
}

PINS = {
    "widget_note": "list_view and card are read from a PastMeetingsPanel and a PastMeetingItem "
                   "built offscreen (QT_QPA_PLATFORM=offscreen), never shown",
    "zone_note": "every started_at that is shown is naive, so it reads as wall time in any zone",
}
