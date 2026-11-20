"""Reference builders of a16's windows in fv_ui_setup: the guided setup window (M2.26) and the
update dialog (M3.6).

Each builder returns (widget, (width, height), focus widget or None) with fixed fake data.

The window is OpenWhisper's GuidedSetupDialog over a real SettingsDialog, as
tests/test_guided_setup.py's make_setup builds it: a throwaway settings file in the isolated
data folder holding OpenWhisper's defaults on Windows (no guided_setup_state yet), no
microphones, Parakeet downloaded (speech_model_downloaded answers True), no cleanup provider
(cleanup is off by default, so provider_summary is never asked). FioriVoce differs where these
builders put its own in: the product name in copy is FioriVoce's, and the app mark is the one
FioriVoce draws (OpenWhisper's drawn fallback, app_icon.render_app_pixmap at every ICON_SIZES
size) where OpenWhisper shows its bundled .ico, and the Clean up page's provider row and privacy
text are hidden, as FioriVoce hides them until M4 provides the provider summary (that page is in
no image, but its hidden widgets are in every tree). The default size is 720x690; the window's own
show-time fit to the screen leaves a fixed size as it is.

welcome is the offer's first page; record, output and ready are the pages a File > Guided setup
pass shows (begin(welcome=False) saves "started"), focus on Continue (Get started, Finish) or,
on ready, on the try box, as _focus_page puts it. cleanup (M4.14) is the Clean up step of such a
pass over saved settings with AI cleanup on at Medium through OpenRouter's free model, whose key
resolve_credential reports saved: its provider row and privacy text show, as FioriVoce's
composition draws them with its provider summary.

The update dialog (app_update_dialog/<state>) shows a fixed check result: running copy 0.1.0 of
the installer channel, a release 0.2.0 (not a prerelease) whose setup asset is 52,428,800 bytes,
and a fixed Markdown body whose "Checksums" section the notes renderer drops. States: available
(the release can be applied), available-notify (its setup asset has no digest, so it cannot),
up-to-date (latest release 0.1.0), development (latest release 0.0.9), check-failed (no result),
downloading (20,971,520 of 52,428,800 bytes, the bar at its end value and its sheen at the start)
and error ("The download failed.", no setup offer).
The product name in the dialog's labels and title is FioriVoce's, and the window is the size its
own sizeHint asks for once the notes are fitted, scrolled to the top.
"""

from contextlib import ExitStack
from unittest.mock import patch

PRODUCT = "FioriVoce"
SIZE = (720, 690)

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


def _drawn_icon():
    from PyQt6.QtGui import QIcon

    from ui_qt.utils.app_icon import ICON_SIZES, render_app_pixmap

    icon = QIcon()
    for size in ICON_SIZES:
        icon.addPixmap(render_app_pixmap(size))
    return icon


def _rebrand(dialog):
    """FioriVoce's name in every label, now and whenever refresh() sets one again."""
    from PyQt6.QtWidgets import QLabel

    from ui_qt.widgets.wrapped_label import WrappedLabel

    dialog.setWindowTitle(dialog.windowTitle().replace("OpenWhisper", PRODUCT))
    for label in dialog.findChildren(QLabel):
        if isinstance(label, WrappedLabel):
            original = label.setText
            label.setText = (lambda text, _set=original:
                             _set(text.replace("OpenWhisper", PRODUCT)))
            label.setText(label.text())
        elif "OpenWhisper" in label.text():
            label.setText(label.text().replace("OpenWhisper", PRODUCT))


def _setup(page, settings=None, provider_row=False):
    import os

    from PyQt6.QtWidgets import QLabel

    from services import guided_setup as service
    from services.settings import SettingsManager
    from ui_qt.dialogs import guided_setup as module
    from ui_qt.dialogs import settings_dialog as settings_dialog_module
    from ui_qt.dialogs import settings_downloads as downloads_module
    from ui_qt.dialogs import settings_models as models_module

    folder = os.path.join(os.environ["OPENWHISPER_DATA_DIR"], "a16")
    os.makedirs(folder, exist_ok=True)
    store = SettingsManager(os.path.join(folder, f"settings{len(_KEEP)}.json"))
    store.save_all_settings(dict(settings or {}))
    stack = ExitStack()
    for target in (settings_dialog_module, models_module, downloads_module, module, service):
        stack.enter_context(patch.object(target, "settings_manager", store))
    stack.enter_context(patch.object(settings_dialog_module.history_manager, "set_retention"))
    for target in (models_module, downloads_module):
        stack.enter_context(patch.object(target, "scan_cached_models", return_value={}))
    stack.enter_context(patch("services.local_asr.cache.inventory", return_value={}))
    stack.enter_context(patch.object(settings_dialog_module.AudioRecorder, "get_input_devices",
                                     staticmethod(lambda: [])))
    stack.enter_context(
        patch.object(settings_dialog_module, "is_native_wayland_session", return_value=False))
    stack.enter_context(patch.object(settings_dialog_module.QSystemTrayIcon,
                                     "isSystemTrayAvailable", return_value=True))
    stack.enter_context(patch.object(service, "speech_model_downloaded", return_value=True))
    stack.enter_context(patch.object(module, "app_icon", _drawn_icon))
    if provider_row:
        # provider_summary's key check: OpenRouter's key is saved (no key store is read).
        stack.enter_context(patch.object(module, "resolve_credential", return_value="key"))
    _KEEP.append(stack)

    built = []

    def get_settings():
        built.append(_keep(settings_dialog_module.SettingsDialog(background_cache_scan=False)))
        return built[-1]

    nothing = lambda *args, **kwargs: None  # noqa: E731
    dialog = module.GuidedSetupDialog(get_settings, open_destination=nothing,
                                      open_all_settings=nothing, download_model=nothing,
                                      manager=store)
    _keep(dialog)
    _rebrand(dialog)
    # The cleanup provider row and the privacy text show only where FioriVoce's window is given
    # a provider summary, as the app's composition gives it; the page images other than cleanup
    # are of a window built without one, so they are hidden there (they sit on the Clean up page).
    if not provider_row:
        provider_title = dialog.provider_detail.parentWidget().findChildren(
            QLabel, "guidedRowTitle")
        for widget in [dialog.privacy, dialog.provider_detail, dialog.provider_button] + [
                label for label in provider_title if label.text() == "Cleanup provider"]:
            widget.setVisible(False)
    dialog.begin(welcome=page == module.WELCOME)
    if page not in (module.WELCOME, module.RECORD):
        dialog.show_page(page)
    focus = dialog.try_box if page == module.READY else dialog.next_button
    return dialog, SIZE, focus


def welcome(variant):
    from ui_qt.dialogs import guided_setup as module

    return _setup(module.WELCOME)


def record(variant):
    from ui_qt.dialogs import guided_setup as module

    return _setup(module.RECORD)


def output(variant):
    from ui_qt.dialogs import guided_setup as module

    return _setup(module.OUTPUT)


def ready(variant):
    from ui_qt.dialogs import guided_setup as module

    return _setup(module.READY)


#: The Clean up step with AI cleanup on at Medium through OpenRouter's free model, whose key is
#: saved: the provider row and the privacy sentence show.
CLEANUP_SETTINGS = {
    "transcript_cleanup_enabled": True,
    "transcript_cleanup_level": "medium",
    "transcript_cleanup_provider": "openrouter",
    "transcript_cleanup_model": "openrouter/free",
}


def cleanup(variant):
    from ui_qt.dialogs import guided_setup as module

    return _setup(module.CLEAN_UP, CLEANUP_SETTINGS, provider_row=True)


UPDATE_NOTES = """## What's new

- Dictation starts faster, with Parakeet running in the app
- The `Settings` window opens on the page you left it on
- Read the [full history](https://example.invalid/history) of changes

## Fixes

- Pasting no longer drops the first character

## Checksums

SHA-256: """ + "ab" * 32 + "\n"

SETUP_SIZE = 52_428_800


def _update_result(state):
    """The check result of one update dialog state (see the module docstring's states)."""
    from services.app_update import (ApplyMode, InstallChannel, ReleaseAsset, ReleaseInfo,
                                     UpdateCheckResult, UpdateStatus)

    if state == "check-failed":
        return None
    version = {"up-to-date": "0.1.0", "development": "0.0.9"}.get(state, "0.2.0")
    status = {"up-to-date": UpdateStatus.UP_TO_DATE,
              "development": UpdateStatus.DEVELOPMENT}.get(state, UpdateStatus.UPDATE_AVAILABLE)
    # available-notify: the setup asset has no digest, so the release cannot be applied here.
    digest = None if state == "available-notify" else "ab" * 32
    applies = state in ("available", "downloading", "error")
    asset = ReleaseAsset(
        url=f"https://example.invalid/releases/download/v{version}/FioriVoce-Setup-{version}.exe",
        name=f"FioriVoce-Setup-{version}.exe", size_bytes=SETUP_SIZE, sha256=digest)
    return UpdateCheckResult(
        status=status, current_version="0.1.0", channel=InstallChannel.INSTALLER,
        release=ReleaseInfo(version=version, tag_name=f"v{version}",
                            html_url=f"https://example.invalid/releases/tag/v{version}",
                            notes=UPDATE_NOTES, setup_asset=asset, prerelease=False),
        can_apply=applies, apply_mode=ApplyMode.SETUP if applies else ApplyMode.NOTIFY_ONLY)


def _rebrand_labels(dialog):
    """FioriVoce's name in every label, now and whenever the dialog sets one again."""
    from PyQt6.QtWidgets import QLabel

    dialog.setWindowTitle(dialog.windowTitle().replace("OpenWhisper", PRODUCT))
    for label in dialog.findChildren(QLabel):
        original = label.setText
        label.setText = (lambda text, _set=original: _set(text.replace("OpenWhisper", PRODUCT)))
        label.setText(label.text())


def _fit_natural_size(dialog):
    """The window's natural size: what the dialog's own sizeHint asks for once its notes have
    been fitted to the width it ends up at (they change the height, and so the hint)."""
    from PyQt6.QtCore import QCoreApplication

    dialog.show()
    QCoreApplication.processEvents()
    for _ in range(6):
        dialog.layout().activate()
        hint = dialog.sizeHint()
        if (hint.width(), hint.height()) == (dialog.width(), dialog.height()):
            break
        dialog.resize(hint)
        QCoreApplication.processEvents()
    return dialog.width(), dialog.height()


def _update_dialog(state):
    from ui_qt.dialogs.app_update_dialog import AppUpdateDialog

    error = "The update server could not be reached." if state == "check-failed" else ""
    dialog = _keep(AppUpdateDialog(_update_result(state), error=error))
    _rebrand_labels(dialog)
    if state in ("downloading", "error"):
        dialog.show()
        dialog._set_downloading()
        if state == "downloading":
            dialog.set_progress("downloading", 20_971_520, SETUP_SIZE)
            # Driven to its end value and held: the bar eases and its sheen travels on a timer,
            # so a render would otherwise show however many ticks happened to run.
            bar = dialog.progress_bar
            bar._timer.stop()
            bar._phase = 0.0
            bar.set_fraction(20_971_520 / SETUP_SIZE, animate=False)
        else:
            dialog.set_error("The download failed.")
    width, height = _fit_natural_size(dialog)
    dialog.notes_view.verticalScrollBar().setValue(0)
    return dialog, (width, height), None


def _update_builder(state):
    return lambda variant: _update_dialog(state)


BUILDERS = {
    "guided_setup/welcome": welcome,
    "guided_setup/record": record,
    "guided_setup/output": output,
    "guided_setup/ready": ready,
    "guided_setup/cleanup": cleanup,
    **{f"app_update_dialog/{state}": _update_builder(state)
       for state in ("available", "available-notify", "up-to-date", "development",
                     "check-failed", "downloading", "error")},
}
