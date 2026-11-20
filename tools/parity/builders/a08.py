"""Reference builders of the Quick Record tab, the compact controller and the stats strip
(fv_ui_record, M1.9), of the Upload File tab's widgets, the reading window and the batch
description dialog (M2.14, further down), of the Upload File tab itself (M2.21) and of the
Scratchpad window (M2.25).

Each builder returns (widget, (width, height), focus widget or None) with fixed fake data: the
settings of a fresh Parakeet install (Parakeet selected, model parakeet-v3, device auto, AI
cleanup and Live preview off, no Command Mode shortcut) in the isolated data root. Nothing here
runs an event loop: render_reference.py processes events once.

OpenWhisper builds widgets for features the port leaves out of this milestone (the API and
Remote engine fields, the Whisper Quant field, the remote line of the stats strip), and the
cleanup profile card, which only the `quick_record_tab/profile` variant shows (M4.11). They are
hidden in every other state these builders render, and are taken out of the tree before it is
dumped, so the reference tree is the port's: nothing visible differs. The Upload
File tab's api variant keeps the API fields, which M2.21 builds.
"""

from unittest import mock

from PyQt6 import sip

DEVICE_INFO = "Parakeet TDT 0.6B v3 | CPU"
TRANSCRIPT = "Hello from FioriVoce."
RAW_TRANSCRIPT = "hello from fiorivoce"


def _settings():
    from services.settings import SettingsKey, settings_manager

    settings_manager.save_all_settings({
        SettingsKey.SELECTED_MODEL: "parakeet",
        SettingsKey.LOCAL_ASR_MODELS: {"parakeet": "parakeet-v3"},
        SettingsKey.LOCAL_ASR_DEVICES: {"parakeet": "auto"},
        SettingsKey.TRANSCRIPT_CLEANUP_ENABLED: False,
        SettingsKey.STREAMING_ENABLED: False,
        SettingsKey.HOTKEYS: {"command_mode": ""},
    })


def _detach(widget):
    """Deletes a hidden widget the port does not have, so it is not in the dumped tree."""
    if widget is not None:
        sip.delete(widget)


def _quick_record_tab(variant):
    from ui_qt.widgets.quick_record_tab import QuickRecordTab

    _settings()
    # The provider's readiness is M4's; the port reports it as not ready until then.
    with mock.patch("services.text_rewrite.provider_readiness", return_value=False):
        tab = QuickRecordTab()
    tab.sync_engine_selection()
    tab.set_device_info(DEVICE_INFO, True)
    tab.update_hotkeys("Ctrl+Alt+R", "Esc")
    return tab


def _prune(tab, keep_profile_card=False):
    for name in ("profile_card", "api_server", "remote_model_field", "remote_engine",
                 "remote_runtime_label", "remote_manage_button", "link_glyph", "serving_row"):
        if name == "profile_card" and keep_profile_card:
            continue
        _detach(getattr(tab, name))
    _detach(tab.local_engine.compute_combo.parentWidget())
    _detach(tab.stats_widget.transcription_time_widget.detail_label)


def quick_record_idle(variant):
    tab = _quick_record_tab(variant)
    _prune(tab)
    return tab, (680, 560), None


def quick_record_transcript(variant):
    tab = _quick_record_tab(variant)
    tab.set_transcription_collapsed(False)
    tab.set_transcript(TRANSCRIPT, RAW_TRANSCRIPT)
    tab.set_transcription_stats(2.4, 12.7, 403200)
    _prune(tab)
    return tab, (680, 760), None


def quick_record_recording(variant):
    tab = _quick_record_tab(variant)
    tab.is_recording = True
    tab._update_recording_state()
    _prune(tab)
    return tab, (680, 560), None


def quick_record_profile(variant):
    """AI cleanup on, two profiles, Email chosen with its shortcut, a Command Mode shortcut and a
    provider that is ready: the cleanup profile card (M4.11) is shown."""
    from services.settings import SettingsKey, settings_manager
    from ui_qt.widgets.quick_record_tab import QuickRecordTab

    _settings()
    settings_manager.update_settings({
        SettingsKey.TRANSCRIPT_CLEANUP_ENABLED: True,
        SettingsKey.TRANSCRIPT_CLEANUP_PROFILES: [
            {"id": "support-ticket", "name": "Support ticket",
             "instructions": "Format the dictation as a support ticket.", "hotkey": "",
             "use_learned_rules": True},
            {"id": "email", "name": "Email",
             "instructions": "Turn the dictation into a concise, professional email.",
             "hotkey": "ctrl+alt+e", "use_learned_rules": True},
        ],
        SettingsKey.QUICK_RECORD_PROFILE: "email",
        SettingsKey.HOTKEYS: {"command_mode": "ctrl+alt+k"},
        # A local server needs no key, so the provider is ready from the settings alone; the
        # tab reads that again when it is shown.
        SettingsKey.TRANSCRIPT_CLEANUP_PROVIDER: "ollama",
        SettingsKey.TRANSCRIPT_CLEANUP_MODEL: "llama3.2",
    })
    tab = QuickRecordTab()
    tab.sync_engine_selection()
    tab.set_device_info(DEVICE_INFO, True)
    tab.update_hotkeys("Ctrl+Alt+R", "Esc")
    _prune(tab, keep_profile_card=True)
    return tab, (680, 640), None


def compact_record_controller_idle(variant):
    from ui_qt.widgets.compact_record_controller import CompactRecordController

    controller = CompactRecordController()
    controller.update_hotkeys("Ctrl+Alt+R", "Esc")
    return controller, (420, 200), None


def compact_record_controller_recording(variant):
    from ui_qt.widgets.compact_record_controller import CompactRecordController

    controller = CompactRecordController()
    controller.update_hotkeys("Ctrl+Alt+R", "Esc")
    controller.set_recording_state(True)
    controller.set_status("Recording in progress...")
    return controller, (420, 200), None


def transcription_stats_filled(variant):
    from ui_qt.widgets.stats_display import TranscriptionStatsWidget

    stats = TranscriptionStatsWidget()
    stats.set_stats(2.4, 12.7, 403200)
    _detach(stats.transcription_time_widget.detail_label)
    return stats, (560, 72), None


# The Upload File tab's widgets (fv_ui_record, M2.14). Each is built alone, as the tab builds it,
# over fixed fake files: nothing here runs an event loop, and every animation (the result note's
# reveal, the progress bar's easing, the elapsed clock) is driven to its end value.
FILE_NAME = "interview.mp3"
QUEUE_FILES = (("interview-a.mp3", 12.4, 845.0), ("interview-b.mp3", 8.2, 563.0),
               ("interview-c.mp3", 3.1, 211.0))
BATCH_FILES = tuple(f"interview-{n}.mp3" for n in range(1, 8))
BATCH_OVERVIEW = ("## interview-a.mp3\n\nGood morning, thanks for joining.\n\n"
                  "## interview-b.mp3\n\nLet's pick up where we left off.")


def _preview(name=FILE_NAME, size_mb=12.4, seconds=845.0):
    from services.audio_processor import AudioFilePreview

    return AudioFilePreview(
        file_path=name, file_name=name, file_size_mb=size_mb, duration_seconds=seconds,
        sample_rate=44100, channels=2, needs_splitting=False, estimated_chunks=1)


def drop_zone_idle(variant):
    from ui_qt.widgets.upload_file_tab import DropZoneWidget

    return DropZoneWidget(), (420, 190), None


def drop_zone_reject(variant):
    """The zone as a drag of non-audio files leaves it: the reject style, by the real handler."""
    from PyQt6.QtCore import QCoreApplication, QMimeData, QPoint, Qt, QUrl
    from PyQt6.QtGui import QDragEnterEvent

    from ui_qt.widgets.upload_file_tab import DropZoneWidget

    zone = DropZoneWidget()
    # A drag arrives at a shown zone; the style filter resolves a sheet's tokens on the change
    # only once the widget has been through it, so a sheet set before the first show stays raw.
    zone.setFixedSize(420, 190)
    zone.show()
    QCoreApplication.processEvents()
    mime = QMimeData()
    mime.setUrls([QUrl.fromLocalFile("/path/to/notes.txt")])
    event = QDragEnterEvent(QPoint(10, 10), Qt.DropAction.CopyAction, mime,
                            Qt.MouseButton.NoButton, Qt.KeyboardModifier.NoModifier)
    zone.dragEnterEvent(event)
    zone._mime = mime
    QCoreApplication.processEvents()
    return zone, (420, 190), None


def _card():
    from ui_qt.widgets.upload_file_tab import FileInfoCard

    card = FileInfoCard()
    card.set_preview(_preview())
    return card


def upload_file_card_single(variant):
    return _card(), (560, 230), None


def upload_file_card_result(variant):
    card = _card()
    card.set_result(62.0, 845.0)
    # The reveal is a timer; the reference is its finished text.
    card.result_label.set_segments(card.result_label._segments, animate=False)
    card.set_copy_enabled(True)
    return card, (560, 230), None


def upload_queue_three(variant):
    from ui_qt.widgets.upload_file_tab import QueueItem, UploadQueueWidget

    queue = UploadQueueWidget()
    states = ("done", "active", "pending")
    items = []
    for (name, size_mb, seconds), state in zip(QUEUE_FILES, states):
        item = QueueItem(name, preview=_preview(name, size_mb, seconds), state=state)
        if state == "done":
            item.transcript = "Good morning, thanks for joining."
        items.append(item)
    queue.render(items)
    return queue, (560, 340), None


def upload_progress_panel_batch(variant):
    import time

    from ui_qt.widgets.transcription_progress import (
        ProgressStage,
        TranscriptionProgressPanel,
    )

    panel = TranscriptionProgressPanel()
    panel.start(True, 3)
    panel.set_batch_position(2, 3, "interview-b.mp3")
    panel.set_stage(ProgressStage.TRANSCRIBING)
    panel._started_at = time.monotonic() - 42
    panel._tick()
    # The bar eases toward its fraction on a timer; the reference is where it settles.
    panel.bar.set_fraction(panel.bar.fraction, animate=False)
    # Its highlight travels on a 16 ms timer that starts when the bar is shown; whether it ticks
    # before the grab depends on the machine, so the timer is given an interval it never reaches.
    panel.bar._timer.setInterval(3_600_000)
    return panel, (520, 72), None


def batch_relation_dialog_default(variant):
    from ui_qt.dialogs.batch_relation_dialog import BatchRelationDialog

    dialog = BatchRelationDialog(list(BATCH_FILES), "", False)
    return dialog, (620, 520), dialog.instructions_edit


def transcript_viewer_dialog_batch(variant):
    from services.batch_upload import (
        BatchItem,
        BatchItemResult,
        BatchRelation,
        BatchResult,
        BatchUploadRequest,
    )
    from ui_qt.dialogs.transcript_viewer_dialog import TranscriptViewerDialog

    request = BatchUploadRequest(
        items=(BatchItem("interview-a.mp3"), BatchItem("interview-b.mp3")),
        relation=BatchRelation.SEPARATE)
    result = BatchResult(request=request, items=(
        BatchItemResult(request.items[0], text="Good morning, thanks for joining."),
        BatchItemResult(request.items[1], text="Let's pick up where we left off."),
    ))
    dialog = TranscriptViewerDialog()
    dialog.set_batch_result(result, BATCH_OVERVIEW, title="2 files")
    return dialog, (900, 520), None


# The Upload File tab itself (M2.21), in the main window's page width: the drop zone, a queue,
# a finished file and the API backend. Previews are given, never read: the worker runs inline.
UPLOAD_TRANSCRIPT = ("## Interview\n\nGood morning, thanks for **joining**.\n\n"
                     "- First point\n- Second point")


def _upload_tab(extra=None):
    from ui_qt.widgets.upload_file_tab import UploadFileTab

    _settings()
    if extra:
        from services.settings import settings_manager

        settings_manager.update_settings(extra)
    tab = UploadFileTab()
    tab.sync_engine_selection()
    tab.set_device_info(DEVICE_INFO, True)
    return tab


def _queue_files(tab, files):
    from PyQt6.QtCore import QCoreApplication

    from ui_qt.widgets import upload_file_tab as module

    previews = {name: _preview(name, size_mb, seconds) for name, size_mb, seconds in files}
    with mock.patch.object(module, "_run_in_thread", lambda target, name: target()), \
            mock.patch.object(module.audio_processor, "preview_file",
                              lambda path, engine_splits=False: previews[path]):
        tab._on_files_selected([name for name, _, _ in files], 0)
    QCoreApplication.processEvents()


def _prune_upload(tab, keep_api=False):
    names = ["remote_model_field", "remote_engine", "remote_runtime_label",
             "remote_manage_button", "link_glyph", "serving_row"]
    if not keep_api:
        names.append("api_server")
    for name in names:
        _detach(getattr(tab, name))
    _detach(tab.local_engine.compute_combo.parentWidget())
    _detach(tab.stats_widget.transcription_time_widget.detail_label)


def upload_file_tab_empty(variant):
    tab = _upload_tab()
    _prune_upload(tab)
    return tab, (680, 560), None


def upload_file_tab_queue(variant):
    tab = _upload_tab()
    _queue_files(tab, QUEUE_FILES)
    _prune_upload(tab)
    return tab, (680, 720), None


def upload_file_tab_result(variant):
    tab = _upload_tab()
    _queue_files(tab, ((FILE_NAME, 12.4, 845.0),))
    tab.set_transcription_collapsed(False)
    tab.set_transcript(UPLOAD_TRANSCRIPT)
    tab.set_transcription_stats(62.0, 845.0, 13002342)
    label = tab.file_info_card.result_label
    # The reveal is a timer; the reference is its finished text.
    label.set_segments(label._segments, animate=False)
    _prune_upload(tab)
    return tab, (680, 760), None


def upload_file_tab_api(variant):
    from services.settings import SettingsKey
    from ui_qt.widgets.speech_server_controls import SpeechServerControls

    # Groq's catalog would be fetched from the internet. No catalog arrives, so the Model field
    # lists the saved model alone, as it does before any answer. Left patched for the render,
    # which may refresh the fields again.
    mock.patch.object(SpeechServerControls, "_fetch_models", lambda self: None).start()
    tab = _upload_tab({SettingsKey.SELECTED_MODEL: "api",
                       SettingsKey.API_TRANSCRIPTION_ENDPOINT: "groq"})
    tab.set_device_info("", None)
    _prune_upload(tab, keep_api=True)
    return tab, (680, 560), None


# The Scratchpad (M2.25): the frameless classic window at 400x380 with on-top on, never shown
# through present() or ensure_loaded(), so the window keeps the offscreen platform's own place and
# the editor shows no caret.
SCRATCHPAD_TEXT = "Groceries\n- milk\n- bread"


def _scratchpad():
    from services.settings import SettingsKey, settings_manager
    from ui_qt.widgets.scratchpad import ScratchpadWindow

    _settings()
    settings_manager.update_settings({SettingsKey.SCRATCHPAD_ALWAYS_ON_TOP: True})
    pad = ScratchpadWindow()
    # The port names the marker of the application's own windows after the product.
    pad.setProperty("openwhisperHotkeyWindow", None)
    pad.setProperty("fiorivoceHotkeyWindow", True)
    pad.editor.setPlainText(SCRATCHPAD_TEXT)
    # The autosave and geometry timers would write into the isolated data root while the
    # render waits; the reference is the window as it stands.
    pad._autosave_timer.stop()
    return pad


def scratchpad_notes(variant):
    return _scratchpad(), (400, 380), None


def scratchpad_undo(variant):
    pad = _scratchpad()
    pad.clear_text()
    pad._autosave_timer.stop()
    return pad, (400, 380), None


# render_reference.py writes one folder per builder name, so a name with a slash is a folder with
# a sub-folder per variant: tests/reference/quick_record_tab/idle/dark-windows11-dpr1-s100.png.
BUILDERS = {
    "drop_zone/idle": drop_zone_idle,
    "drop_zone/reject": drop_zone_reject,
    "upload_file_card/single": upload_file_card_single,
    "upload_file_card/result": upload_file_card_result,
    "upload_queue/three": upload_queue_three,
    "upload_progress_panel/batch": upload_progress_panel_batch,
    "batch_relation_dialog/default": batch_relation_dialog_default,
    "transcript_viewer_dialog/batch": transcript_viewer_dialog_batch,
    "quick_record_tab/idle": quick_record_idle,
    "quick_record_tab/transcript": quick_record_transcript,
    "quick_record_tab/recording": quick_record_recording,
    "quick_record_tab/profile": quick_record_profile,
    "compact_record_controller/idle": compact_record_controller_idle,
    "compact_record_controller/recording": compact_record_controller_recording,
    "transcription_stats/filled": transcription_stats_filled,
    "upload_file_tab/empty": upload_file_tab_empty,
    "upload_file_tab/queue": upload_file_tab_queue,
    "upload_file_tab/result": upload_file_tab_result,
    "upload_file_tab/api": upload_file_tab_api,
    "scratchpad/notes": scratchpad_notes,
    "scratchpad/undo": scratchpad_undo,
}
