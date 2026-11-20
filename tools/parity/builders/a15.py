"""Reference builders of the Downloads rows and dialogs (fv_ui_downloads, M2.13) and, at the end,
of the Voice model page inside the Settings dialog (M2.20).

Each builder returns (widget, (width, height), focus widget or None) with fixed fake data. The
rows are 680 px wide at the height their layout asks for; the dialogs are shown once and hidden
again to learn the size they open at, which is the size recorded.

OpenWhisper_CPP differs from OpenWhisper in the text these builders put OpenWhisper_CPP's in (the C++ side
reads the same copy from fv_models):
- MODEL_SECURITY_NOTICE names OpenWhisper_CPP where OpenWhisper names itself;
- the speech runtimes' profile (ComponentDetails) is OpenWhisper_CPP's: the runtime is a set of native
  libraries loaded into the app's own process, so the rows say so instead of "Runs in a
  dedicated process" and carry no portable Python;
- the consent dialog names the one runtime it still needs (the isolated data folder has none
  installed) and the notice fits a screen 900 px high, as the C++ test pins it;
- the speech runtimes' archives and install sizes are OpenWhisper_CPP's (src/models/data/
  speech_catalog.json, the win_amd64 entries), which carry no portable Python;
- the batch dialog's destination is a fixed folder instead of the Hugging Face cache.

Nothing here runs an event loop but the dialogs' one processEvents to learn their size;
render_reference.py then processes events once more.
"""

import json
from dataclasses import replace
from pathlib import Path
from types import MappingProxyType, SimpleNamespace
from unittest.mock import patch

ROW_WIDTH = 680
SCREEN_HEIGHT = 900
DESTINATION = "C:/Users/test/AppData/Local/OpenWhisper_CPP/speech-models"
PARAKEET = "parakeet-v3"
CUDA = "asr-nvidia-cuda"
CPU = "asr-nvidia-cpu"
PARAKEET_BYTES = 713975456

_SECURITY_NOTICE = (
    "Models are provided by third-party publishers. Model files and speech runtimes "
    "can have security vulnerabilities. Keep OpenWhisper_CPP and its runtimes updated, "
    "and review important transcripts for errors."
)

# component_details.cpp's rows for the NVIDIA runtimes: (display name, licence, summary, requires).
_RUNTIMES = {
    "asr-nvidia-cpu": (
        "NVIDIA Speech CPU", "Apache-2.0", "CPU runtime shared by Parakeet models.",
        "Windows x64, Linux x86_64 (glibc 2.31+), or macOS (Apple Silicon or Intel)"),
    "asr-nvidia-cuda": (
        "NVIDIA Speech GPU", "Apache-2.0; NVIDIA CUDA",
        "NVIDIA GPU runtime shared by Parakeet models.",
        "Windows x64 or Linux x86_64 (glibc 2.31+), and an NVIDIA GPU (Turing or newer) with its "
        "driver"),
}
_UPSTREAM = "https://github.com/NVIDIA/NeMo-Speech.cpp"

_CATALOG_JSON = (
    Path(__file__).resolve().parents[3] / "src" / "models" / "data" / "speech_catalog.json"
)
_pinned = False


def _openwhisper_cpp_entry(component_id):
    """OpenWhisper_CPP's catalog entry of a runtime for Windows x64: its archives and install size."""
    catalog = json.loads(_CATALOG_JSON.read_text(encoding="utf-8"))
    return catalog["components"][component_id]["platforms"]["win_amd64"]


def _pin():
    """OpenWhisper_CPP's notice and runtime profiles, once per process."""
    global _pinned
    if _pinned:
        return
    _pinned = True

    from services import component_catalog, model_catalog

    model_catalog.MODEL_SECURITY_NOTICE = _SECURITY_NOTICE
    catalog = dict(component_catalog.COMPONENT_CATALOG)
    for component_id, (name, licence, summary, requires) in _RUNTIMES.items():
        catalog[component_id] = replace(
            catalog[component_id], display_name=name, summary=summary,
            description=summary + " Models download separately. Runs inside OpenWhisper_CPP.",
            origin_name=name, origin_url=_UPSTREAM, origin_label="Project \u2197",
            source_name="Pinned upstream archives", source_url=_UPSTREAM,
            source_label="Project \u2197",
            maintainer="Upstream publishers; packaged by OpenWhisper_CPP", family="Speech runtime",
            requires=requires, payload="Verified native libraries",
            local_format="Native libraries loaded by OpenWhisper_CPP", license=licence,
            best_for=summary, limitations=("Model weights are a separate download.",),
            compact_tags="Local speech", source_urls=(_UPSTREAM,))
    component_catalog.COMPONENT_CATALOG = MappingProxyType(catalog)


def _runtime_dialogs():
    """The runtime dialogs' module, reading OpenWhisper_CPP's sizes."""
    from ui_qt.dialogs import required_runtime_dialog

    required_runtime_dialog.catalog_entry_for_platform = _openwhisper_cpp_entry
    return required_runtime_dialog


def _fit(widget, width=ROW_WIDTH):
    """The row at `width`, as tall as its layout asks for once it is styled."""
    widget.ensurePolished()
    return width, widget.sizeHint().height()


def _natural_size(dialog):
    """The size a dialog opens at: shown once, then hidden again."""
    from PyQt6.QtCore import QCoreApplication

    dialog.show()
    QCoreApplication.processEvents()
    size = (dialog.width(), dialog.height())
    dialog.hide()
    return size


def _cached():
    from services.hf_access import CachedModelInfo

    return CachedModelInfo(repo_id="nvidia/parakeet-tdt-0.6b-v3", size_bytes=PARAKEET_BYTES,
                           path="", revision_hashes=())


def _model_row(info=None, **state):
    from ui_qt.widgets.model_row_widget import ModelRowWidget

    _pin()
    row = ModelRowWidget(PARAKEET)
    row.update_state(info, state.get("is_active", False), state.get("is_loaded", False),
                     state.get("downloading", False), state.get("downloads_blocked", False),
                     state.get("download_slot_busy", False), state.get("queued", False),
                     state.get("selection_enabled", True))
    return row


def model_row_not_downloaded(variant):
    row = _model_row()
    return row, _fit(row), None


def model_row_downloaded(variant):
    row = _model_row(_cached())
    return row, _fit(row), None


def model_row_downloading(variant):
    from PyQt6.QtCore import QCoreApplication

    # The row is on screen when its download starts, as in the page: the bar is laid over the
    # row's bottom edge only while the row is visible. The busy bar is at t = 0: it starts
    # moving when it is first painted, which is the grab.
    row = _model_row()
    width, height = _fit(row)
    row.setFixedSize(width, height)
    row.show()
    QCoreApplication.processEvents()
    row.update_state(None, False, False, True, False, True, False, False)
    return row, (width, height), None


def _component_info(state, **fields):
    from services.components import ComponentInfo

    _pin()
    entry = _openwhisper_cpp_entry(CUDA)
    download = sum(archive.get("size_bytes", 0) for archive in entry.get("archives", []))
    return ComponentInfo(
        component_id=CUDA, display_name=_RUNTIMES[CUDA][0], summary=_RUNTIMES[CUDA][2],
        state=state, installed_version=None, available_version=None,
        download_bytes=download, install_bytes=int(entry.get("install_bytes", 0)), **fields)


def component_row_not_installed(variant):
    from ui_qt.widgets.component_row_widget import ComponentRowWidget
    from services.components import ComponentState

    row = ComponentRowWidget(CUDA)
    row.update_state(_component_info(ComponentState.NOT_INSTALLED), False)
    return row, _fit(row), None


def component_row_installing(variant):
    from ui_qt.widgets.component_row_widget import ComponentRowWidget
    from services.components import ComponentState, InstallPhase

    row = ComponentRowWidget(CUDA)
    row.update_state(_component_info(ComponentState.NOT_INSTALLED), True)
    row.set_progress(InstallPhase.DOWNLOADING, 40_000_000, 100_000_000)
    return row, _fit(row), None


def hf_consent_dialog_ask(variant):
    from services.settings import HuggingFaceAccessPolicy
    from ui_qt.dialogs.hf_consent_dialog import HuggingFaceConsentDialog

    _pin()
    with patch("services.local_asr.catalog.missing_runtime", return_value=CPU), \
            patch("services.local_asr.catalog.gpu_runtime_offer", return_value=None):
        dialog = HuggingFaceConsentDialog(PARAKEET, HuggingFaceAccessPolicy.ASK)
    dialog._available_height = lambda: SCREEN_HEIGHT
    return dialog, _natural_size(dialog), None


def required_runtime_dialog_cpu(variant):
    _pin()
    dialog = _runtime_dialogs().RequiredRuntimeDialog(PARAKEET, CPU)
    return dialog, _natural_size(dialog), None


def gpu_runtime_dialog_rtx_2060(variant):
    from services.gpu_info import NvidiaGpu

    _pin()
    gpu = NvidiaGpu("NVIDIA GeForce RTX 2060", 6144, (7, 5))
    with patch("services.gpu_info.nvidia_gpu", return_value=gpu):
        dialog = _runtime_dialogs().GpuRuntimeDialog(PARAKEET, CUDA, CPU)
    return dialog, _natural_size(dialog), None


def batch_download_dialog_two(variant):
    from ui_qt.dialogs import settings_downloads

    _pin()
    with patch.object(settings_downloads, "get_hf_cache_dir", return_value=DESTINATION):
        dialog = settings_downloads.BatchDownloadDialog([PARAKEET, "orukeet-v0.1"])
    return dialog, _natural_size(dialog), None


def component_details_dialog_cuda(variant):
    from services.components import ComponentState
    from ui_qt.dialogs import component_details_dialog

    _pin()
    info = _component_info(ComponentState.NOT_INSTALLED)
    stub = SimpleNamespace(describe=lambda component_id: info)
    with patch.object(component_details_dialog, "component_coordinator", stub):
        dialog = component_details_dialog.ComponentDetailsDialog(CUDA)
    return dialog, _natural_size(dialog), None


BUILDERS = {
    "model_row/not-downloaded": model_row_not_downloaded,
    "model_row/downloaded": model_row_downloaded,
    "model_row/downloading": model_row_downloading,
    "component_row/not-installed": component_row_not_installed,
    "component_row/installing": component_row_installing,
    "hf_consent_dialog/ask": hf_consent_dialog_ask,
    "required_runtime_dialog/cpu": required_runtime_dialog_cpu,
    "gpu_runtime_dialog/rtx-2060": gpu_runtime_dialog_rtx_2060,
    "batch_download_dialog/two": batch_download_dialog_two,
    "component_details_dialog/cuda": component_details_dialog_cuda,
}


# --- The Voice model page (M2.20) ---
#
# The page is OpenWhisper's own, built by its SettingsDialog (build_voice_page) among stand-ins
# for every other destination, the way a11.py builds the shell: OpenWhisper_CPP's test registers the
# same stand-ins around the real page. Differences from OpenWhisper put OpenWhisper_CPP's in:
# - the product name in the header, and the app mark OpenWhisper_CPP draws (as a11.py);
# - the footnote is OpenWhisper_CPP's, which names Parakeet's previews only;
# - OpenWhisper's Local Whisper picker field and the Quant field of the local engine row (Local
#   Whisper's, hidden for every engine rendered here) are taken out of the tree, and nothing else is;
# - the rail has no values but Voice model's, because the stand-ins register none.
# The data is fixed: Parakeet TDT 0.6B v3 downloaded, the CPU runtime installed, two dictation
# languages (English active, Russian) and Auto on the device; or the API engine on Groq.

VOICE_SIZE = (1315, 850)
VOICE_FOOTNOTE = (
    "For dictation previews, Parakeet transcribes short audio chunks with the loaded model. The "
    "final transcript uses your selected model."
)
_VOICE_KEEP = []
_VOICE_PATCHES = []


def _voice_values(**engine):
    return {
        "dictation_languages": ["en", "ru"],
        "dictation_active_language": "en",
        "local_asr_models": {"parakeet": PARAKEET},
        "local_asr_devices": {"parakeet": "auto"},
        **engine,
    }


def _voice_dialog(values):
    from contextlib import ExitStack

    from PyQt6 import sip
    from PyQt6.QtCore import QRect
    from PyQt6.QtGui import QPixmap
    from PyQt6.QtWidgets import QLabel

    from services.settings import settings_manager
    from ui_qt.dialogs import settings_dialog as dialog_module
    from ui_qt.dialogs import settings_downloads as downloads_module
    from ui_qt.dialogs import settings_models as models_module
    from ui_qt.dialogs.settings_destinations import VOICE_MODEL
    from ui_qt.utils.app_icon import render_app_pixmap
    from ui_qt.widgets.nav_rail import NavRail

    # An earlier dialog is still subscribed to the engine selection, and the save below would reach
    # its deleted Local Whisper picker.
    while _VOICE_KEEP:
        sip.delete(_VOICE_KEEP.pop())
    _pin()
    settings_manager.save_all_settings(dict(values))
    cls = dialog_module.SettingsDialog
    if not _VOICE_PATCHES:
        # Entered once and kept for the process: a second set would wrap the first's replacements.
        nothing = lambda self, *args, **kwargs: None  # noqa: E731
        original_set_value = NavRail.set_value
        original_add_page = cls._add_page
        original_footnote = models_module.ModelAssignments._footnote

        def add_page(self, key, title, subtitle, builder, *, scroll=True):
            original_add_page(self, key, title, subtitle,
                              builder if key == VOICE_MODEL else (lambda layout: None),
                              scroll=scroll)

        def set_value(self, key, text):
            if key == VOICE_MODEL:
                original_set_value(self, key, text)

        stack = ExitStack()
        for target, name, value in (
                (NavRail, "set_value", set_value),
                (cls, "_add_page", add_page),
                (cls, "_load_settings", nothing),
                (cls, "_render_rail_values", nothing),
                (cls, "_refresh_home", nothing),
                (models_module.ModelAssignments, "_footnote",
                 lambda self, text: original_footnote(self, VOICE_FOOTNOTE)),
        ):
            stack.enter_context(patch.object(target, name, value))
        for module in (models_module, downloads_module):
            stack.enter_context(patch.object(module, "scan_cached_models", return_value={}))
        stack.enter_context(patch.object(models_module, "peek_cached_models", return_value={}))
        # What is on this computer: the one model downloaded and the CPU runtime installed; Auto
        # runs on the CPU (OpenWhisper_CPP asks NVML, and the answer here is no GPU).
        stack.enter_context(patch("services.local_asr.cache.is_cached",
                                  side_effect=lambda key: key == PARAKEET))
        stack.enter_context(patch("services.components.is_installed",
                                  side_effect=lambda component: component == CPU))
        stack.enter_context(patch("services.local_asr.catalog._auto_device", return_value="cpu"))
        stack.enter_context(patch.object(dialog_module.history_manager, "set_retention"))
        _VOICE_PATCHES.append(stack)
    dialog = cls(get_loaded_model=lambda: None, background_cache_scan=False)
    # Instance attributes, read from Python by the methods that call them.
    dialog.refresh_models = lambda: None
    dialog._refresh_recordings_usage = lambda: None
    dialog._render_rail_values = lambda settings: None
    dialog._refresh_home = lambda: None
    dialog._load_settings = lambda: None
    screen = SimpleNamespace(availableGeometry=lambda: QRect(0, 0, 1920, 1080))
    dialog.screen = lambda: screen

    dialog.search_palette.setParent(None)
    dialog.settings_brand.setText("OpenWhisper_CPP")
    header = dialog.findChild(QLabel, "settingsViewBrand").parentWidget()
    icon = next(label for label in header.findChildren(QLabel) if not label.objectName())
    icon.setPixmap(QPixmap(render_app_pixmap(32)))

    pages = [dialog._page_scrolls.get(key, dialog._pages[key]) for key in dialog.rail.keys()]
    for page in pages:
        dialog.stack.removeWidget(page)
    for page in pages:
        dialog.stack.addWidget(page)

    dialog.select_destination(VOICE_MODEL)
    # OpenWhisper's Local Whisper picker: hidden for every engine rendered here, not OpenWhisper_CPP's.
    sip.delete(dialog.models.ondemand_whisper_field)
    # The Quant field of the local engine row is Local Whisper's; OpenWhisper_CPP's row has none.
    sip.delete(dialog.models.speech_controls.compute_combo.parentWidget())
    dialog.models._refresh_rail_values()
    # The language chips are rebuilt on every refresh and the old ones are deleted later; the
    # reference shows the chips that stay, so the deferred deletes are delivered now.
    from PyQt6.QtCore import QCoreApplication, QEvent

    QCoreApplication.sendPostedEvents(None, QEvent.Type.DeferredDelete.value)
    _VOICE_KEEP.append(dialog)
    return dialog, VOICE_SIZE, None


def settings_voice_model_parakeet(variant):
    return _voice_dialog(_voice_values(selected_model="parakeet"))


def settings_voice_model_groq(variant):
    return _voice_dialog(_voice_values(selected_model="api", api_transcription_endpoint="groq"))


BUILDERS["settings_voice_model/parakeet"] = settings_voice_model_parakeet
BUILDERS["settings_voice_model/groq"] = settings_voice_model_groq


# --- The Downloads page (M2.23) ---
#
# OpenWhisper's own DownloadsPage, built by its SettingsDialog among stand-ins for every other
# destination, as the Voice model builder does above: OpenWhisper_CPP's test registers the same
# stand-ins around the real page. The data is fixed: Parakeet TDT 0.6B v3 downloaded, Orukeet
# not, the CPU runtime installed and the CUDA runtime available, the "Ask before downloading"
# policy, Parakeet the dictation model. Differences from OpenWhisper put OpenWhisper_CPP's in:
# - the catalog is the Parakeet family only (MODELS, BACKENDS and the Whisper rows are cut to it,
#   and the backend filter's "Whisper" entry is taken out);
# - "Add custom models…" is taken out of the tree (custom faster-whisper models are not ported);
# - the header names the speech models folder where OpenWhisper names the Hugging Face cache;
# - meetings use no local model (Meeting Mode is not ported yet), so the usage chip says
#   "Dictation" alone;
# - the isolation's HF_HUB_OFFLINE=1 is not the computer's: the offline banner stays hidden;
# - the rail shows Downloads' value only, as OpenWhisper_CPP's stand-ins register none.
# The Voice model builder's patches are closed first: they stand in for the Downloads page.

DOWNLOADS_SIZE = (1315, 850)
NARROW_SIZE = (940, 850)
ORUKEET = "orukeet-v0.1"
_DOWNLOADS_KEEP = []
_DOWNLOADS_PATCHES = []


def _downloads_patches():
    from contextlib import ExitStack

    from config import config
    from services.components import ComponentState
    from services.local_asr import catalog
    from ui_qt.dialogs import settings_dialog as dialog_module
    from ui_qt.dialogs import settings_downloads as downloads_module
    from ui_qt.dialogs import settings_models as models_module
    from ui_qt.dialogs.settings_destinations import DOWNLOADS
    from ui_qt.widgets.nav_rail import NavRail

    cls = dialog_module.SettingsDialog
    nothing = lambda self, *args, **kwargs: None  # noqa: E731
    original_set_value = NavRail.set_value
    original_add_page = cls._add_page

    def add_page(self, key, title, subtitle, builder, *, scroll=True):
        original_add_page(self, key, title, subtitle,
                          builder if key == DOWNLOADS else (lambda layout: None), scroll=scroll)

    def set_value(self, key, text):
        if key == DOWNLOADS:
            original_set_value(self, key, text)

    infos = {
        CPU: _component_for(CPU, ComponentState.INSTALLED),
        CUDA: _component_for(CUDA, ComponentState.NOT_INSTALLED),
    }
    coordinator = SimpleNamespace(
        list_components=lambda: tuple(infos.values()),
        describe=lambda component_id: infos[component_id],
        is_installing=lambda component_id: False,
    )
    models = {key: catalog.MODELS[key] for key in (PARAKEET, ORUKEET)}

    stack = ExitStack()
    for target, name, value in (
            (NavRail, "set_value", set_value),
            (cls, "_add_page", add_page),
            (cls, "_load_settings", nothing),
            (cls, "_render_rail_values", nothing),
            (cls, "_refresh_home", nothing),
            (catalog, "MODELS", models),
            (catalog, "BACKENDS", {"parakeet": "Parakeet"}),
            (config, "WHISPER_MODEL_CHOICES", ["auto"]),
            (downloads_module, "component_coordinator", coordinator),
            (downloads_module, "get_hf_cache_dir", lambda: DESTINATION),
            (downloads_module, "is_hf_hub_offline_env_set", lambda: False),
            (downloads_module, "resolve_meeting_whisper_model", lambda settings=None: ""),
            (downloads_module, "scan_cached_models", lambda max_age_seconds=None: {}),
            (downloads_module, "peek_cached_models", lambda: {}),
            (models_module, "scan_cached_models", lambda max_age_seconds=None: {}),
            (models_module, "peek_cached_models", lambda: {}),
    ):
        stack.enter_context(patch.object(target, name, value))
    stack.enter_context(patch("services.local_asr.cache.is_cached",
                              side_effect=lambda key: key == PARAKEET))
    stack.enter_context(patch("services.components.is_installed",
                              side_effect=lambda component: component == CPU))
    stack.enter_context(patch("services.local_asr.catalog._auto_device", return_value="cpu"))
    stack.enter_context(patch.object(dialog_module.history_manager, "set_retention"))
    return stack


def _component_for(component_id, state):
    from services.components import ComponentInfo

    _pin()
    entry = _openwhisper_cpp_entry(component_id)
    download = sum(archive.get("size_bytes", 0) for archive in entry.get("archives", []))
    return ComponentInfo(
        component_id=component_id, display_name=_RUNTIMES[component_id][0],
        summary=_RUNTIMES[component_id][2], state=state, installed_version=None,
        available_version=None, download_bytes=download,
        install_bytes=int(entry.get("install_bytes", 0)))


def _downloads_dialog(size, tab=0, select=PARAKEET):
    from PyQt6 import sip
    from PyQt6.QtCore import QRect
    from PyQt6.QtGui import QPixmap
    from PyQt6.QtWidgets import QLabel

    from services.settings import settings_manager
    from ui_qt.dialogs import settings_dialog as dialog_module
    from ui_qt.dialogs.settings_destinations import DOWNLOADS
    from ui_qt.utils.app_icon import render_app_pixmap
    from ui_qt.widgets.nav_rail import NavRail

    while _VOICE_KEEP:
        sip.delete(_VOICE_KEEP.pop())
    while _VOICE_PATCHES:
        _VOICE_PATCHES.pop().close()
    while _DOWNLOADS_KEEP:
        sip.delete(_DOWNLOADS_KEEP.pop())
    _pin()
    settings_manager.save_all_settings({
        "selected_model": "parakeet",
        "local_asr_models": {"parakeet": PARAKEET},
        "hf_access_policy": "ask",
    })
    if not _DOWNLOADS_PATCHES:
        # Entered once and kept for the process: a second set would wrap the first's replacements.
        _DOWNLOADS_PATCHES.append(_downloads_patches())
    cls = dialog_module.SettingsDialog
    dialog = cls(get_loaded_model=lambda: None, background_cache_scan=False)
    # Instance attributes, read from Python by the methods that call them.
    dialog.refresh_models = lambda: None
    dialog._refresh_recordings_usage = lambda: None
    dialog._render_rail_values = lambda settings: None
    dialog._refresh_home = lambda: None
    dialog._load_settings = lambda: None
    screen = SimpleNamespace(availableGeometry=lambda: QRect(0, 0, 1920, 1080))
    dialog.screen = lambda: screen

    dialog.search_palette.setParent(None)
    dialog.settings_brand.setText("OpenWhisper_CPP")
    header = dialog.findChild(QLabel, "settingsViewBrand").parentWidget()
    icon = next(label for label in header.findChildren(QLabel) if not label.objectName())
    icon.setPixmap(QPixmap(render_app_pixmap(32)))

    pages = [dialog._page_scrolls.get(key, dialog._pages[key]) for key in dialog.rail.keys()]
    for page in pages:
        dialog.stack.removeWidget(page)
    for page in pages:
        dialog.stack.addWidget(page)

    dialog.select_destination(DOWNLOADS)
    downloads = dialog.downloads
    # Custom faster-whisper models are not ported.
    sip.delete(downloads.custom_models_button)
    combo = downloads.backend_filter_combo
    combo.removeItem(combo.findData("local_whisper"))
    downloads.refresh()
    downloads.tabs.setCurrentIndex(tab)
    if select:
        downloads.select_model(select)
        # The rescan Settings runs when it is shown (refresh_models, silenced above) fills in the
        # selected model's usage.
        downloads.refresh()
    NavRail.set_value(dialog.rail, DOWNLOADS, downloads.rail_value())
    _DOWNLOADS_KEEP.append(dialog)
    return dialog, size, None


def settings_downloads_models(variant):
    return _downloads_dialog(DOWNLOADS_SIZE)


def settings_downloads_components(variant):
    return _downloads_dialog(DOWNLOADS_SIZE, tab=1, select=None)


def settings_downloads_floating_inspector(variant):
    return _downloads_dialog(NARROW_SIZE)


BUILDERS["settings_downloads/models"] = settings_downloads_models
BUILDERS["settings_downloads/components"] = settings_downloads_components
BUILDERS["settings_downloads/floating-inspector"] = settings_downloads_floating_inspector


# --- The Cleanup model card of the AI cleanup page (M4.12) ---
#
# The card as builders/a11.py's settings_cleanup pages hold it (OpenRouter's openrouter/free with
# its key found, thinking Off, no agent picker), taken out of the page at SECTION_WIDTH and as tall
# as its layout asks for there.
#
#     cleanup_model_section/openrouter

SECTION_WIDTH = 680
_A11 = []


def _a11():
    """builders/a11.py, for its AI cleanup dialog."""
    import importlib.util

    if not _A11:
        path = Path(__file__).resolve().with_name("a11.py")
        spec = importlib.util.spec_from_file_location("builders_a11_for_a15", path)
        module = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(module)
        _A11.append(module)
    return _A11[0]


def cleanup_model_section_openrouter(variant):
    a11 = _a11()
    dialog = a11.cleanup_dialog(a11.CLEANUP_VALUES)
    tile = dialog.cleanup_model_tile
    tile.setParent(None)
    tile.move(0, 0)
    a11._keep(tile)
    return tile, (SECTION_WIDTH, a11._settled_height(tile, SECTION_WIDTH)), None


BUILDERS["cleanup_model_section/openrouter"] = cleanup_model_section_openrouter


# --- The Voice & speakers page (M7.18) ---
#
# The page body is OpenWhisper's own (build_meeting_voice_page), taken out of a real
# SettingsDialog as builders/a31.py takes the Fast judgments page out, with the speaker model's
# whereabouts pinned (found or not, no download blocked, no failure). Where OpenWhisper_CPP differs,
# these builders put its own in:
# - "Runs on" is hidden: remote meeting speech is not in this build, so where a meeting runs is
#   not a choice (OpenWhisper_CPP shows it only while a saved setting sends a separate engine's
#   meetings to the paired computer, which no variant here does);
# - the meeting model picker lists the catalog's meeting models (Orukeet TDT 0.6B, Parakeet TDT
#   0.6B v3) with no caption and no "Add custom models…" button, as OpenWhisper_CPP's picker does;
# - the OpenAI speaker item reads "OpenAI (retired)" and cannot be chosen.
#
#   settings_meeting_voice/following         meetings follow dictation's Parakeet, the speaker
#                                            model on this computer
#   settings_meeting_voice/separate          meetings with an engine of their own (Parakeet TDT
#                                            0.6B v3), the speaker model on this computer
#   settings_meeting_voice/speaker-download  following dictation, no speaker model yet: the
#                                            status says when it downloads, with Download now

_A31 = []
MEETING_VOICE_MODELS = (("Orukeet TDT 0.6B", ORUKEET), ("Parakeet TDT 0.6B v3", PARAKEET))
SPEAKER_MODEL = "C:/Users/test/AppData/Local/OpenWhisper_CPP/models/speaker-id/voxceleb_resnet34_LM.onnx"


def _a31():
    """builders/a31.py, for its page-taking helpers."""
    import importlib.util

    if not _A31:
        path = Path(__file__).resolve().with_name("a31.py")
        spec = importlib.util.spec_from_file_location("builders_a31_for_a15", path)
        module = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(module)
        _A31.append(module)
    return _A31[0]


def _meeting_voice(values, model_found):
    from PyQt6 import sip
    from PyQt6.QtCore import QSignalBlocker, Qt
    from PyQt6.QtWidgets import QComboBox, QPushButton, QWidget

    from ui_qt.dialogs import settings_models as models_module
    from ui_qt.dialogs.settings_destinations import MEETING_VOICE

    a31 = _a31()
    original_refresh = models_module.ModelAssignments._refresh_meeting_source

    def refresh_source(self):
        # Hidden before any layout reaches it, as OpenWhisper_CPP's field is never shown.
        original_refresh(self)
        self.meeting_source_field.setVisible(False)

    with patch.object(models_module.ModelAssignments, "_refresh_meeting_source", refresh_source), \
            patch.object(models_module, "speaker_model_path",
                      return_value=SPEAKER_MODEL if model_found else None), \
            patch.object(models_module, "speaker_model_download_blocked", return_value=""), \
            patch.object(models_module, "speaker_model_error", return_value=""):
        page, _size, focus = a31._app_page(MEETING_VOICE, values)
    picker = page.findChild(QWidget, "localModelPicker")
    custom = next(button for button in picker.findChildren(QPushButton)
                  if button.text().startswith("Add custom models"))
    sip.delete(custom)
    combo = picker.model_combo
    with QSignalBlocker(combo):
        selected = combo.currentData()
        combo.clear()
        for label, key in MEETING_VOICE_MODELS:
            combo.addItem(label, key)
            combo.setItemData(combo.count() - 1, key, Qt.ItemDataRole.ToolTipRole)
        combo.setCurrentIndex(max(0, combo.findData(selected)))
    picker.set_caption("")
    speakers = page.findChild(QComboBox, "meetingSpeakerIdCombo")
    openai = speakers.findData("openai")
    if openai < 0:
        speakers.addItem("OpenAI (retired)", "openai")
        openai = speakers.count() - 1
    speakers.setItemText(openai, "OpenAI (retired)")
    speakers.model().item(openai).setEnabled(False)
    return page, (a31.PAGE_WIDTH, a31._settled_height(page, a31.PAGE_WIDTH)), focus


def settings_meeting_voice_following(variant):
    return _meeting_voice({"selected_model": "parakeet", "meeting_speech_separate": False}, True)


def settings_meeting_voice_separate(variant):
    return _meeting_voice({"selected_model": "parakeet", "meeting_speech_separate": True,
                           "meeting_asr_model": PARAKEET}, True)


def settings_meeting_voice_speaker_download(variant):
    return _meeting_voice({"selected_model": "parakeet", "meeting_speech_separate": False}, False)


BUILDERS["settings_meeting_voice/following"] = settings_meeting_voice_following
BUILDERS["settings_meeting_voice/separate"] = settings_meeting_voice_separate
BUILDERS["settings_meeting_voice/speaker-download"] = settings_meeting_voice_speaker_download
