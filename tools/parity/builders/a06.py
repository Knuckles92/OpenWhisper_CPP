"""Reference builders of the a06 Hotkeys page (fv_ui_hotkeys, M2.17).

Each builder returns (widget, (width, height), focus widget or None) with fixed fake data. The
page is OpenWhisper's own, built by a SettingsDialog over a throwaway settings file in the
isolated data folder (out/parity/ow-data), the way tests/fakes/personalize.py builds one, then
taken out of the dialog: FioriVoce's HotkeysPage is the page body the Settings shell hosts.

FioriVoce differs from OpenWhisper in three places these builders put FioriVoce's in:
- the page body has no object name (settingsPage_<key> is the shell's wrapper);
- the shortcut fields carry no `optional` property (no theme rule reads it; lead decision 7);
- the product name in the last group's title and its Enable or disable row is FioriVoce's.

"No bindings" is an empty profile, transform and action library, so the three groups show their
empty texts (profiles and transforms arrive with M4, actions with M5). Global captures are a
stand-in that never hooks the keyboard. Nothing here runs an event loop of its own beyond the
processEvents a size measurement needs.
"""

import os
from contextlib import ExitStack
from unittest.mock import MagicMock, patch

PRODUCT = "FioriVoce"
PAGE_WIDTH = 640
NO_BINDINGS = {"transcript_cleanup_profiles": [], "text_transforms": [], "actions": []}

# Patches and dialogs live as long as the process: the references are grabbed after the
# builders return.
_KEEP = []
# The widgets the builders made, deleted at exit while the application still exists: left to
# interpreter teardown, after render_reference.py's QApplication had gone, they crashed the
# process once the references were written.
_WIDGETS = []
_APP = []


def _release():
    from PyQt6 import sip

    for widget in reversed(_WIDGETS):
        if not sip.isdeleted(widget):
            sip.delete(widget)
    _WIDGETS.clear()
    for stack, _dialog_kept in reversed(_KEEP):
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


class _FakeCaptureThread:
    """Stands in for the global capture thread, which would hook the real keyboard."""

    def __init__(self, _parent=None):
        self.captured = MagicMock()
        self.cancelled = MagicMock()
        self.failed = MagicMock()
        self.finished = MagicMock()
        self.stopped = False

    def start(self):
        pass

    def deleteLater(self):
        pass

    def isRunning(self):
        return not self.stopped

    def stop(self):
        self.stopped = True

    def wait(self, _timeout):
        return True


def _dialog(values):
    from services.settings import SettingsManager
    from ui_qt.dialogs import settings_dialog as settings_dialog_module
    from ui_qt.dialogs import settings_downloads as downloads_module
    from ui_qt.dialogs import settings_models as models_module

    folder = os.path.join(os.environ["OPENWHISPER_DATA_DIR"], "a06")
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
        patch.object(settings_dialog_module, "HotkeyCaptureThread", _FakeCaptureThread))
    stack.enter_context(
        patch.object(settings_dialog_module, "is_native_wayland_session", return_value=False))
    dialog = settings_dialog_module.SettingsDialog(background_cache_scan=False)
    dialog.on_profile_hotkey_capture = MagicMock()
    _KEEP.append((stack, _keep(dialog)))
    return dialog


def _fiorivoce(page):
    """The page as FioriVoce builds it (see the module docstring)."""
    from PyQt6.QtWidgets import QLabel

    from ui_qt.widgets.hotkey_capture import HotkeyCaptureInput

    page.setObjectName("")
    for field in page.findChildren(HotkeyCaptureInput):
        field.setProperty("optional", None)
    for label in page.findChildren(QLabel):
        if "OpenWhisper" in label.text():
            label.setText(label.text().replace("OpenWhisper", PRODUCT))


def _page(values):
    """The built Hotkeys page body, out of its dialog and scroll area, and the dialog."""
    from ui_qt.dialogs.settings_destinations import HOTKEYS

    dialog = _dialog({**NO_BINDINGS, **values})
    dialog.ensure_page(HOTKEYS)
    dialog._page_scrolls[HOTKEYS].takeWidget()
    page = dialog._pages[HOTKEYS]
    page.setParent(None)
    _keep(page)
    _fiorivoce(page)
    return page, dialog


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


def hotkeys_page_defaults(variant):
    page, _dialog_kept = _page({})
    return page, (PAGE_WIDTH, _settled_height(page, PAGE_WIDTH)), None


def hotkeys_page_push_hold(variant):
    page, _dialog_kept = _page({"recording_trigger_mode": "push_hold"})
    return page, (PAGE_WIDTH, _settled_height(page, PAGE_WIDTH)), None


def hotkey_shortcut_card_conflict(variant):
    """Command Mode's card after a capture of the record shortcut was refused."""
    from PyQt6.QtCore import QCoreApplication

    page, dialog = _page({})
    field = dialog.hotkey_inputs["command_mode"]
    dialog._start_hotkey_capture("command_mode", field)
    dialog._on_hotkey_captured(dialog.capture_thread, dialog.current_hotkeys["record_toggle"])
    card = field.parentWidget()
    page.resize(PAGE_WIDTH, 4000)
    page.show()
    for _ in range(3):
        QCoreApplication.processEvents()
    height = card.height()
    page.hide()
    card.setParent(None)
    _keep(card)
    return card, (PAGE_WIDTH, height), None


BUILDERS = {
    "hotkeys_page/defaults": hotkeys_page_defaults,
    "hotkeys_page/push-hold": hotkeys_page_push_hold,
    "hotkey_shortcut_card/conflict": hotkey_shortcut_card_conflict,
}
