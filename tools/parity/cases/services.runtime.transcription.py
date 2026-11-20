"""Curated inputs for services/runtime/transcription.py (M1.19 dictation-core): the builders of
the status and notice strings the runtime decides. TranscriptionRuntime itself is M1.14's.

The methods run on a bare TranscriptionRuntime (TranscriptionRuntime.__new__) with stub
controller pieces, and report what they emitted. History calls are replaced for the call by a
stub that answers as the case says, and restored after. An action travels as
{"name", "kind", "params"}; an exception as [class name, message].
"""

import threading
import types

from services.actions.model import Action


def _action(fields):
    if fields is None:
        return None
    return Action(id="a1", name=fields["name"], kind=fields["kind"],
                  params=fields.get("params", {}))


class _Signal:
    def __init__(self):
        self.emitted = []

    def emit(self, *args):
        self.emitted.append(args[0] if len(args) == 1 else list(args))


def _bare(module, host_update=False):
    runtime = module.TranscriptionRuntime.__new__(module.TranscriptionRuntime)
    runtime.controller = types.SimpleNamespace(
        status_update=_Signal(), overlay_state_update=_Signal(), history_persisted=_Signal(),
        host_update_active=lambda: host_update,
        _pending_audio_duration=1.5, _pending_file_size=2048, _pending_source_name="",
        recorder=types.SimpleNamespace(),
    )
    runtime._job_lock = threading.Lock()
    runtime._walkthrough = None
    runtime._remote_retry_exhausted = False
    return runtime


def _start_failure_message(module, kind, message):
    classes = {"EngineNotReadyError": module.EngineNotReadyError, "RuntimeError": RuntimeError,
               "ValueError": ValueError}
    return module._start_failure_message(classes[kind](message))


def _recording_destination(module, action, mode):
    return module._recording_destination(_action(action), mode)


def _reads_selection(module, action):
    return module._reads_selection(_action(action))


def _report_busy(module, action, host_update):
    runtime = _bare(module, host_update)
    returned = runtime._report_busy(action)
    return {"returned": returned, "status": runtime.controller.status_update.emitted}


def _show_listening(module, mode, action_name, profile_name):
    runtime = _bare(module)
    action = None if action_name is None else types.SimpleNamespace(name=action_name)
    profile = None if profile_name is None else types.SimpleNamespace(name=profile_name)
    runtime._show_listening(mode, action, profile)
    return {"overlay": [state.value for state in runtime.controller.overlay_state_update.emitted],
            "status": runtime.controller.status_update.emitted}


def _error_status(module, message, mode, refused):
    from services.dictation_pipeline import DictationJob

    runtime = _bare(module)
    job = None if mode is None else DictationJob(mode=mode)
    return runtime._error_status(message, job, refused=refused)


def _save_failed_recording(module, status, outcome, remote_exhausted):
    runtime = _bare(module)
    runtime._remote_retry_exhausted = remote_exhausted
    runtime._model_info_for_history = lambda: "parakeet"

    def add_entry(**_fields):
        if outcome == "failed":
            raise RuntimeError("database is locked")
        return types.SimpleNamespace(audio_file="saved.wav" if outcome == "saved" else "")

    original = module.history_manager
    module.history_manager = types.SimpleNamespace(add_entry=add_entry)
    try:
        runtime._save_failed_recording("C:/elsewhere/failed.wav", status)
    finally:
        module.history_manager = original
    return runtime.controller.history_persisted.emitted


def _preserve_failed_audio(module, status, outcome, name):
    import os

    runtime = _bare(module)

    def preserve_recording(_path):
        if outcome == "failed":
            raise OSError("disk full")
        return name if outcome == "saved" else ""

    original, original_os = module.history_manager, module.os
    module.history_manager = types.SimpleNamespace(preserve_recording=preserve_recording)
    module.os = types.SimpleNamespace(path=types.SimpleNamespace(
        abspath=os.path.abspath, isfile=lambda _path: outcome == "not_retained"))
    try:
        runtime._preserve_failed_audio("C:/elsewhere/failed.wav", status)
    finally:
        module.history_manager, module.os = original, original_os
    return runtime.controller.history_persisted.emitted


def _delivery_notice(module, shaping, profile_name, failure):
    runtime = _bare(module)
    runtime._shaping_notice = shaping
    runtime._formatting_profile = (None if profile_name is None
                                   else types.SimpleNamespace(name=profile_name))
    runtime._last_cleanup_failure = failure
    return runtime._delivery_notice()


def _constants(module):
    return {"EMPTY_ASR_MESSAGE": module.EMPTY_ASR_MESSAGE,
            "MIN_RECORDING_BYTES": module.MIN_RECORDING_BYTES,
            "WALKTHROUGH_LABEL": module.WALKTHROUGH_LABEL}


WRAPPERS = {
    "_start_failure_message": _start_failure_message,
    "_recording_destination": _recording_destination,
    "_reads_selection": _reads_selection,
    "_report_busy": _report_busy,
    "_show_listening": _show_listening,
    "_error_status": _error_status,
    "_save_failed_recording": _save_failed_recording,
    "_preserve_failed_audio": _preserve_failed_audio,
    "_delivery_notice": _delivery_notice,
    "constants": _constants,
}

_AGENT = {"name": "Ship it", "kind": "agent", "params": {}}

CASES = {
    "_start_failure_message": [
        ["EngineNotReadyError",
         "The speech model isn't downloaded yet. Approve the download, then try again."],
        ["EngineNotReadyError", ""],
        ["RuntimeError", "Could not open the audio stream"],
        ["ValueError", "bad sample rate"],
        ["RuntimeError", ""],
    ],
    "_recording_destination": [
        [None, "dictation"], [None, "walkthrough"], [_AGENT, "walkthrough"],
        [_AGENT, "action"], [{**_AGENT, "params": {"agent": "codex"}}, "action"],
        [{**_AGENT, "params": {"agent": "opencode_cli"}}, "action"],
        [{**_AGENT, "params": {"agent": "claude_code"}}, "action"],
        [{**_AGENT, "params": {"agent": "someone_else"}}, "action"],
        [{**_AGENT, "params": {"agent": ""}}, "action"],
        [{"name": "Search", "kind": "open_link", "params": {"template": "x"}}, "action"],
        [{"name": "Hook", "kind": "webhook"}, "dictation"],
    ],
    "_reads_selection": [
        [_AGENT], [{**_AGENT, "params": {"include_selection": True}}],
        [{**_AGENT, "params": {"include_selection": False}}],
        [{**_AGENT, "params": {"include_selection": 1}}],
        [{**_AGENT, "params": {"include_selection": "yes"}}],
        [{"name": "Hook", "kind": "webhook", "params": {"include_selection": True}}],
        [{"name": "Hook", "kind": "webhook"}],
    ],
    "_report_busy": [
        ["starting another recording", False], ["processing this recording", False],
        ["re-transcribing audio", False], ["uploading audio", False], ["rewriting text", False],
        ["pasting", False], ["recovering a walkthrough", False],
        ["starting another recording", True],
    ],
    "_show_listening": [
        ["dictation", None, None], ["dictation", None, "Email"], ["dictation", None, ""],
        ["command", None, None], ["command", "Ship it", "Email"], ["action", "Ship it", None],
        ["action", "", None], ["walkthrough", None, None], ["walkthrough", None, "Email"],
        ["transform", None, None],
    ],
    "_error_status": [
        ["Failed to process audio: boom", "dictation", False],
        ["Set up AI cleanup to use Command Mode", "command", True],
        ["Transcription canceled", None, False],
        ["Transcription canceled", "walkthrough", False],
        ["", "dictation", False],
    ],
    "_save_failed_recording": [
        ["Error: Failed to process audio: boom", "saved", False],
        ["Error: Failed to process audio: boom", "saved", True],
        ["Error: Failed to process audio: boom", "not_retained", False],
        ["Error: Failed to process audio: boom", "failed", False],
    ],
    "_preserve_failed_audio": [
        ["Error: x", "saved", "recording_2026-11-14_10-00-00.wav"],
        ["Error: x", "not_retained", ""],
        ["Error: x", "missing", ""],
        ["Error: x", "failed", ""],
    ],
    "_delivery_notice": [
        ["", None, None], ["", "Email", None], ["", "Email", ""],
        ["", "Email", "timed out"], ["", None, "timed out"],
        ["", "Email", "cleanup unavailable"], ["", "Support ticket", "snippet placeholder lost"],
        ["", "Email", "canceled"], ["", "Email", "timed out after 17 s"],
        ["This action's transform no longer exists — sent your cleaned words", "Email",
         "timed out"],
    ],
    "constants": [[]],
}


# M2.16 dictation-uploads: the texts a finished batch shows. A file travels as {"path", "text",
# "raw_text", "error"} (each optional but the path); a result as {"relation", "custom_combine",
# "items", "combined_text", "combined_raw_text", "cleanup_error"}; _batch_status takes the number
# of saveable files.


def _batch_item_result(fields):
    from services.batch_upload import BatchItem, BatchItemResult

    return BatchItemResult(BatchItem(fields["path"]), text=fields.get("text", ""),
                           raw_text=fields.get("raw_text"), error=fields.get("error"))


def _batch_result(fields):
    from services.batch_upload import BatchResult, BatchUploadRequest

    items = tuple(_batch_item_result(item) for item in fields["items"])
    request = BatchUploadRequest(items=tuple(item.item for item in items),
                                 relation=fields.get("relation", "separate"),
                                 custom_combine=fields.get("custom_combine", False))
    return BatchResult(request=request, items=items,
                       combined_text=fields.get("combined_text"),
                       combined_raw_text=fields.get("combined_raw_text"),
                       cleanup_error=fields.get("cleanup_error"))


def _batch_line(module, item, raw):
    return module._batch_line(_batch_item_result(item), raw=raw)


def _batch_display(module, result, is_empty):
    return list(module._batch_display(_batch_result(result), is_empty))


def _batch_status(module, result, saveable, canceled):
    built = _batch_result(result)
    return module._batch_status(built, list(built.items[:saveable]), canceled=canceled)


WRAPPERS.update({
    "_batch_line": _batch_line,
    "_batch_display": _batch_display,
    "_batch_status": _batch_status,
})

_A = {"path": "C:/rec/a.wav", "text": " first words. "}
_B = {"path": "C:\\rec\\b.mp3", "text": "second words."}
_C = {"path": "D:/x/c.m4a", "text": "third."}
_FAILED = {"path": "C:/rec/f.wav", "error": "boom"}
_EMPTY = {"path": "C:/rec/e.wav", "text": "   "}
_CLEANED = {"path": "C:/rec/r.wav", "text": "Cleaned.", "raw_text": "um cleaned"}

CASES.update({
    "_batch_line": [
        [_A, False], [_A, True], [_CLEANED, False], [_CLEANED, True],
        [{**_CLEANED, "raw_text": ""}, True], [_FAILED, False], [_FAILED, True],
        [{**_A, "error": ""}, False], [_EMPTY, False], [_EMPTY, True],
        [{"path": "x.wav"}, False], [{"path": "x.wav", "text": "\tcafé — ok\n"}, False],
    ],
    "_batch_display": [
        [{"items": [_A, _B, _C]}, False],
        [{"items": [_A, _CLEANED]}, False],
        [{"items": [_A, _FAILED, _EMPTY]}, False],
        [{"items": [_EMPTY, _EMPTY]}, True],
        [{"items": [_A, {**_CLEANED, "raw_text": ""}]}, False],
        [{"relation": "sequential", "items": [_A, _B], "combined_text": "Joined.",
          "combined_raw_text": "first words.\n\nsecond words."}, False],
        [{"relation": "sequential", "items": [_A, _B], "combined_text": "   "}, True],
        [{"relation": "sequential", "items": [_A, _B]}, False],
        [{"relation": "custom", "custom_combine": True, "items": [_A, _B],
          "combined_text": "Joined."}, False],
        [{"relation": "custom", "custom_combine": False, "items": [_A, _B],
          "combined_text": "Joined."}, False],
    ],
    "_batch_status": [
        [{"items": [_A, _B, _C]}, 3, False],
        [{"items": [_A, _B, _C]}, 1, True],
        [{"items": [_A, _B]}, 0, True],
        [{"relation": "sequential", "items": [_A, _B]}, 2, True],
        [{"relation": "sequential", "items": [_A, _B], "cleanup_error": "canceled"}, 2, False],
        [{"relation": "sequential", "items": [_A, _B], "cleanup_error": "canceled"}, 2, True],
        [{"relation": "sequential", "items": [_A, _B], "cleanup_error": "timed out"}, 2, False],
        [{"relation": "sequential", "items": [_A, _B], "cleanup_error": "cleanup unavailable"},
         2, False],
        [{"relation": "sequential", "items": [_A, _B], "cleanup_error": ""}, 2, False],
        [{"relation": "custom", "custom_combine": True, "items": [_A, _B],
          "cleanup_error": "HTTP 429"}, 2, True],
        [{"items": [_A, _B], "cleanup_error": "timed out"}, 1, True],
    ],
})


# M4.13 command-runtime: what a Command Mode or transform rewrite says when it is copied instead
# of pasted (_copy_rewrite) and after a forced paste (_notify_forced_outcome). The UI stub copies
# as the case says and records the status and the notices; a job travels as its selection text
# (null for none).


class _RewriteUI:
    def __init__(self, copies):
        self.copies = copies
        self.statuses, self.notices, self.copied = [], [], []

    def discard_clipboard_prefetch(self):
        pass

    def copy_to_clipboard(self, text):
        self.copied.append(text)
        return self.copies

    def set_status(self, text):
        self.statuses.append(text)

    def show_notice(self, text, done=False):
        self.notices.append([text, done])


def _copy_rewrite(module, copies, target):
    runtime = _bare(module)
    ui = _RewriteUI(copies)
    runtime.controller.ui_controller = ui
    runtime._copy_rewrite(ui, "Rewritten.", target)
    return {"statuses": ui.statuses, "notices": ui.notices, "copied": ui.copied}


def _notify_forced_outcome(module, selection, pasted, delivery_status):
    from concurrent.futures import Future

    from services.dictation_pipeline import DictationJob, JobMode

    runtime = _bare(module)
    ui = _RewriteUI(True)
    runtime.controller.ui_controller = ui
    runtime._delivery_status = delivery_status
    future = None
    if selection is not None:
        future = Future()
        future.set_result(selection)
    runtime._notify_forced_outcome(DictationJob(mode=JobMode.COMMAND, selection=future), pasted)
    return ui.notices


WRAPPERS.update({
    "_copy_rewrite": _copy_rewrite,
    "_notify_forced_outcome": _notify_forced_outcome,
})

CASES.update({
    "_copy_rewrite": [
        [True, "changed"], [True, "unknown"], [True, "same"],
        [False, "changed"], [False, "unknown"],
    ],
    "_notify_forced_outcome": [
        ["old words", True, "Ready (Pasted)"], ["", True, "Ready (Pasted)"],
        ["   ", True, "Ready (Pasted)"], [None, True, "Ready (Pasted)"],
        ["old words", False, "Transcription complete (paste failed)"],
        ["", False, "Transcription complete (copy failed)"], ["old words", False, ""],
    ],
})


# M6.11 walkthrough-job: what a finished walkthrough says once its documents exist
# (_copy_walkthrough) and when they could not be written (_deliver_walkthrough without a
# result). The settings the copy reads are the case's; the UI stub copies as the case says and
# records the status, the notices, the files copied (by name) and whether text was copied, the
# card's `copied` and the pastes. A result travels as (shots, written_up, write_up_error,
# lost_shots); the session of a missing document as its document_error (null for no session).


class _WalkthroughUI:
    def __init__(self, copies):
        self.copies = copies
        self.statuses, self.notices, self.copied, self.ready = [], [], [], []
        self.pasted = 0
        self.dismissed = 0

    def copy_files_to_clipboard(self, paths, text="", *, image=None):
        self.copied.append([[path.name for path in paths], bool(text)])
        return self.copies

    def set_status(self, text):
        self.statuses.append(text)

    def show_notice(self, text, done=False):
        self.notices.append(text)

    def show_walkthrough_ready(self, result, *, copied):
        self.ready.append(copied)

    def paste_walkthrough(self, result, target):
        self.pasted += 1

    def dismiss_walkthrough_panel(self):
        self.dismissed += 1


def _walkthrough_result(shots, written_up, write_up_error, lost_shots):
    from pathlib import Path

    from services.walkthrough.session import WalkthroughResult

    folder = Path("C:/captures/2026-10-21_14-32-00")
    stem = "write-up" if written_up else "walkthrough"
    return WalkthroughResult(
        folder=folder, pdf=folder / f"{stem}.pdf", markdown=folder / f"{stem}.md",
        clipboard_text="# Walkthrough", history_text="words", history_raw=None, shots=shots,
        duration_s=12.0, first_shot=None, written_up=written_up, write_up_error=write_up_error,
        lost_shots=lost_shots)


def _copy_walkthrough(module, copy_format, auto_paste, copies, shots, written_up,
                      write_up_error, lost_shots):
    runtime = _bare(module)
    ui = _WalkthroughUI(copies)
    runtime.controller.ui_controller = ui
    settings = {"walkthrough_copy_format": copy_format, "auto_paste": auto_paste}
    original = module.settings_manager
    module.settings_manager = types.SimpleNamespace(load_all_settings=lambda: dict(settings))
    try:
        runtime._copy_walkthrough(ui, _walkthrough_result(shots, written_up, write_up_error,
                                                          lost_shots))
    finally:
        module.settings_manager = original
    return {"statuses": ui.statuses, "copied": ui.copied, "ready": ui.ready,
            "pasted": ui.pasted}


def _deliver_walkthrough(module, document_error, transcript, copies):
    runtime = _bare(module)
    ui = _WalkthroughUI(copies)
    runtime.controller.ui_controller = ui
    if document_error is not None:
        runtime._walkthrough = types.SimpleNamespace(result=None, document_error=document_error)
    runtime._deliver_walkthrough(ui, transcript)
    return {"statuses": ui.statuses, "notices": ui.notices, "copied": ui.copied,
            "dismissed": ui.dismissed}


WRAPPERS.update({
    "_copy_walkthrough": _copy_walkthrough,
    "_deliver_walkthrough": _deliver_walkthrough,
})


def _copy_walkthrough_cases():
    import itertools

    cases = []
    combinations = itertools.product(
        ["pdf", "agent_task", "both", "agent", "word", "unknown"], [True, False],
        [True, False], ["", "Claude Code is not signed in."], [0, 1, 2])
    for index, (copy_format, auto_paste, copies, error, lost) in enumerate(combinations):
        shots = [0, 1, 3][index % 3]
        written_up = index % 2 == 0 and not error
        cases.append([copy_format, auto_paste, copies, shots, written_up, error, lost])
    return cases


CASES.update({
    "_copy_walkthrough": _copy_walkthrough_cases(),
    "_deliver_walkthrough": [
        [error, transcript, copies]
        for error in [None, "", "No space left on device"]
        for transcript in ["", "first part\n\n[Screenshot 1]"]
        for copies in [True, False]
    ],
})