"""Curated inputs for meeting/content.py (M7.7 meeting-state-store).

    meeting_display_title(row), fallback_meeting_title(row), meeting_preview_text(row)
    meeting_insights_pill(row)            (label, tone) as a list, or None
    untitled_title_from_topic(state), build_untitled_title_ops(state)
"""

import json


def _state(**fields):
    return json.dumps(fields)


_ROWS = [
    {},
    {"title": "  SQL title  ", "state_json": _state(title="State title")},
    {"title": "", "state_json": _state(title="  State title ", topic={"current": "Topic"})},
    {"title": None, "state_json": _state(title="", topic={"current": "  Topic only  "})},
    {"title": "   ", "state_json": _state(topic="not a dict")},
    {"state": {"title": "Live state title"}, "state_json": _state(title="Ignored")},
    {"state": "not a dict", "state_json": _state(title="From json")},
    {"state_json": "{broken", "status": "failed"},
    {"status": "FAILED"},
    {"status": "ended"},
    {"status": None},
    {"title": 42},
    {"content_summary": {"preview_text": "  Hello from ASR  "},
     "state_json": _state(topic={"current": "Ignored topic"})},
    {"content_summary": {"preview_text": "   "},
     "state_json": _state(topic={"current": "Quarterly roadmap"})},
    {"content_summary": "not a dict", "state_json": _state(topic={"current": "Topic"})},
    {"content_summary": {"is_empty": True}, "status": "ended"},
    {"content_summary": {"is_empty": False}, "status": "ended",
     "state_json": _state(finalization={"status": "completed"}, cloud_enabled=True)},
    {"status": "failed", "content_summary": {"is_empty": True}},
    {"status": "active", "state_json": _state(finalization={"status": "running"})},
    {"status": "ended", "cloud_enabled": True, "state_json": _state(finalization={
        "status": "failed", "card_deferred": True})},
    {"status": "ended", "cloud_enabled": False},
    {"status": "ended", "cloud_enabled": True},
    {"status": "needs_recovery", "cloud_enabled": True, "state_json": "{broken"},
]

_STATES = [
    {},
    {"title": "Named"},
    {"title": "   ", "topic": {"current": "  Budget planning  "}},
    {"title": None, "topic": {"current": "x" * 150}},
    {"topic": {"current": "é" * 130}},
    {"topic": "not a dict"},
    {"topic": {"current": None}},
    {"topic": {"current": "   "}},
    {"topic": {}},
]

CASES = {
    "meeting_display_title": [[row] for row in _ROWS],
    "fallback_meeting_title": [[row] for row in _ROWS],
    "meeting_preview_text": [[row] for row in _ROWS],
    "meeting_insights_pill": [[row] for row in _ROWS],
    "untitled_title_from_topic": [[state] for state in _STATES],
    "build_untitled_title_ops": [[state] for state in _STATES],
}


def _pill(module, row):
    pill = module.meeting_insights_pill(row)
    return None if pill is None else list(pill)


WRAPPERS = {"meeting_insights_pill": _pill}
