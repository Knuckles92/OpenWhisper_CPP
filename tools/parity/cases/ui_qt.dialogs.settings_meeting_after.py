"""Curated inputs for ui_qt/dialogs/settings_meeting_after.py (fv_ui_meeting, M7.18).

Every vector builds a real MeetingAfterSection on an offscreen QApplication, its page laid out by
a stand-in _tile_group that returns a title label and an intro label, as the dialog's does. The
section's persist and persist_many record what they are given and, like the dialog's, ignore
everything while the section loads; its binder records nothing (the plain switches only save).
Each wrapper loads the settings first (with the loading flag set), then acts:

  load(settings)                                  what the page shows once loaded
  _on_report_view_toggled(settings, view, checked)
                                                  one report view's switch set to `checked`
  _on_end_report_toggled(settings, checked)       "Write the final report" set to `checked`
  _on_review_toggled(settings, answer)            the review switch turned on, the question
                                                  answered Yes (True) or No (False); the route
                                                  is the one the settings choose for Fast
                                                  judgments (selected_route)
  _meeting_after_rail_value(settings)             settings_dialog.py's rail value for the page
  _meeting_dashboard_rail_value(settings)         and for the Dashboard page

A state is {"report", "ribbon", "brief", "signal", "review"} (the switches), "views_enabled"
(the three view tiles), "title_enabled" (the group title and intro) and "hint_visible";
"persisted" lists every [key, value] saved, and "persisted_many" every update written at once.
"""
from __future__ import annotations

from unittest.mock import patch

_APP = []
_KEEP = []

VIEWS = {"ribbon": "meeting_report_ribbon", "brief": "meeting_report_brief",
         "signal": "meeting_report_signal"}


def _app():
    from PyQt6.QtWidgets import QApplication

    if not _APP:
        _APP.append(QApplication.instance() or QApplication([]))


class _Bindings:
    """The meeting binder: the plain switches save their key alone, which the dialog tests."""

    def checkbox(self, *args):
        pass

    def combo(self, *args):
        pass


def _build(module, settings):
    from PyQt6.QtWidgets import QLabel, QVBoxLayout, QWidget

    from ui_qt.dialogs.settings_fast_judgments import typesafe_route_from_settings

    _app()
    loading = [False]
    persisted = []
    persisted_many = []

    def persist(key, value):
        if not loading[0]:
            persisted.append([str(key), value])

    def persist_many(updates):
        if not loading[0]:
            persisted_many.append({str(key): value for key, value in updates.items()})

    section = module.MeetingAfterSection(
        None, persist=persist, persist_many=persist_many, bindings=_Bindings(),
        is_loading=lambda: loading[0],
        review_route=lambda: typesafe_route_from_settings(dict(settings)))
    page = QWidget()
    _KEEP.append(page)
    layout = QVBoxLayout(page)

    def tile_group(layout, title, tiles, intro=None, columns=2):
        title_label = QLabel(title, page)
        info = QLabel(intro or "", page)
        return title_label, info

    section.build(layout, tile_group)
    loading[0] = True
    try:
        section.load(dict(settings))
    finally:
        loading[0] = False
    return section, persisted, persisted_many


def _state(section):
    return {
        "report": section.meeting_end_report_check.isChecked(),
        "ribbon": section.meeting_report_ribbon_check.isChecked(),
        "brief": section.meeting_report_brief_check.isChecked(),
        "signal": section.meeting_report_signal_check.isChecked(),
        "review": section.meeting_review_check.isChecked(),
        "views_enabled": all(tile.isEnabled() for tile in (
            section.meeting_report_ribbon_tile, section.meeting_report_brief_tile,
            section.meeting_report_signal_tile)),
        "title_enabled": section.meeting_report_views_title.isEnabled()
        and section.meeting_report_views_info.isEnabled(),
        "hint_visible": not section.meeting_report_views_hint.isHidden(),
    }


def _load(module, settings):
    section, persisted, persisted_many = _build(module, settings)
    return {**_state(section), "persisted": persisted, "persisted_many": persisted_many}


def _view_toggled(module, settings, view, checked):
    section, persisted, persisted_many = _build(module, settings)
    getattr(section, f"meeting_report_{view}_check").setChecked(checked)
    return {**_state(section), "persisted": persisted, "persisted_many": persisted_many}


def _report_toggled(module, settings, checked):
    section, persisted, persisted_many = _build(module, settings)
    section.meeting_end_report_check.setChecked(checked)
    return {**_state(section), "persisted": persisted, "persisted_many": persisted_many}


def _review_toggled(module, settings, answer):
    from PyQt6.QtWidgets import QMessageBox

    section, persisted, persisted_many = _build(module, settings)
    asked = []

    def question(parent, title, text, buttons, default):
        asked.append([title, text])
        return QMessageBox.StandardButton.Yes if answer else QMessageBox.StandardButton.No

    with patch.object(module.QMessageBox, "question", side_effect=question):
        section.meeting_review_check.setChecked(True)
    return {**_state(section), "asked": asked, "persisted": persisted,
            "persisted_many": persisted_many}


def _after_rail(module, settings):
    from ui_qt.dialogs import settings_dialog

    return settings_dialog._meeting_after_rail_value(dict(settings))


def _dashboard_rail(module, settings):
    from ui_qt.dialogs import settings_dialog

    return settings_dialog._meeting_dashboard_rail_value(dict(settings))


CONSENT = "typesafe-text-v1"

LOADS = [{}]
for report in (True, False):
    for ribbon in (True, False):
        for brief in (True, False):
            for signal in (True, False):
                LOADS.append({"meeting_end_report": report, "meeting_report_ribbon": ribbon,
                              "meeting_report_brief": brief, "meeting_report_signal": signal})
LOADS += [
    {"meeting_insight_review": True, "meeting_insight_review_consent": CONSENT},
    {"meeting_insight_review": True, "meeting_insight_review_consent": "typesafe-text-v0"},
    {"meeting_insight_review": True},
    {"meeting_insight_review": False, "meeting_insight_review_consent": CONSENT},
    {"meeting_insight_review": "yes", "meeting_insight_review_consent": CONSENT},
    {"meeting_end_report": "no", "meeting_report_ribbon": 0},
]

VIEW_STATES = [
    {},
    {"meeting_report_brief": False, "meeting_report_signal": False},
    {"meeting_report_ribbon": False, "meeting_report_signal": False},
    {"meeting_report_ribbon": False, "meeting_report_brief": False},
    {"meeting_end_report": False, "meeting_report_brief": False, "meeting_report_signal": False},
]

#: The routes of the review's question: TypeSafe, OpenRouter, a text model on this computer,
#: a hosted text model, and AI cleanup's model on this computer.
ROUTES = [
    {},
    {"typesafe_provider": "openrouter"},
    {"typesafe_provider": "text_model", "meeting_llm_provider": "lm_studio",
     "meeting_llm_model": "qwen3-4b"},
    {"typesafe_provider": "text_model", "meeting_llm_provider": "openrouter",
     "meeting_llm_model": "openai/gpt-5-mini"},
    {"typesafe_provider": "text_model", "typesafe_text_model": "cleanup",
     "transcript_cleanup_provider": "ollama", "transcript_cleanup_model": "llama3.2"},
]

RAILS = [
    {},
    {"meeting_end_report": False},
    {"meeting_end_report": False, "meeting_end_polish": False},
    {"meeting_end_report": False, "meeting_end_polish": False, "meeting_end_redecode": True},
    {"meeting_end_report": False, "meeting_end_polish": True, "meeting_end_redecode": True},
]

DASHBOARDS = [
    {},
    {"meeting_server_bind": "lan"},
    {"meeting_server_bind": "lan", "meeting_server_port": 8080},
    {"meeting_server_bind": "localhost", "meeting_server_port": 8080},
    {"meeting_server_bind": "lan", "meeting_server_port": 70000},
    {"meeting_server_bind": "wan"},
]

WRAPPERS = {
    "MeetingAfterSection.load": _load,
    "MeetingAfterSection._on_report_view_toggled": _view_toggled,
    "MeetingAfterSection._on_end_report_toggled": _report_toggled,
    "MeetingAfterSection._on_review_toggled": _review_toggled,
    "_meeting_after_rail_value": _after_rail,
    "_meeting_dashboard_rail_value": _dashboard_rail,
}

CASES = {
    "MeetingAfterSection.load": [[settings] for settings in LOADS],
    "MeetingAfterSection._on_report_view_toggled": [
        [settings, view, checked] for settings in VIEW_STATES for view in VIEWS
        for checked in (True, False)
    ],
    "MeetingAfterSection._on_end_report_toggled": [
        [settings, checked] for settings in VIEW_STATES for checked in (True, False)
    ],
    "MeetingAfterSection._on_review_toggled": [
        [settings, answer] for settings in ROUTES for answer in (True, False)
    ],
    "_meeting_after_rail_value": [[settings] for settings in RAILS],
    "_meeting_dashboard_rail_value": [[settings] for settings in DASHBOARDS],
}

PINS = {
    "note": "a real MeetingAfterSection on an offscreen QApplication; persist and persist_many "
            "record what they are given outside loading, as the dialog's do; QMessageBox.question "
            "answers Yes or No as the case says. The rail values are settings_dialog.py's",
}
