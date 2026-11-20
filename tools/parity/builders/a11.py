"""Reference builders of the a11 Settings shell (fv_ui_settings, M2.11).

Each builder returns (widget, (width, height), focus widget or None) with fixed fake data.

The shell is OpenWhisper's SettingsDialog as it is, with every page replaced by the same
stand-in FioriVoce's test registers (tests/ui_settings/settings_support.h): the page's own
container holding a group title and two tiles, added through _add_page and _tile_group. The
pages, their loads, the rail values and Home's refresh are OpenWhisper's real code otherwise, so
the builder switches off what reads a page that is not there:
- _load_settings, _render_rail_values, _refresh_home, refresh_models and
  _refresh_recordings_usage do nothing (the stand-ins bind no setting);
- the rail's set_value does nothing, so the rail shows no values, as FioriVoce's stand-ins have
  none (the model assignments write Voice model's value themselves);
- the search palette, which FioriVoce builds with Home (M2.24), is taken out of the tree;
- the stack holds the pages in rail order, as FioriVoce adds them, not in _add_page order.

FioriVoce's product name is in the header, and its app mark, which it draws, where OpenWhisper
shows its bundled .ico. The screen is pinned (a fake one), so the first-show fit sizes the window
the same way on both sides: 1920x1080 for the shell, and for the narrow variant a screen whose
room after the 32 px margins is the window itself, which lowers the 940 px minimum to 780.
"""

from contextlib import ExitStack
from unittest.mock import patch

PRODUCT = "FioriVoce"
GROUP_TITLE = "Stand-in group"
TILES = (
    ("First tile", "A tile of the page that registers here."),
    ("Second tile", "Another tile of the same page."),
)
SHELL_SIZE = (1315, 850)
NARROW_SIZE = (780, 850)
MARGIN = 32


class _Screen:
    def __init__(self, rect):
        self._rect = rect

    def availableGeometry(self):
        return self._rect


def _stand_in(dialog, layout):
    from ui_qt.widgets import InfoTile

    tiles = [InfoTile(title, description) for title, description in TILES]
    dialog._tile_group(layout, GROUP_TITLE, tiles)


def _dialog(screen_size):
    from PyQt6.QtCore import QRect
    from PyQt6.QtGui import QPixmap
    from PyQt6.QtWidgets import QLabel

    from ui_qt.dialogs import settings_dialog as module
    from ui_qt.dialogs.settings_destinations import RECORDING
    from ui_qt.utils.app_icon import render_app_pixmap

    cls = module.SettingsDialog
    original_add_page = cls._add_page

    def add_stand_in(self, key, title, subtitle, builder, *, scroll=True):
        original_add_page(self, key, title, subtitle,
                          lambda layout, self=self: _stand_in(self, layout), scroll=scroll)

    from ui_qt.widgets.nav_rail import NavRail

    nothing = lambda self, *args, **kwargs: None  # noqa: E731
    with patch.object(NavRail, "set_value", nothing), \
            patch.object(cls, "_add_page", add_stand_in), \
            patch.object(cls, "_load_settings", nothing), \
            patch.object(cls, "_render_rail_values", nothing), \
            patch.object(cls, "_refresh_home", nothing):
        dialog = cls(get_loaded_model=lambda: None, background_cache_scan=False)
    # Instance attributes, read from Python by the methods that call them.
    dialog.refresh_models = lambda: None
    dialog._refresh_recordings_usage = lambda: None
    dialog._render_rail_values = lambda settings: None
    dialog._refresh_home = lambda: None
    dialog._load_settings = lambda: None
    dialog.rail.set_value = lambda key, text: None
    width, height = screen_size
    dialog.screen = lambda: _Screen(QRect(0, 0, width, height))

    dialog.search_palette.setParent(None)
    dialog.settings_brand.setText(PRODUCT)
    header = dialog.findChild(QLabel, "settingsViewBrand").parentWidget()
    icon = next(label for label in header.findChildren(QLabel) if not label.objectName())
    icon.setPixmap(QPixmap(render_app_pixmap(32)))

    pages = [dialog._page_scrolls.get(key, dialog._pages[key]) for key in dialog.rail.keys()]
    for page in pages:
        dialog.stack.removeWidget(page)
    for page in pages:
        dialog.stack.addWidget(page)

    dialog.select_destination(RECORDING)
    return dialog


def shell(variant):
    return _dialog((1920, 1080)), SHELL_SIZE, None


def narrow(variant):
    width, height = NARROW_SIZE
    return _dialog((width + 2 * MARGIN, height + 2 * MARGIN)), NARROW_SIZE, None


# --- The App pages (M2.19) ---
#
# Each page body is OpenWhisper's own, built by a real SettingsDialog over a throwaway settings
# file in the isolated data folder (as builders/a06.py builds the Hotkeys page), then taken out
# of the dialog at PAGE_WIDTH: FioriVoce's page class is the body the Settings shell hosts, and
# the shell around it is settings_dialog/shell's. FioriVoce differs where these builders put its
# own in: the body has no object name (settingsPage_<key> is the shell's wrapper), the product
# name in copy is FioriVoce's, and the live-preview tile names Parakeet's preview only. The
# system tray is present, as on Windows.

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


def _app_dialog(values, **options):
    import os
    from contextlib import ExitStack

    from services.settings import SettingsManager
    from ui_qt.dialogs import settings_dialog as settings_dialog_module
    from ui_qt.dialogs import settings_downloads as downloads_module
    from ui_qt.dialogs import settings_models as models_module

    # The page reads credential variables from the environment: none of this process's may
    # reach a reference (nor be read at all).
    for name in [name for name in os.environ if name.upper().endswith("_API_KEY")]:
        del os.environ[name]
    folder = os.path.join(os.environ["OPENWHISPER_DATA_DIR"], "a11")
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
    return _keep(settings_dialog_module.SettingsDialog(background_cache_scan=False, **options))


def _fiorivoce(page):
    """The page body as FioriVoce builds it (see above)."""
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


def _app_page(key, values=None, prepare=None):
    dialog = _app_dialog(values or {})
    dialog.ensure_page(key)
    if prepare is not None:
        prepare(dialog)
    dialog._page_scrolls[key].takeWidget()
    page = dialog._pages[key]
    page.setParent(None)
    _keep(page)
    _fiorivoce(page)
    return page, (PAGE_WIDTH, _settled_height(page, PAGE_WIDTH)), None


def settings_general_default(variant):
    from ui_qt.dialogs.settings_destinations import GENERAL

    return _app_page(GENERAL)


def settings_developer_default(variant):
    from ui_qt.dialogs.settings_destinations import ADVANCED

    return _app_page(ADVANCED)


# --- The Walkthroughs page (M6.9) ---
#
# The page body is OpenWhisper's own, built as the App pages are, with the installed-agent scan
# pinned: Settings marks the agents the last scan did not find, so cached_agents answers what
# the variant says instead of the owner's computer. The shortcut rows show the platform's default
# shortcuts, and their fields carry no `optional` property (as in builders/a06.py).
#
#   settings_walkthroughs_page/defaults        every setting default, all three agents found
#   settings_walkthroughs_page/write-up-codex  the AI write-up on with Codex chosen, which the
#                                              scan did not find: "Codex (not installed)"

#: Which agents the pinned scan found; the others were looked for and are missing.
FOUND_AGENTS = {"claude_code": True, "codex": True, "opencode_cli": True}
MISSING_CODEX = {"claude_code": True, "codex": None, "opencode_cli": True}


def _walkthroughs_page(values, found):
    from ui_qt.dialogs.settings_destinations import WALKTHROUGHS
    from ui_qt.widgets import agent_picker

    from ui_qt.widgets.hotkey_capture import HotkeyCaptureInput

    with patch.object(agent_picker, "cached_agents", lambda: found):
        page, size, focus = _app_page(WALKTHROUGHS, values)
    # As the Hotkeys page's builder (a06) does: FioriVoce's shortcut fields carry no `optional`
    # property, which no theme rule reads (lead decision 7).
    for field in page.findChildren(HotkeyCaptureInput):
        field.setProperty("optional", None)
    return page, size, focus


def settings_walkthroughs_page_defaults(variant):
    return _walkthroughs_page({}, FOUND_AGENTS)


def settings_walkthroughs_page_write_up_codex(variant):
    return _walkthroughs_page(
        {"walkthrough_write_up": True, "walkthrough_write_up_agent": "codex"}, MISSING_CODEX)


#: The saved OpenAI key: masked, it shows its last four characters.
SAVED_KEY = "sk-test-key-0000001234"


def settings_api_keys_saved(variant):
    from services import credentials
    from ui_qt.dialogs.settings_destinations import API_KEYS

    credentials.store().set("OPENAI_API_KEY", SAVED_KEY)
    return _app_page(API_KEYS)


#: The text-model owners: OpenRouter's key is saved and selected, and a custom endpoint, "Team
#: gateway", sends TEAM_KEY. The combo is closed, so it names the selected row only; the status
#: line shows the saved key masked.
OPENROUTER_SAVED_KEY = "sk-or-test-key-0000005678"
TEXT_OWNER_SETTINGS = {"text_llm_profiles": [
    {"id": "custom_team", "name": "Team gateway", "base_url": "https://llm.example.test/v1",
     "api_key_env": "TEAM_KEY"},
]}


def settings_api_keys_text_owners(variant):
    from services import credentials
    from ui_qt.dialogs.settings_destinations import API_KEYS

    credentials.store().set("OPENROUTER_API_KEY", OPENROUTER_SAVED_KEY)

    def select(dialog):
        combo = dialog.api_key_combo
        combo.setCurrentIndex(combo.findData("OPENROUTER_API_KEY"))

    return _app_page(API_KEYS, TEXT_OWNER_SETTINGS, select)


#: Recording's microphones: the dumped Windows device list (tests/fixtures/audio), the USB
#: microphone preferred, then Blue Snowball and a DirectSound headset as backups.
DEVICES_FIXTURE = "tests/fixtures/audio/devices-windows.json"
MICROPHONES = [
    {"name": "Microphone (2- USB Audio Device", "hostapi": "MME"},
    {"name": "Microphone (Blue Snowball )", "hostapi": "MME"},
    {"name": "Headset (soundcore V20i)", "hostapi": "Windows DirectSound"},
]
#: Two saved recordings, 806 KB in all.
RECORDINGS_USAGE = (2, 825344)
#: The live-preview tile names Parakeet's preview only (FioriVoce has no Nemotron or Whisper).
LIVE_PREVIEW_DESCRIPTION = (
    "Shows text as you speak on the near-cursor overlay. Parakeet transcribes short audio "
    "chunks with the loaded model. The final transcript uses your selected model and follows "
    "the General paste and clipboard settings. Its text size is under Appearance."
)


class _FixtureDevices:
    """sounddevice's query functions over a dumped device list."""

    def __init__(self, dump):
        self._dump = dump
        self.default = type("Default", (), {})()
        self.default.device = (dump["default"]["input"], dump["default"]["output"])
        self.default.hostapi = dump["default"]["hostapi"]

    def query_devices(self):
        return [dict(device) for device in self._dump["devices"]]

    def query_hostapis(self, index=None):
        apis = [dict(api) for api in self._dump["hostapis"]]
        return apis if index is None else apis[index]


def _wait(predicate):
    from PyQt6.QtCore import QCoreApplication, QElapsedTimer

    timer = QElapsedTimer()
    timer.start()
    while not predicate() and timer.elapsed() < 10000:
        QCoreApplication.processEvents()
    if not predicate():
        raise RuntimeError("The page's background work did not finish.")


def settings_recording_backups(variant):
    import json
    from pathlib import Path

    from services import audio_devices
    from ui_qt.dialogs import settings_dialog as settings_dialog_module
    from ui_qt.dialogs.settings_destinations import RECORDING

    dump = json.loads((Path(__file__).resolve().parents[3] / DEVICES_FIXTURE).read_text("utf-8"))
    devices = audio_devices.list_input_devices(_FixtureDevices(dump))
    stack = ExitStack()
    stack.enter_context(patch.object(settings_dialog_module.AudioRecorder, "get_input_devices",
                                     staticmethod(lambda: list(devices))))
    stack.enter_context(patch.object(settings_dialog_module.history_manager,
                                     "get_recordings_usage", return_value=RECORDINGS_USAGE))
    _KEEP.append(stack)

    def settled(dialog):
        _wait(lambda: dialog._microphones.devices is not None
              and bool(dialog.recordings_usage_label.text()))
        dialog.streaming_enabled_tile.set_description(LIVE_PREVIEW_DESCRIPTION)

    return _app_page(RECORDING, {"audio_input_priority": MICROPHONES}, settled)


#: The Appearance page's look preview, held as builders/a07.py holds recording_look_preview/bars:
#: the overlay's clock pinned at PREVIEW_NOW, the particles' hue clock at PREVIEW_HUE_TIME, and
#: the sample voice fed frame by frame up to PREVIEW_T seconds into the loop, every timer stopped.
PREVIEW_NOW = 1000.0
PREVIEW_HUE_TIME = 3.0
PREVIEW_T = 2.0


def settings_appearance_bars(variant):
    """Appearance with the Bars look, its preview shown once measured and then held still.

    Measuring shows and hides the page, which starts and stops the preview. The page is shown
    again here, before the harness shows it, so the entry the preview replays on show happens
    first; its timers are then stopped and the sample voice fed.
    """
    import types

    from ui_qt.dialogs.settings_destinations import APPEARANCE
    from ui_qt.overlays import waveform_overlay
    from ui_qt.widgets.recording_look_preview import sample_level

    waveform_overlay.time = types.SimpleNamespace(monotonic=lambda: PREVIEW_NOW)
    previews = []

    def held(dialog):
        preview = dialog.recording_look_preview
        # The language chip reads the dictation languages; the reference shows none.
        preview.overlay._refresh_language = lambda: None
        previews.append(preview)

    page, size, focus = _app_page(APPEARANCE, {"recording_overlay_look": "bars"}, held)
    preview = previews[0]
    overlay = preview.overlay
    overlay.style.animation_time = PREVIEW_HUE_TIME
    page.resize(*size)
    page.show()
    preview._timer.stop()
    overlay.timer.stop()
    overlay.hidden_timer.stop()
    overlay._caption_timer.stop()
    for frame in range(round(PREVIEW_T * 30)):
        overlay._ribbon.hear(sample_level(frame / 30))
        overlay._ribbon.advance(1 / 30)
    overlay._listen_started = PREVIEW_NOW - PREVIEW_T
    return page, size, focus


# --- Settings > Home (M2.24) ---
#
# Home's body is OpenWhisper's own HomePage, built by a real SettingsDialog (as the App pages
# above), taken out of its page at HOME_WIDE or HOME_NARROW, and cut to what FioriVoce's Home
# shows with the M2 pages registered:
# - the Dictation card loses the AI cleanup rows and status line (the AI cleanup page, M4, adds
#   them through HomeRows);
# - the Personalize and Meetings cards keep their header and closing link, both hidden: they have
#   no rows and their pages (M4, M7) are not registered;
# - the where strip, the Storage card and the footer show the summary FioriVoce composes from the
#   test's fixed sources (Parakeet downloaded, 714 MB; the CPU runtime installed, the CUDA one
#   not; no API key), not OpenWhisper's model manager;
# - the voice combo lists FioriVoce's engines only;
# - the note is FioriVoce's, under "New in FioriVoce".
# Home is not refreshed after that (its refresh would read the rows taken out).

#: Two card columns at 100 % text; one at 130 %.
HOME_WIDE = 1000
#: One card column, the where strip stacked.
HOME_NARROW = 700
#: FioriVoce's engines (settings keys.inc choices of selected_model).
HOME_ENGINES = ("api", "parakeet", "remote")
#: The where strip with the default settings: OpenRouter, the default cleanup provider, is a cloud
#: one, and cleanup and reading text near the cursor are off. Learned rules and Recordings close
#: the local list, as OpenWhisper's _refresh_home ends it; the page shown here has no meeting
#: items.
HOME_LOCAL = ["Dictation voice", "Text near the cursor (off)", "Learned rules", "Recordings"]
HOME_CLOUD = ["AI cleanup (off)"]
HOME_STORAGE = (1, 3, 714_000_000, {"Parakeet": 714_000_000})
HOME_COMPONENTS = [("NVIDIA speech runtime (CPU)", True), ("NVIDIA speech runtime (CUDA)", False)]
#: FioriVoce's footer: API keys counts the text-model and speech-endpoint keys and TypeSafe's (six
#: rows), and the runtime is model_assignments' runtime_summary for the saved CPU build.
HOME_FOOTER = ("API keys: 0 of 6 set  ·  Runtime: Auto · CPU runtime not loaded  ·  "
               "Missing models: ask first")
HOME_NOTE = ("FioriVoce 0.1 is the C++ rewrite of OpenWhisper: the same dictation, uploads, "
             "history and settings, with Parakeet running in-process for faster starts and "
             "lower memory.")


def _home_row_items(card, title):
    """The divider before the row titled `title` and the row's layout, in card.rows."""
    from PyQt6.QtWidgets import QLabel

    layout = card.rows
    label = next(item for item in card.findChildren(QLabel, "homeRowTitle")
                 if item.text() == title)
    for index in range(layout.count()):
        row = layout.itemAt(index).layout()
        if row is not None and _holds(row, label):
            return index, row
    raise LookupError(title)


def _holds(layout, widget):
    for index in range(layout.count()):
        item = layout.itemAt(index)
        if item.widget() is widget:
            return True
        if item.layout() is not None and _holds(item.layout(), widget):
            return True
    return False


def _widgets_of(layout):
    found = []
    for index in range(layout.count()):
        item = layout.itemAt(index)
        if item.widget() is not None:
            found.append(item.widget())
        elif item.layout() is not None:
            found.extend(_widgets_of(item.layout()))
    return found


def _drop_home_row(card, title):
    """Takes a row (and the divider above it, or below the first row) out of a Home card."""
    index, row = _home_row_items(card, title)
    layout = card.rows
    widgets = _widgets_of(row)
    previous = layout.itemAt(index - 1).widget() if index > 0 else None
    if previous is not None and previous.objectName() == "homeDivider":
        layout.takeAt(index)
        layout.takeAt(index - 1)
        widgets.append(previous)
    else:
        layout.takeAt(index)
    card._rows = [(r, c) for r, c in card._rows if r is not row]
    card._stacked = None
    for widget in widgets:
        widget.setParent(None)
        widget.deleteLater()


def _empty_home_card(card):
    """Keeps a card's header and closing link (hidden) and takes every row out."""
    from PyQt6.QtWidgets import QPushButton

    layout = card.rows
    link = next(button for button in card.findChildren(QPushButton, "homeLink"))
    # Index 0 is the header, 1 its spacing.
    index = 2
    while index < layout.count():
        item = layout.itemAt(index)
        if item.widget() is link:
            index += 1
            continue
        layout.takeAt(index)
        widgets = [item.widget()] if item.widget() is not None else \
            _widgets_of(item.layout()) if item.layout() is not None else []
        for widget in widgets:
            widget.setParent(None)
            widget.deleteLater()
    card._rows = []
    link.hide()
    card.hide()


def _home(width):
    from PyQt6.QtCore import QCoreApplication

    from ui_qt.dialogs.settings_home import HomeSummary

    dialog = _app_dialog({})
    home = dialog.home
    dictation = home.cards["dictation"]
    _drop_home_row(dictation, "Clean up my dictation")
    _drop_home_row(dictation, "Cleanup level")
    home.cleanup_status.setParent(None)
    home.cleanup_status.deleteLater()
    _empty_home_card(home.cards["personalize"])
    _empty_home_card(home.cards["meetings"])
    # The voice combo lists FioriVoce's engines only (the Whisper, Moonshine, Qwen and Nemotron
    # ones are dropped); its width at large text follows its longest name.
    from PyQt6.QtCore import QSignalBlocker, Qt

    from config import config
    from ui_qt.widgets.speech_backend_picker import backend_picker_label, backend_picker_tooltip

    voice = home.voice_combo
    with QSignalBlocker(voice):
        voice.clear()
        for display in sorted(config.MODEL_CHOICES, key=str.casefold):
            value = config.MODEL_VALUE_MAP[display]
            if value not in HOME_ENGINES:
                continue
            voice.addItem(backend_picker_label(display), value)
            voice.setItemData(voice.count() - 1, backend_picker_tooltip(display),
                              Qt.ItemDataRole.ToolTipRole)
        voice.setCurrentIndex(max(0, voice.findData("parakeet")))
    home.intro.title_label.setText("New in " + PRODUCT)
    home.intro.set_description(HOME_NOTE)
    downloaded, total, size, by_backend = HOME_STORAGE
    home.update_summary(HomeSummary(
        local_items=list(HOME_LOCAL), cloud_items=list(HOME_CLOUD), storage_downloaded=downloaded,
        storage_total=total, storage_bytes=size, storage_by_backend=dict(by_backend),
        storage_checking=False, components=list(HOME_COMPONENTS), footer=HOME_FOOTER,
    ))
    dialog._refresh_home = lambda: None
    home.refresh = lambda *args, **kwargs: None
    dialog._page_scrolls["overview"].takeWidget()
    home.setParent(None)
    _keep(home)
    QCoreApplication.processEvents()
    return home, (width, _settled_height(home, width)), None


def settings_home_wide(variant):
    return _home(HOME_WIDE)


def settings_home_narrow(variant):
    return _home(HOME_NARROW)


# --- The search palette (M2.24) ---
#
# OpenWhisper's SearchPalette over a plain host of PALETTE_SIZE, opened on "mic". Its index is
# what FioriVoce's Settings window finds for that query with the M2 pages registered (Recording's
# registered "Preferred microphone" field and its heading), so the palette draws the same rows;
# OpenWhisper's own window would also list its pages FioriVoce does not have yet.

PALETTE_SIZE = (900, 520)
PALETTE_QUERY = "mic"


def _palette_entries():
    from ui_qt.dialogs.settings_search import SETTING, SearchEntry

    return [
        SearchEntry(SETTING, "Preferred microphone", "Dictation › Recording", "recording",
                    target_name="audio_device_tile", keywords="Used for dictation and meetings.",
                    icon="microphone-blue.svg"),
        SearchEntry(SETTING, "Recording",
                    "Dictation › Recording · Microphone, saved audio retention, and the "
                    "live preview.", "recording", icon="microphone-blue.svg"),
    ]


def settings_search_palette_microphone(variant):
    from PyQt6.QtWidgets import QWidget

    from ui_qt.dialogs.settings_search import SearchPalette

    from PyQt6.QtCore import QCoreApplication, QEvent

    host = _keep(QWidget())
    host.resize(*PALETTE_SIZE)
    palette = SearchPalette(host, _palette_entries)
    palette.open(PALETTE_QUERY)
    # Opening lists the results twice (the query's textChanged, then open's own run); the first
    # run's rows wait for deletion, never shown, and leave the tree before it is dumped.
    QCoreApplication.sendPostedEvents(None, QEvent.Type.DeferredDelete.value)
    return host, PALETTE_SIZE, palette.input


# --- Settings > Backup & restore (M3.9) ---
#
# The page is OpenWhisper's own BackupSettingsPage in the _SettingsPage container of a real
# SettingsDialog built with a fake coordinator, taken out of the dialog at BACKUP_WIDE or
# BACKUP_NARROW_WIDTH (the container is what FioriVoce's dialog builds around the page too). The
# coordinator's snapshot is fixed: the last backup of 2026-10-04T12:30:00Z, 734,003,200 bytes, in
# BACKUP_FOLDER; a daily schedule keeping 5, next due 2026-10-05 12:30 UTC; nothing inspected;
# idle. The local zone is pinned to UTC and today to 2026-10-04: the page and the calendar read
# datetime and date through their modules' globals, so those are replaced by subclasses.
#
# FioriVoce differs where these builders put its own in:
# - the product name in copy is FioriVoce's;
# - the restore tile has a hidden "Import from OpenWhisper..." button after "Restore and
#   restart" (offered only when OpenWhisper data is found, which the reference has none of).

BACKUP_WIDE = (900, 1100)
BACKUP_NARROW_WIDTH = 360
BACKUP_FOLDER = "C:\\Backups\\FioriVoce"
BACKUP_TODAY = (2026, 10, 4)
BACKUP_LAST = {
    "path": BACKUP_FOLDER + "\\FioriVoce-backup.fvbackup",
    "created_at": "2026-10-04T12:30:00Z",
    "app_version": "0.1.0",
    "schema_version": 1,
    "include_recordings": True,
    "file_count": 14,
    "total_bytes": 1468006400,
    "archive_bytes": 734003200,
    "warnings": [],
}

_backup_pinned = False


def _pin_backup():
    """Pins the zone to UTC and today to BACKUP_TODAY, once per process."""
    global _backup_pinned
    if _backup_pinned:
        return
    _backup_pinned = True
    import datetime as datetime_module

    from ui_qt.dialogs import settings_backup
    from ui_qt.widgets import backup_calendar, history_calendar

    class PinnedDate(datetime_module.date):
        @classmethod
        def today(cls):
            return cls(*BACKUP_TODAY)

    class UtcDatetime(datetime_module.datetime):
        def astimezone(self, tz=None):
            return super().astimezone(tz or datetime_module.timezone.utc)

    settings_backup.date = PinnedDate
    settings_backup.datetime = UtcDatetime
    backup_calendar.date = PinnedDate
    history_calendar.date = PinnedDate


def _backup_coordinator():
    from datetime import datetime, timezone

    from PyQt6.QtCore import QObject, pyqtSignal

    next_at = datetime(2026, 10, 5, 12, 30, tzinfo=timezone.utc).timestamp()

    class FakeCoordinator(QObject):
        busy_changed = pyqtSignal(bool)
        progress = pyqtSignal(str, int, int)
        backup_finished = pyqtSignal(object, str)
        inspection_finished = pyqtSignal(object, str)
        restore_staged = pyqtSignal(str, str)
        schedule_changed = pyqtSignal()

        def snapshot(self):
            return {"last_backup": dict(BACKUP_LAST), "frequency": "daily", "retention_count": 5,
                    "destination": BACKUP_FOLDER, "busy": False, "operation": "",
                    "last_error": "", "previous_data_dir": "", "next_backup_at": next_at}

    return _keep(FakeCoordinator())


def _rebrand(page):
    from PyQt6.QtWidgets import QLabel

    from ui_qt.widgets.wrapped_label import WrappedLabel

    for label in page.findChildren(QLabel):
        if "OpenWhisper" in label.text():
            text = label.text().replace("OpenWhisper", PRODUCT)
            if isinstance(label, WrappedLabel):
                label.setText(text)
            else:
                QLabel.setText(label, text)


def _backup_page(width, height=None):
    from PyQt6.QtCore import Qt

    from ui_qt.dialogs.settings_destinations import BACKUP
    from ui_qt.widgets import Button
    from ui_qt.widgets.buttons import neutral_button

    _pin_backup()
    dialog = _app_dialog({}, backup_coordinator=_backup_coordinator())
    dialog.ensure_page(BACKUP)
    dialog._page_scrolls[BACKUP].takeWidget()
    page = dialog._pages[BACKUP]
    page.setParent(None)
    _keep(page)
    _rebrand(page)
    backup = dialog.backup_page
    importing = neutral_button(Button("Import from OpenWhisper\u2026"))
    importing.setObjectName("backupImportButton")
    backup.restore_actions.insertWidget(backup.restore_actions.count() - 1, importing)
    backup.restore_actions.setAlignment(importing, Qt.AlignmentFlag.AlignLeft)
    importing.hide()
    return page, (width, height or _settled_height(page, width)), None


def backup_settings_page_default(variant):
    return _backup_page(*BACKUP_WIDE)


def backup_settings_page_narrow(variant):
    return _backup_page(BACKUP_NARROW_WIDTH)


# --- The AI cleanup page (M4.12) ---
#
# The page body as _app_page builds it: AI cleanup on, the cleanup model OpenRouter's
# openrouter/free with its key found (the picker's credential_status is replaced by the answer a
# host that found OpenRouter's key gives, as builders/a13.py does for the picker alone; FioriVoce
# reads the key off the GUI thread and the test waits for it), thinking Off. FioriVoce's page has
# no installed-agent runners until the agents arrive (M8), so the agent picker above the Cleanup
# model card and its model card inside it are taken out of the tree; nothing else is. No catalog
# is loaded (the page is never shown here), so the picker says it is loading. OpenWhisper elides
# the endpoint address with the label's font before the theme has styled the page, FioriVoce
# once it is styled (its page is styled as it is built), which at 130 % elides
# "https://openrouter.ai/api/v1": the endpoint is shown again once the page is styled, as any
# later choice of endpoint shows it in OpenWhisper too.
#
#     settings_cleanup/medium   Medium, no custom prompt: the box previews the preset
#     settings_cleanup/custom   a saved custom prompt, so the bar shows Custom
#
# builders/a15.py takes the Cleanup model card out of the same page, and builders/a13.py builds
# the Learned rules page the same way.

CLEANUP_VALUES = {
    "selected_model": "parakeet",
    "transcript_cleanup_enabled": True,
    "transcript_cleanup_level": "medium",
    "transcript_cleanup_provider": "openrouter",
    "transcript_cleanup_model": "openrouter/free",
    "transcript_cleanup_reasoning": "off",
}
CUSTOM_PROMPT = "Rewrite it as a friendly Slack message."


def _openrouter_key_found(profile):
    """What credential_status answers with OpenRouter's key found and no other key set."""
    if not profile.api_key_env:
        return True, "No API key required"
    if profile.id == "openrouter":
        return True, "OpenRouter API key found"
    return False, f"Requires {profile.api_key_env} - add it in Settings"


def _without_agents(dialog):
    """Takes the installed-agent picker and its model card out (see above)."""
    from PyQt6 import sip

    picker = dialog.models.cleanup_agent_picker
    card = picker.model_card
    sip.delete(picker)
    if not sip.isdeleted(card):
        sip.delete(card)


def cleanup_dialog(values):
    """A Settings dialog over `values` with the AI cleanup page built as FioriVoce builds it."""
    from ui_qt.dialogs.settings_destinations import CLEANUP
    from ui_qt.widgets import text_model_picker

    text_model_picker.credential_status = _openrouter_key_found
    dialog = _app_dialog(values)
    dialog.ensure_page(CLEANUP)
    _without_agents(dialog)
    picker = dialog.models.text_model_picker
    picker.ensurePolished()
    picker.set_provider(picker.provider)
    return dialog


def _cleanup_page(values):
    from ui_qt.dialogs.settings_destinations import CLEANUP

    dialog = cleanup_dialog(values)
    dialog._page_scrolls[CLEANUP].takeWidget()
    page = dialog._pages[CLEANUP]
    page.setParent(None)
    _keep(page)
    _fiorivoce(page)
    return page, (PAGE_WIDTH, _settled_height(page, PAGE_WIDTH)), None


def settings_cleanup_medium(variant):
    return _cleanup_page(CLEANUP_VALUES)


def settings_cleanup_custom(variant):
    return _cleanup_page({**CLEANUP_VALUES, "transcript_cleanup_prompt": CUSTOM_PROMPT})


BUILDERS = {
    "settings_walkthroughs_page/defaults": settings_walkthroughs_page_defaults,
    "settings_walkthroughs_page/write-up-codex": settings_walkthroughs_page_write_up_codex,
    "settings_cleanup/medium": settings_cleanup_medium,
    "settings_cleanup/custom": settings_cleanup_custom,
    "backup_settings_page/default": backup_settings_page_default,
    "backup_settings_page/narrow": backup_settings_page_narrow,
    "settings_search_palette/microphone": settings_search_palette_microphone,
    "settings_home/wide": settings_home_wide,
    "settings_home/narrow": settings_home_narrow,
    "settings_dialog/shell": shell,
    "settings_dialog/narrow": narrow,
    "settings_recording/backups": settings_recording_backups,
    "settings_general/default": settings_general_default,
    "settings_developer/default": settings_developer_default,
    "settings_api_keys/saved": settings_api_keys_saved,
    "settings_api_keys/text-owners": settings_api_keys_text_owners,
    "settings_appearance/bars": settings_appearance_bars,
}
