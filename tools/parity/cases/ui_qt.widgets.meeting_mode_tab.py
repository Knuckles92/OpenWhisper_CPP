"""Curated inputs for the pure logic of ui_qt/widgets/meeting_mode_tab.py (M7.9 meeting-core).

    _finalization_heading, _step_icon, _step_detail_text, _step_action_label   as they are
    finalization_view(payload)          dataclasses.asdict(_FinalizationView.read(payload))
    meeting_audio_support_copy(platform, machine, mac_release)   and the tone and warning
    progress_percent(current, total)    the running card's bar value (_show_running_finalization)
    elapsed_text(seconds)               the on-air clock (_refresh_elapsed)
    card(payload, background_available) the finalization card a MeetingModeTab shows for the
        payload while no meeting is live: titles, texts, tone, which controls are visible and
        enabled, and the step rows. Read from the real widget, offscreen.

The widget is never shown, so "visible" is not isHidden(). Values that the widget leaves as they
were (the bar's value while indeterminate, the speaker re-run's enabled state while hidden, the
detail line's text while hidden) are recorded as None.
"""

import contextlib
import dataclasses
import os
import re
import types

_app = None


def _application():
    global _app
    os.environ.setdefault("QT_QPA_PLATFORM", "offscreen")
    from PyQt6.QtWidgets import QApplication

    _app = QApplication.instance() or QApplication([])
    return _app


@contextlib.contextmanager
def _pinned(platform, machine, mac_release):
    from meeting import platform as policy

    module = importlib_module()
    saved = (module.sys, policy.platform_module.machine, policy.platform_module.mac_ver)
    module.sys = types.SimpleNamespace(platform=platform)
    policy.platform_module.machine = lambda: machine
    policy.platform_module.mac_ver = lambda: (mac_release, ("", "", ""), "")
    try:
        yield module
    finally:
        module.sys = saved[0]
        policy.platform_module.machine = saved[1]
        policy.platform_module.mac_ver = saved[2]


def importlib_module():
    import importlib

    return importlib.import_module("ui_qt.widgets.meeting_mode_tab")


def _support_copy(module, platform, machine, mac_release):
    with _pinned(platform, machine, mac_release) as tab_module:
        return list(tab_module.meeting_audio_support_copy(platform))


def _tone(module, platform, machine, mac_release):
    with _pinned(platform, machine, mac_release) as tab_module:
        return tab_module.meeting_audio_platform_tone(platform)


def _warning(module, platform, machine, mac_release):
    with _pinned(platform, machine, mac_release) as tab_module:
        return tab_module.meeting_audio_shows_platform_warning(platform)


def _finalization_view(module, payload):
    return dataclasses.asdict(module._FinalizationView.read(payload))


def _heading(module, *args):
    return list(module._finalization_heading(*args))


def _icon(module, status):
    return list(module._step_icon(status))


def _visible(widget):
    return not widget.isHidden()


def _new_tab(module):
    _application()
    return module.MeetingModeTab()


def _progress_percent(module, current, total):
    tab = _new_tab(module)
    tab._set_finalization({"status": "running", "current_step": current, "total_steps": total})
    tab._apply_layout_state()
    value = tab.finalization_progress.value()
    tab.deleteLater()
    return value


def _elapsed_text(module, seconds):
    tab = _new_tab(module)
    tab._elapsed_base_s = seconds
    tab._running_since = None
    tab._refresh_elapsed()
    text = tab.elapsed_label.text()
    tab.deleteLater()
    return text


_COLOR = re.compile(r"color:\s*([^;]+);")


def _rows(tab):
    from PyQt6.QtGui import QFont
    from PyQt6.QtWidgets import QLabel, QPushButton

    rows = []
    layout = tab.finalization_steps_layout
    for index in range(layout.count()):
        row = layout.itemAt(index).widget()
        if row is None:
            continue
        icon = row.findChild(QLabel, "meetingFinalizationStepIcon")
        name = row.findChild(QLabel, "meetingFinalizationStepName")
        detail = row.findChild(QLabel, "meetingFinalizationStepDetail")
        action = row.findChild(QPushButton, "meetingFinalizationStepAction")
        color = _COLOR.search(icon.styleSheet())
        rows.append({
            "accessible_name": row.accessibleName(),
            "accessible_description": row.accessibleDescription(),
            "icon": icon.text(),
            "icon_color": color.group(1) if color else "",
            "name": name.text(),
            "running": name.font().weight() == QFont.Weight.DemiBold,
            "detail_text": detail.text(),
            "action_label": action.text() if action is not None else "",
            "action_enabled": action.isEnabled() if action is not None else True,
            "action_tooltip": action.toolTip() if action is not None else "",
        })
    return rows


def _card(module, payload, background_available):
    tab = _new_tab(module)
    tab._background_available = background_available
    tab._set_finalization(payload)
    tab._apply_layout_state()
    progress = tab.finalization_progress
    indeterminate = progress.minimum() == 0 and progress.maximum() == 0
    speakers = tab.finalization_retry_speakers_button
    card = {
        "title": tab.finalization_title.text(),
        "message": tab.finalization_message.text(),
        "tone": tab.finalization_card.property("finalizationTone"),
        "keep_hint": tab.finalization_keep_hint.text(),
        "keep_hint_visible": _visible(tab.finalization_keep_hint),
        "report_enabled": tab.finalization_report_button.isEnabled(),
        "report_tip": tab.finalization_report_button.toolTip(),
        "show_step_badge": _visible(tab.finalization_step_badge),
        "step_badge": tab.finalization_step_badge.text(),
        "show_progress": _visible(progress),
        "progress_minimum": progress.minimum(),
        "progress_maximum": progress.maximum(),
        "progress_value": None if indeterminate else progress.value(),
        "show_detail": _visible(tab.finalization_detail),
        "detail": tab.finalization_detail.text() if _visible(tab.finalization_detail) else None,
        "show_steps": _visible(tab.finalization_steps_widget),
        "steps": _rows(tab) if _visible(tab.finalization_steps_widget) else [],
        "show_active_box": _visible(tab.finalization_active_box),
        "show_retry": _visible(tab.finalization_retry_button),
        "retry_enabled": tab.finalization_retry_button.isEnabled(),
        "show_background": _visible(tab.finalization_background_button),
        "show_keep_later": _visible(tab.finalization_keep_later_button),
        "show_done": _visible(tab.finalization_done_button),
        "show_open_report": _visible(tab.finalization_report_button),
        "show_rerun_speakers": _visible(speakers),
        "rerun_speakers_enabled": speakers.isEnabled() if _visible(speakers) else None,
        "rerun_speakers_tip": speakers.toolTip(),
    }
    tab.deleteLater()
    return card


def _step(step_id, status, name=None, detail=None):
    step = {"id": step_id, "status": status}
    if name is not None:
        step["name"] = name
    if detail is not None:
        step["detail"] = detail
    return step


_RUNNING_STEPS = [
    _step("redecode", "completed", "Audio Re-transcription", "High-accuracy re-decoding complete"),
    _step("speaker_id", "running", "Speaker Identification", "Uploading system audio"),
    _step("finalize", "pending", "State Finalization", "Saving final transcript"),
]
_DONE_STEPS = [
    _step("redecode", "completed", "Audio Re-transcription", "High-accuracy re-decoding complete"),
    _step("polish", "completed", "Transcript Cleanup", "Transcript cleanup finished"),
    _step("finalize", "completed", "State Finalization", "Saved 3 segments (12 words)"),
]
_FAILED_STEPS = [
    _step("redecode", "failed", "Audio Re-transcription", "Re-decoding failed; kept live transcript"),
    _step("speaker_id", "skipped", "Speaker Identification", "Consent missing"),
    _step("polish", "completed", "Transcript Cleanup", "Transcript cleanup finished"),
    _step("finalize", "completed", "State Finalization", "Saved 3 segments (12 words)"),
]
_ODD_STEPS = [
    {"status": "completed"},
    {"id": "polish"},
    {"id": "speaker_id", "status": None, "name": "", "detail": None},
    {"id": "consolidation", "status": "failed", "name": "Summary & Action Items", "detail": 5},
    {"id": "finalize", "status": "skipped", "name": "State Finalization", "detail": ""},
]

_SPEAKERS_OK = {"can_rerun_speakers": True}
_SPEAKERS_BLOCKED = {"can_rerun_speakers": True,
                     "speaker_rerun_reason": "Audio-upload consent has not been given."}


def _payload(status, message="", steps=None, current=0, total=0, details="", content=None):
    payload = {"status": status, "message": message, "current_step": current,
               "total_steps": total, "step_details": details}
    if steps is not None:
        payload["steps"] = steps
    if content is not None:
        payload["content_summary"] = content
    return payload


_CARDS = [
    [_payload("running", "Identifying speakers…", _RUNNING_STEPS, 2, 3,
              "Uploading system audio"), True],
    [_payload("running", "Identifying speakers…", _RUNNING_STEPS, 2, 3,
              "Uploading system audio"), False],
    [_payload("running", "Re-transcribing meeting…", _RUNNING_STEPS, 1, 4,
              "Re-transcribing meeting…"), True],
    [_payload("running", "", [], 0, 0, ""), False],
    [_payload("running", "", _RUNNING_STEPS, 3, 3, "x", content=_SPEAKERS_OK), True],
    [_payload("running", "Finalizing", _RUNNING_STEPS, 9, 2, "", content={"is_empty": True}),
     False],
    [_payload("completed", "Final insights ready — 3 segments.", _DONE_STEPS, 3, 3,
              "Final insights ready — 3 segments.", content=_SPEAKERS_OK), True],
    [_payload("completed", "", _DONE_STEPS, 3, 3, "", content=_SPEAKERS_BLOCKED), False],
    [_payload("completed", "", [], 0, 0, "", content={"speaker_rerun_offered": False}), False],
    [_payload("completed", "Speaker identification finished.", _RUNNING_STEPS[:2], 2, 2, "",
              content=_SPEAKERS_OK), False],
    [_payload("completed", "Done.", _FAILED_STEPS, 4, 4, "Detail line", content=_SPEAKERS_OK),
     True],
    [_payload("failed", "Audio Re-transcription failed. The recording and transcript were kept.",
              _FAILED_STEPS, 4, 4, "Audio Re-transcription failed.", content=_SPEAKERS_BLOCKED),
     False],
    [_payload("failed", "", [], 0, 0, ""), False],
    [_payload("unavailable",
              "Final insights are unavailable until transcription recovery finishes.",
              _DONE_STEPS, 3, 3,
              "Final insights are unavailable until transcription recovery finishes."), False],
    [_payload("unavailable", "The agent did not start."), True],
    [_payload("disabled", "AI insights are off for this meeting."), False],
    [_payload("disabled", "", content=_SPEAKERS_OK), True],
    [_payload("disabled", "", content={"speaker_rerun_offered": False, **_SPEAKERS_OK}), False],
    [_payload("completed", "", [], 0, 0, "", content={"is_empty": True}), False],
    [_payload("failed", "", [], 0, 0, "",
              content={"is_empty": True, "meeting_status": "failed"}), False],
    [_payload("failed", "Start failed.", [], 0, 0, "", content={"meeting_status": "failed"}),
     False],
    [_payload("completed", "Odd.", _ODD_STEPS, 5, 5, "Odd detail", content=_SPEAKERS_BLOCKED),
     False],
    [_payload("weird", "", _DONE_STEPS[:1], 1, 1, ""), False],
    [_payload("weird", "", _FAILED_STEPS[:1], 1, 1, "Retry me"), False],
]

_PLATFORMS = [
    ["win32", "AMD64", ""], ["darwin", "arm64", "14.0"], ["darwin", "x86_64", "12.6"],
    ["darwin", "arm64", ""], ["linux", "x86_64", ""], ["linux", "aarch64", ""],
    ["linux", "i686", ""], ["linux", "AMD64", ""], ["freebsd14", "amd64", ""],
    ["cygwin", "x86_64", ""],
]

_STATUSES = ["completed", "running", "failed", "skipped", "pending", "", None, "done", 5]

CASES = {
    "_finalization_heading": [
        [status, message, failed, empty, meeting_failed]
        for status in ("running", "completed", "disabled", "unavailable", "failed", "", "odd")
        for message in ("", "Given message.")
        for failed, empty, meeting_failed in ((False, False, False), (True, False, False),
                                              (False, True, False), (False, True, True),
                                              (True, True, True), (False, False, True))
    ],
    "_step_icon": [[status] for status in _STATUSES],
    "_step_detail_text": [[status] for status in _STATUSES],
    "_step_action_label": [
        [step_id, status, allow]
        for step_id in ("redecode", "speaker_id", "polish", "consolidation", "finalize", "")
        for status in ("failed", "skipped", "completed", "running", "pending")
        for allow in (True, False)
    ],
    "finalization_view": [
        [{}],
        [{"status": "running", "message": "  Working…  ", "current_step": 2,
          "total_steps": 4, "step_details": "\tdetail\n", "steps": _RUNNING_STEPS,
          "content_summary": {"meeting_status": "failed", "is_empty": 1}}],
        [{"status": None, "message": None, "current_step": None, "total_steps": "3",
          "step_details": None, "steps": None, "content_summary": None}],
        [{"status": 5, "current_step": 2.9, "total_steps": True, "message": 0,
          "content_summary": {"meeting_status": "ended", "is_empty": 0}}],
        [{"status": "completed", "current_step": "x"}],
        [{"status": "completed", "current_step": [1]}],
        [{"status": "completed", "steps": "ab"}],
        [{"status": "completed", "content_summary": {"is_empty": "no"}}],
    ],
    "meeting_audio_support_copy": _PLATFORMS,
    "meeting_audio_platform_tone": _PLATFORMS,
    "meeting_audio_shows_platform_warning": _PLATFORMS,
    "progress_percent": [[current, total] for total in (1, 2, 3, 4, 5, 7, 20)
                         for current in (0, 1, 2, 3, total, total + 1)] + [[-3, 4]],
    "elapsed_text": [[0.0], [0.4], [9.99], [59.0], [60.0], [61.7], [599.0], [3599.9], [3600.0],
                     [3661.0], [36000.0], [359999.0], [-5.0], [86399.5]],
    "card": _CARDS,
}

WRAPPERS = {
    "_finalization_heading": _heading,
    "_step_icon": _icon,
    "finalization_view": _finalization_view,
    "meeting_audio_support_copy": _support_copy,
    "meeting_audio_platform_tone": _tone,
    "meeting_audio_shows_platform_warning": _warning,
    "progress_percent": _progress_percent,
    "elapsed_text": _elapsed_text,
    "card": _card,
}

PINS = {
    "platform_note": "meeting_audio_* run with sys.platform, platform.machine() and "
                     "platform.mac_ver() pinned to the case's three arguments",
    "widget_note": "progress_percent, elapsed_text and card are read from a MeetingModeTab built "
                   "offscreen (QT_QPA_PLATFORM=offscreen), never shown",
}
