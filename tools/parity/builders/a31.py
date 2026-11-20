"""Reference builders of the a31 Meeting Mode pages (fv_ui_settings, M5.19; M7 extends this file).

Each builder returns (widget, (width, height), focus widget or None) with fixed fake data.

meeting_mode_tab/<state> (M7.10, fv_ui_meeting) is the Meeting Mode tab at the width it asks for
(preferred_width plus the page margins) and the stage's height plus the page margins. Pinned: the
Windows platform (the copy and the setup link follow sys.platform), developer mode and the AI
insights switch off, the speech note empty, ai_insights_destination giving "OpenRouter ·
deepseek-v4.1-flash — sends transcript text, never audio.", time.monotonic() at T0, the LiveDot's
breath disconnected (phase 0) and no recent-meetings read (the empty stage lists RECENT_ROWS).
Timestamps are naive, so they read as wall time in any zone. The C++ test (tests/ui_meeting/
test_reference_screenshots.cpp) gives the tab the same data on a FakeClock and calls
setAiInsightsAvailable(true) with the same destination.

    idle              empty stage with three recent meetings
    live              a meeting at 00:42
    paused            the same meeting paused at 00:42
    starting          a start not yet confirmed
    result-running    the wrap-up at step 2 of 4
    result-completed  every step done
    result-failed     final insights failed after a failed re-transcription
    empty-meeting     a meeting that captured nothing

The Fast judgments page body is OpenWhisper's own, built by a real SettingsDialog over a
throwaway settings file in the isolated data folder (as builders/a11.py builds the App pages),
then taken out of the dialog at PAGE_WIDTH: OpenWhisper_CPP's page class is the body the Settings shell
hosts, and the shell around it is settings_dialog/shell's. OpenWhisper_CPP differs where this builder
puts its own in: the body has no object name (settingsPage_<key> is the shell's wrapper).

    settings_fast_judgments_page/off-no-key   TypeSafe, Fast judgments off, no key set
    settings_fast_judgments_page/text-model   "Your text model" (LM Studio, qwen3-4b, on this
                                              computer), Live checks on

The Past Meetings widgets (M7.15, fv_ui_meeting) are OpenWhisper's own. Today is pinned to
2026-03-15 for the calendar button, as in a09.py, and every timestamp is naive, so it reads as
wall time in any zone. Where the copy names the product, OpenWhisper_CPP's name is put in.

    past_meetings_panel/list             the panel at the History sidebar's width with six rows
                                         in the provider (ready, saved for later, failed start,
                                         empty, untitled with a topic, and a running one the
                                         panel leaves out), "Saved for later" selected
    meeting_delete_dialog/with-audio     the delete confirmation for a meeting with recordings
    meeting_delete_dialog/no-audio       and for one without
    meeting_recovery_dialog/two-rows     two interrupted meetings, the first settled
                                         "(finalizing)"
    meeting_intro_dialog/first-visit     the first-visit overview
    meeting_export_dialog/three-meetings the export dialog over three past meetings, the date
                                         range on 2026-02-15 to 2026-03-15
"""

from contextlib import ExitStack
from unittest.mock import patch

PRODUCT = "OpenWhisper_CPP"
#: Two tile columns at every font scale, as the page has inside the window at its default size.
PAGE_WIDTH = 900

_KEEP = []
_WIDGETS = []
_APP = []


def _release():
    from PyQt6 import sip

    for widget in reversed(_WIDGETS):
        if not sip.isdeleted(widget):
            sip.delete(widget)
    _WIDGETS.clear()
    for stack in reversed(_KEEP):
        stack.close()
    _KEEP.clear()
    _APP.clear()


def _keep(widget):
    import atexit

    from PyQt6.QtWidgets import QApplication

    if not _APP:
        _APP.append(QApplication.instance())
        atexit.register(_release)
    _WIDGETS.append(widget)
    return widget


def _app_dialog(values):
    import os

    from services.settings import SettingsManager
    from ui_qt.dialogs import settings_dialog as settings_dialog_module
    from ui_qt.dialogs import settings_downloads as downloads_module
    from ui_qt.dialogs import settings_models as models_module

    # The page reads credential variables from the environment: none of this process's may
    # reach a reference (nor be read at all).
    for name in [name for name in os.environ if name.upper().endswith("_API_KEY")]:
        del os.environ[name]
    folder = os.path.join(os.environ["OPENWHISPER_DATA_DIR"], "a31")
    os.makedirs(folder, exist_ok=True)
    store = SettingsManager(os.path.join(folder, f"settings{len(_KEEP)}.json"))
    store.save_all_settings(dict(values))
    stack = ExitStack()
    for module in (settings_dialog_module, models_module, downloads_module):
        stack.enter_context(patch.object(module, "settings_manager", store))
    stack.enter_context(patch.object(settings_dialog_module.history_manager, "set_retention"))
    for module in (models_module, downloads_module):
        stack.enter_context(patch.object(module, "scan_cached_models", return_value={}))
    stack.enter_context(patch("services.local_asr.cache.inventory", return_value={}))
    stack.enter_context(
        patch.object(settings_dialog_module, "is_native_wayland_session", return_value=False))
    stack.enter_context(patch.object(settings_dialog_module.QSystemTrayIcon,
                                     "isSystemTrayAvailable", return_value=True))
    _KEEP.append(stack)
    return _keep(settings_dialog_module.SettingsDialog(background_cache_scan=False))


def _openwhisper-cpp(page):
    """The page body as OpenWhisper_CPP builds it (see above)."""
    from PyQt6.QtWidgets import QLabel

    from ui_qt.widgets.wrapped_label import WrappedLabel

    page.setObjectName("")
    for label in page.findChildren(QLabel):
        if "OpenWhisper" in label.text():
            text = label.text().replace("OpenWhisper", PRODUCT)
            if isinstance(label, WrappedLabel):
                label.setText(text)
            else:
                QLabel.setText(label, text)


def _settled_height(widget, width):
    """The height the widget's layout needs at `width`, measured shown."""
    from PyQt6.QtCore import QCoreApplication

    widget.resize(width, 4000)
    widget.show()
    for _ in range(3):
        QCoreApplication.processEvents()
    height = widget.layout().minimumSize().height()
    widget.hide()
    return height


def _app_page(key, values=None):
    dialog = _app_dialog(values or {})
    dialog.ensure_page(key)
    dialog._page_scrolls[key].takeWidget()
    page = dialog._pages[key]
    page.setParent(None)
    _keep(page)
    _openwhisper-cpp(page)
    return page, (PAGE_WIDTH, _settled_height(page, PAGE_WIDTH)), None


def settings_fast_judgments_page_off_no_key(variant):
    from ui_qt.dialogs.settings_destinations import MEETING_FAST

    return _app_page(MEETING_FAST)


#: LM Studio is a local server preset (http://127.0.0.1:1234/v1, no key), so its judgments never
#: leave this computer.
TEXT_MODEL_SETTINGS = {
    "typesafe_enabled": True,
    "typesafe_provider": "text_model",
    "meeting_llm_provider": "lm_studio",
    "meeting_llm_model": "qwen3-4b",
}


def settings_fast_judgments_page_text_model(variant):
    from ui_qt.dialogs.settings_destinations import MEETING_FAST

    return _app_page(MEETING_FAST, TEXT_MODEL_SETTINGS)


#: The pinned time.monotonic(), in seconds.
T0 = 5000.0
AI_DESTINATION = ("OpenRouter · deepseek-v4.1-flash", "sends transcript text, never audio")
RECENT_ROWS = [
    {"id": "m_review", "title": "Weekly design review", "status": "ended",
     "started_at": "2026-11-13T15:30:00", "ended_at": "2026-11-13T16:12:00",
     "paused_total_s": 0, "cloud_enabled": True,
     "state_json": '{"cloud_enabled": true, "finalization": {"status": "completed"}}'},
    {"id": "m_acme", "title": "Customer call with Acme", "status": "ended",
     "started_at": "2026-11-12T09:00:00", "ended_at": "2026-11-12T09:25:40",
     "paused_total_s": 0},
    {"id": "m_failed", "title": "", "status": "failed",
     "started_at": "2026-11-10T14:00:00", "ended_at": "2026-11-10T14:00:20"},
]
IDENTITY = {"meeting_id": "m_sync", "display_title": "Quarterly roadmap sync",
            "started_at": "2026-11-14T09:00:00", "ended_at": "2026-11-14T09:42:00",
            "paused_total_s": 120}
FOUR_STEPS = [("redecode", "Audio Re-transcription"), ("polish", "Transcript Cleanup"),
              ("consolidation", "Summary & Action Items"), ("finalize", "State Finalization")]


def _steps(*statuses, details=None):
    details = details or {}
    steps = []
    for (step_id, name), status in zip(FOUR_STEPS, statuses):
        step = {"id": step_id, "name": name, "status": status}
        if step_id in details:
            step["detail"] = details[step_id]
        steps.append(step)
    return steps


#: The scripts of the C++ test, payload by payload.
MEETING_SCRIPTS = {
    "idle": [],
    "live": [{"active": True, "paused": False, "status": "active", "elapsed_s": 42.0,
              "elapsed_at": T0}],
    "paused": [{"active": True, "paused": False, "status": "active", "elapsed_s": 42.0,
                "elapsed_at": T0},
               {"active": True, "paused": True, "status": "paused", "elapsed_s": 42.0,
                "elapsed_at": T0}],
    "starting": [{"active": False, "status": "starting"}],
    "result-running": [{"active": False, "status": "ended", "dashboard_available": True,
                        **IDENTITY, "finalization": {
                            "status": "running", "message": "Cleaning transcript…",
                            "stage": "polish", "current_step": 2, "total_steps": 4,
                            "step_details": "Polishing block 1 of 2 (40 segments)...",
                            "steps": _steps("completed", "running", "pending", "pending")}}],
    "result-completed": [{"active": False, "status": "ended", "dashboard_available": True,
                          **IDENTITY, "finalization": {
                              "status": "completed",
                              "message": "Final insights ready — 32 segments, 4 key points.",
                              "current_step": 4, "total_steps": 4,
                              "steps": _steps("completed", "completed", "completed",
                                              "completed"),
                              "content_summary": {"meeting_status": "ended",
                                                  "can_rerun_speakers": True}}}],
    "result-failed": [{"active": False, "status": "ended", "dashboard_available": True,
                       **IDENTITY, "finalization": {
                           "status": "failed", "message": "Final insights failed.",
                           "current_step": 4, "total_steps": 4,
                           "step_details": "Re-decoding failed; kept live transcript",
                           "steps": _steps("failed", "completed", "completed", "completed",
                                           details={"redecode": "Re-decoding failed; kept "
                                                                "live transcript"}),
                           "content_summary": {"meeting_status": "ended",
                                               "can_rerun_speakers": False}}}],
    "empty-meeting": [{"active": False, "status": "ended", "meeting_id": "m_empty",
                       "display_title": "Morning stand-up", "started_at": "2026-11-14T08:30:00",
                       "ended_at": "2026-11-14T08:30:12", "finalization": {
                           "status": "completed", "message": "Final insights are ready.",
                           "content_summary": {"meeting_status": "ended", "is_empty": True,
                                               "has_audio": False, "has_transcript": False,
                                               "can_rerun_speakers": False}}}],
}


def _meeting_mode_tab(state):
    from types import SimpleNamespace

    import services.settings as settings_module
    from ui_qt.widgets import meeting_mode_tab as module
    from ui_qt.widgets.studio import GAP, scaled

    # Every pin stays for the render: show() runs refresh_ai_destination and the recent load.
    module.time = SimpleNamespace(monotonic=lambda: T0)
    module._saved_switch = lambda setting: False
    module.ai_insights_destination = lambda settings=None: AI_DESTINATION
    settings_module.resolve_meeting_speech = lambda settings=None: SimpleNamespace(
        source="local", note="")
    tab = _keep(module.MeetingModeTab())
    tab._load_recent = lambda: None
    tab.live_dot._timer.timeout.disconnect()
    if state == "idle":
        tab.set_recent_meetings(RECENT_ROWS)
    for payload in MEETING_SCRIPTS[state]:
        tab.set_meeting_state(payload)
    # The page's margins (24, 6, 24, 16) around the split and the stage's height.
    width = tab.preferred_width() + 48
    height = scaled(module.STAGE_HEIGHT) + 22
    assert width == tab.split.column_width + GAP + scaled(module.STAGE_WIDTH) + 48
    return tab, (width, height), None


def _meeting_builder(state):
    return lambda variant: _meeting_mode_tab(state)


#: The History sidebar's width; the panel is that wide beside the tabs.
PAST_PANEL_SIZE = (380, 900)
PAST_TODAY = (2026, 3, 15)
EXPORT_ROOT = r"C:\Users\Dana\AppData\Local\OpenWhisper_CPP"


def _past_state(**fields):
    import json

    return json.dumps(fields)


def _past_row(meeting_id, title, status="ended", started="2026-03-14T09:30:00", minutes=42,
              paused=120, state=None, summary=None):
    from datetime import datetime, timedelta

    began = datetime.fromisoformat(started)
    row = {"id": meeting_id, "title": title, "status": status, "started_at": started,
           "ended_at": (began + timedelta(minutes=minutes)).isoformat(),
           "paused_total_s": paused}
    if state is not None:
        row["state_json"] = state
    if summary is not None:
        row["content_summary"] = summary
    return row


PAST_ROWS = [
    _past_row("m_ready", "Planning review",
              state=_past_state(finalization={"status": "completed"}),
              summary={"has_audio": True, "has_transcript": True,
                       "preview_text": "We should ship the sidebar polish next."}),
    _past_row("m_saved", "Deferred review", started="2026-03-13T15:00:00", minutes=25, paused=0,
              state=_past_state(finalization={"status": "failed", "card_deferred": True}),
              summary={"has_audio": True, "has_transcript": True,
                       "preview_text": "Notes were not finished before the meeting ended."}),
    _past_row("m_failed", "", status="failed", started="2026-03-12T14:00:00", minutes=0, paused=0,
              summary={"is_empty": True, "has_audio": False, "has_transcript": False}),
    {"id": "m_empty", "title": "Morning stand-up", "status": "ended",
     "started_at": "2026-03-11T08:30:00", "ended_at": "2026-03-11T08:30:20",
     "paused_total_s": 0,
     "content_summary": {"is_empty": True, "has_audio": False, "has_transcript": False,
                         "preview_text": ""}},
    _past_row("m_topic", "", started="2026-03-10T11:00:00", minutes=61, paused=0,
              state=_past_state(title="", topic={"current": "Quarterly roadmap"}),
              summary={"has_audio": True, "has_transcript": True,
                       "preview_text": "Let's start with last quarter's numbers."}),
    _past_row("m_live", "Running now", status="active", started="2026-03-15T09:00:00",
              summary={"has_audio": True, "has_transcript": True, "preview_text": "Hello."}),
]


def _pin_today():
    """Pins the calendar button's day."""
    import datetime as _datetime

    from ui_qt.widgets import history_calendar

    class PinnedDate(_datetime.date):
        @classmethod
        def today(cls):
            return cls(*PAST_TODAY)

    history_calendar.date = PinnedDate


def _openwhisper_cpp_labels(dialog):
    from PyQt6.QtWidgets import QLabel

    for label in dialog.findChildren(QLabel):
        if "OpenWhisper" in label.text():
            label.setText(label.text().replace("OpenWhisper", PRODUCT))


def _dialog_size(dialog):
    """The size the dialog opens at: its hint at the width it takes, clamped to its limits."""
    width = max(dialog.minimumWidth(), min(dialog.maximumWidth(), dialog.sizeHint().width()))
    layout = dialog.layout()
    height = dialog.sizeHint().height()
    if layout.hasHeightForWidth():
        height = max(height, layout.totalHeightForWidth(width))
    return width, height


def past_meetings_panel_list(variant):
    from ui_qt.widgets.past_meetings_panel import PastMeetingsPanel

    _pin_today()
    panel = _keep(PastMeetingsPanel(meeting_provider=lambda: PAST_ROWS))
    panel.refresh()
    panel.set_selected_meeting_id("m_saved")
    return panel, PAST_PANEL_SIZE, None


def _delete_dialog(has_audio):
    from ui_qt.dialogs.meeting_delete_dialog import MeetingDeleteDialog

    dialog = _keep(MeetingDeleteDialog(has_audio=has_audio))
    return dialog, _dialog_size(dialog), None


def meeting_delete_dialog_with_audio(variant):
    return _delete_dialog(True)


def meeting_delete_dialog_no_audio(variant):
    return _delete_dialog(False)


def meeting_recovery_dialog_two_rows(variant):
    from ui_qt.dialogs.meeting_recovery_dialog import MeetingRecoveryDialog
    from ui_qt.widgets import PrimaryButton

    dialog = _keep(MeetingRecoveryDialog([
        {"id": "m_crash_1", "title": "Weekly design review", "started_at": "2026-03-14T15:30:12"},
        {"id": "m_crash_2", "title": "", "started_at": "2026-03-13T09:00:00"},
    ]))
    _openwhisper_cpp_labels(dialog)
    dialog.findChildren(PrimaryButton)[0].click()
    return dialog, _dialog_size(dialog), None


def meeting_intro_dialog_first_visit(variant):
    from ui_qt.dialogs.meeting_intro_dialog import MeetingModeIntroDialog

    dialog = _keep(MeetingModeIntroDialog())
    return dialog, _dialog_size(dialog), None


EXPORT_MEETINGS = (
    # id, title, started
    ("m_export_1", "Weekly design review", "2026-03-14T15:30:00"),
    ("m_export_2", "Customer call with Acme", "2026-03-12T09:00:00"),
    ("m_export_3", "Quarterly roadmap sync", "2026-03-10T11:00:00"),
)


def meeting_export_dialog_three_meetings(variant):
    from PyQt6.QtCore import QDate

    from ui_qt.dialogs.meeting_export_dialog import MeetingExportDialog

    meetings = [{"id": meeting_id, "title": title, "status": "ended", "started_at": started,
                 "content_summary": {"has_transcript": True}}
                for meeting_id, title, started in EXPORT_MEETINGS]
    # The default path is made as the dialog is built: OpenWhisper_CPP's file name in a fixed folder.
    with (
        patch.object(MeetingExportDialog, "_DEFAULT_FILE_STEM", "openwhisper_cpp_meetings"),
        patch.object(MeetingExportDialog, "_export_root", lambda self: EXPORT_ROOT),
    ):
        dialog = _keep(MeetingExportDialog(meeting_provider=lambda: meetings))
    dialog.from_date.setDate(QDate(2026, 2, 15))
    dialog.to_date.setDate(QDate(2026, 3, 15))
    hint = dialog.sizeHint()
    width = max(dialog.minimumWidth(), min(dialog.maximumWidth(), hint.width()))
    return dialog, (width, hint.height()), None


# --- Meeting Mode settings (M7.18, fv_ui_meeting) ---
#
# The setup dialog is OpenWhisper's MeetingSetupDialog opened on the way to a meeting, its model
# inspection and audio check answered by the pinned results below (no device opens and nothing
# is read of this computer), each step's worker waited for as the dialog's own poll does:
#
#   meeting_setup_dialog/initial        the models inspected (the speaker model still needed),
#                                       audio not checked yet
#   meeting_setup_dialog/ready-check    every model ready and both channels heard
#   meeting_setup_dialog/unready-check  every model ready, the microphone heard and system audio
#                                       silent: "Start anyway" is offered
#
# The After the meeting and Dashboard page bodies are OpenWhisper's own, taken out of a real
# SettingsDialog as the Fast judgments page is above. OpenWhisper_CPP's re-transcription runs
# Parakeet, so its tile says so where OpenWhisper's says Whisper.
#
#   settings_meeting_after/report-on        every default: the final report and its views on
#   settings_meeting_after/report-off       the final report off, its views disabled
#   settings_meeting_dashboard/localhost    every default: this computer only, port Automatic
#   settings_meeting_dashboard/lan          shared on the local network, with the warning

SETUP_LABEL = "Parakeet TDT 0.6B v3"
SETUP_STATES = {
    "initial": ({"speaker_download": True}, None),
    "ready-check": ({}, ("Signal detected", "Signal detected")),
    "unready-check": ({}, ("Signal detected", "No signal detected")),
}


def _settle_setup(dialog):
    """Turns the event loop until the dialog's step has finished and shown its result."""
    import time

    from PyQt6.QtCore import QCoreApplication

    deadline = time.monotonic() + 10
    while dialog._future is not None and time.monotonic() < deadline:
        QCoreApplication.processEvents()
        time.sleep(0.01)
    assert dialog._future is None, "the setup step did not finish"
    QCoreApplication.processEvents()


def _meeting_setup_dialog(state):
    from services.meeting_setup import AudioCheck, ModelSetup
    from ui_qt.dialogs import meeting_setup as module

    model_fields, audio = SETUP_STATES[state]
    stack = ExitStack()
    stack.enter_context(patch.object(module, "inspect_models",
                                     lambda: ModelSetup(SETUP_LABEL, **model_fields)))
    stack.enter_context(patch.object(module, "system_audio_permission_required", lambda: False))
    if audio is not None:
        stack.enter_context(patch.object(module, "check_audio",
                                         lambda cancel, **_kwargs: AudioCheck(*audio)))
    _KEEP.append(stack)
    dialog = _keep(module.MeetingSetupDialog(starting=True))
    _settle_setup(dialog)
    if audio is not None:
        dialog.audio_button.click()
        _settle_setup(dialog)
    return dialog, _dialog_size(dialog), None


def _setup_builder(state):
    return lambda variant: _meeting_setup_dialog(state)


def _parakeet(page):
    """OpenWhisper_CPP's re-transcription runs Parakeet, where OpenWhisper's runs Whisper."""
    from ui_qt.widgets.wrapped_label import WrappedLabel

    for label in page.findChildren(WrappedLabel):
        if "run Whisper again" in label.text():
            label.setText(label.text().replace("run Whisper again", "run Parakeet again"))


def settings_meeting_after_report_on(variant):
    from ui_qt.dialogs.settings_destinations import MEETING_AFTER

    page, size, focus = _app_page(MEETING_AFTER)
    _parakeet(page)
    return page, (PAGE_WIDTH, _settled_height(page, PAGE_WIDTH)), focus


def settings_meeting_after_report_off(variant):
    from ui_qt.dialogs.settings_destinations import MEETING_AFTER

    page, size, focus = _app_page(MEETING_AFTER, {"meeting_end_report": False})
    _parakeet(page)
    return page, (PAGE_WIDTH, _settled_height(page, PAGE_WIDTH)), focus


def settings_meeting_dashboard_localhost(variant):
    from ui_qt.dialogs.settings_destinations import MEETING_DASHBOARD

    return _app_page(MEETING_DASHBOARD)


def settings_meeting_dashboard_lan(variant):
    from ui_qt.dialogs.settings_destinations import MEETING_DASHBOARD

    return _app_page(MEETING_DASHBOARD, {"meeting_server_bind": "lan"})

BUILDERS = {
    "settings_fast_judgments_page/off-no-key": settings_fast_judgments_page_off_no_key,
    "settings_fast_judgments_page/text-model": settings_fast_judgments_page_text_model,
    **{f"meeting_mode_tab/{state}": _meeting_builder(state) for state in MEETING_SCRIPTS},
    "past_meetings_panel/list": past_meetings_panel_list,
    "meeting_delete_dialog/with-audio": meeting_delete_dialog_with_audio,
    "meeting_delete_dialog/no-audio": meeting_delete_dialog_no_audio,
    "meeting_recovery_dialog/two-rows": meeting_recovery_dialog_two_rows,
    "meeting_intro_dialog/first-visit": meeting_intro_dialog_first_visit,
    "meeting_export_dialog/three-meetings": meeting_export_dialog_three_meetings,
    **{f"meeting_setup_dialog/{state}": _setup_builder(state) for state in SETUP_STATES},
    "settings_meeting_after/report-on": settings_meeting_after_report_on,
    "settings_meeting_after/report-off": settings_meeting_after_report_off,
    "settings_meeting_dashboard/localhost": settings_meeting_dashboard_localhost,
    "settings_meeting_dashboard/lan": settings_meeting_dashboard_lan,
}
