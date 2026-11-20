"""Curated inputs for ui_qt/clipboard.py (M1.17 clipboard). The C++ replay is
tests/clipboard/test_golden.cpp.

- _html_document over fragments, whole documents and the edges of its "<html" test (leading
  Unicode whitespace, case, a prefix that only looks like the tag).
- TemporaryClipboard: event traces. Each case is [name, counter, steps]; a step drives the real
  TemporaryClipboard over a stand-in clipboard (a QObject with a Windows-style change counter
  that every write bumps and a dataChanged signal), and the state after it is recorded. The
  queued hops and the three timers are driven directly, as their slots would run: the timers are
  swapped for stand-ins that only record start and stop, "fire_*" runs a timer's slot when it is
  active, and "poll" moves a fake monotonic clock and runs one selection poll. No QApplication is
  made, so the module needs no display. counter false means no sequence source at all.

Steps:
  ["copy", text]               another app copies text: emptied, filled (two changes), signalled
  ["copy_quiet", text]         another app's copy Qt does not signal (one change)
  ["reown", text]              a clipboard manager re-owns the clipboard: same text, signalled
  ["edit", text]               the content changes with no change counted or signalled
  ["counter_hidden", bool]     the sequence source answers None while true
  ["fail_reads", bool], ["fail_writes", bool]
  ["stage", text, html|null, restore]   result: [written, has lease, restore_unavailable]
  ["schedule_restore", ms], ["commit", text], ["restore_now"]   act on the newest lease
  ["fire_restore"], ["prefetch", ms], ["fire_prefetch"], ["discard_prefetch"], ["cleanup"]
  ["write_text", text, html]
  ["select", timeout_ms, raises]   capture_selection with a send_copy that does nothing (or raises)
  ["poll", advance_ms]

State after each step: {"result", "text", "html", "marker", "counter", "writes", "pending",
"restore_timer", "prefetched", "prefetch_timer", "selecting", "captures", "selected",
"failures"}: the clipboard's text and html (None when absent), whether our marker is there, the
change counter, how many writes this app made, the private state OpenWhisper's tests also read,
how many clipboard captures were taken, every selection callback result so far, and every
restore_failed message so far.
"""

from __future__ import annotations

from types import SimpleNamespace
from unittest.mock import patch

_HTML = "Thanks, <b>Dana Lee</b><br>Product designer"


def _clipboard_class():
    from PyQt6.QtCore import QMimeData, QObject, pyqtSignal

    class TraceClipboard(QObject):
        dataChanged = pyqtSignal()

        def __init__(self):
            super().__init__()
            self.counter = 1
            self.counter_hidden = False
            self.fail_reads = False
            self.fail_writes = False
            self.writes = 0
            self.mime = QMimeData()

        def sequence(self):
            return None if self.counter_hidden else self.counter

        def mimeData(self):
            if self.fail_reads:
                raise RuntimeError("clipboard read failed")
            return self.mime

        def setMimeData(self, mime):
            if self.fail_writes:
                raise RuntimeError("clipboard write failed")
            self.writes += 1
            self.mime = mime
            self.counter += 1
            self.dataChanged.emit()

        def setText(self, text):
            if self.fail_writes:
                raise RuntimeError("clipboard write failed")
            mime = QMimeData()
            mime.setText(text)
            self.writes += 1
            self.mime = mime
            self.counter += 1
            self.dataChanged.emit()

        def foreign(self, text, changes, signal):
            mime = QMimeData()
            mime.setText(text)
            self.mime = mime
            self.counter += changes
            if signal:
                self.dataChanged.emit()

    return TraceClipboard


class _Timer:
    """Stands in for a QTimer: records start and stop, never fires on its own."""

    def __init__(self):
        self.active = False

    def start(self, *_args):
        self.active = True

    def stop(self):
        self.active = False

    def isActive(self):
        return self.active


def _state(module, temporary, clipboard, captures, selected, failures, result):
    mime = clipboard.mime
    return {
        "result": result,
        "text": mime.text() if mime.hasText() else None,
        "html": mime.html() if mime.hasHtml() else None,
        "marker": mime.hasFormat(module.AUTO_PASTE_MARKER_FORMAT),
        "counter": clipboard.counter,
        "writes": clipboard.writes,
        "pending": temporary._pending is not None,
        "restore_timer": temporary._restore_timer.isActive(),
        "prefetched": temporary._prefetched is not None,
        "prefetch_timer": temporary._prefetch_timer.isActive(),
        "selecting": temporary._selection is not None,
        "captures": len(captures),
        "selected": list(selected),
        "failures": list(failures),
    }


def _trace(module, name, counter, steps):
    del name  # names the scenario in the vector file only
    clipboard = _clipboard_class()()
    # Whole milliseconds from 0, so a deadline (now + timeout_ms / 1000, taken at 0) and the time
    # compare exactly as the C++ replay's integer clock does.
    clock = SimpleNamespace(ms=0)
    fake_time = SimpleNamespace(monotonic=lambda: clock.ms / 1000,
                                perf_counter=lambda: clock.ms / 1000)
    captures, selected, failures, leases = [], [], [], []
    original_capture = module.ClipboardSnapshot.capture.__func__

    def counting_capture(cls, mime_data):
        captures.append(True)
        return original_capture(cls, mime_data)

    with patch.object(module, "time", fake_time), \
            patch.object(module.ClipboardSnapshot, "capture", classmethod(counting_capture)), \
            patch.object(module, "system_text_renderer", lambda: None), \
            patch.object(module, "system_html_renderer", lambda: None):
        temporary = module.TemporaryClipboard(
            clipboard, sequence_source=clipboard.sequence, sees_foreign_copies=True)
        if not counter:
            temporary._sequence = None
        temporary._restore_timer = _Timer()
        temporary._prefetch_timer = _Timer()
        temporary._selection_timer = _Timer()
        temporary.restore_failed.connect(failures.append)

        states = []
        for step in steps:
            op, args = step[0], step[1:]
            result = None
            if op == "copy":
                clipboard.foreign(args[0], 2, True)
            elif op == "copy_quiet":
                clipboard.foreign(args[0], 1, False)
            elif op == "reown":
                clipboard.foreign(args[0], 1, True)
            elif op == "edit":
                clipboard.mime.setText(args[0])
            elif op == "counter_hidden":
                clipboard.counter_hidden = args[0]
            elif op == "fail_reads":
                clipboard.fail_reads = args[0]
            elif op == "fail_writes":
                clipboard.fail_writes = args[0]
            elif op == "stage":
                stage = temporary.stage_text(args[0], args[1], restore=args[2])
                if stage.lease is not None:
                    leases.append(stage.lease)
                result = [stage.written, stage.lease is not None, stage.restore_unavailable]
            elif op == "schedule_restore":
                result = temporary.schedule_restore(leases[-1], args[0])
            elif op == "commit":
                result = temporary.commit_text(leases[-1], args[0])
            elif op == "restore_now":
                result = temporary.restore_now(leases[-1]).value
            elif op == "fire_restore":
                if temporary._restore_timer.isActive():
                    temporary._restore_timer.stop()
                    temporary._restore_pending()
            elif op == "prefetch":
                temporary._start_prefetch(args[0])
            elif op == "fire_prefetch":
                if temporary._prefetch_timer.isActive():
                    temporary._prefetch_timer.stop()
                    temporary._capture_prefetch()
            elif op == "discard_prefetch":
                temporary._discard_prefetch()
            elif op == "cleanup":
                temporary.cleanup()
            elif op == "write_text":
                result = temporary.write_text(args[0], args[1])
            elif op == "select":
                timeout_ms, raises = args

                def send_copy(raises=raises):
                    if raises:
                        raise OSError("SendInput failed")

                temporary.capture_selection(send_copy, selected.append, timeout_ms)
            elif op == "poll":
                clock.ms += args[0]
                if temporary._selection_timer.isActive():
                    temporary._poll_selection()
            else:
                raise ValueError(f"unknown step {op}")
            states.append(_state(module, temporary, clipboard, captures, selected, failures,
                                 result))
        return states


WRAPPERS = {"TemporaryClipboard": _trace}

_USER = ["copy", "user text"]

CASES = {
    "_html_document": [
        [""], ["plain"], [_HTML], ["<p>Hi</p>"], ["<html><body><p>Hi</p></body></html>"],
        ["<HTML><body>x</body></HTML>"], ["  \n\t<html>x</html>"], [" <html>x</html>"],
        ["　<Html lang=\"en\">x</Html>"], ["<htm>x</htm>"], ["<html"], ["<htmlx>"],
        ["<!DOCTYPE html><html><body>x</body></html>"], ["x<html>"], ["café “quoted”"],
        ["<b>\U0001F600</b>"], ["\x1c<html>x</html>"], ["İ<html>"],
    ],
    "TemporaryClipboard": [
        ["stage_then_timed_restore", True, [
            _USER, ["stage", "dictated", None, True], ["schedule_restore", 250],
            ["fire_restore"], ["restore_now"]]],
        ["user_copy_before_the_restore", True, [
            _USER, ["stage", "dictated", None, True], ["schedule_restore", 250],
            ["copy", "new user copy"], ["fire_restore"], ["restore_now"]]],
        ["user_copy_before_scheduling", True, [
            _USER, ["stage", "dictated", None, True], ["copy", "new user copy"],
            ["schedule_restore", 250], ["fire_restore"]]],
        ["blank_clipboards_are_not_kept", True, [
            ["stage", "first", None, True], ["copy", " \n\t "], ["stage", "second", None, True],
            ["copy", ""], ["stage", "third", _HTML, True]]],
        ["stage_without_restore", True, [
            _USER, ["stage", "dictated", _HTML, False], ["stage", "next", None, True],
            ["restore_now"]]],
        ["consecutive_stages_restore_the_original", True, [
            _USER, ["stage", "first", None, True], ["schedule_restore", 250],
            ["stage", "second", None, True], ["schedule_restore", 250], ["fire_restore"]]],
        ["prefetch_reused_when_unchanged", True, [
            _USER, ["prefetch", 100], ["fire_prefetch"], ["stage", "dictated", None, True],
            ["restore_now"], ["stage", "again", None, True]]],
        ["prefetch_recaptured_after_a_copy", True, [
            _USER, ["prefetch", 100], ["fire_prefetch"], ["copy", "copied while dictating"],
            ["stage", "dictated", None, True], ["restore_now"]]],
        ["prefetch_recaptured_when_the_counter_is_unknown", True, [
            _USER, ["prefetch", 0], ["fire_prefetch"], ["counter_hidden", True],
            ["stage", "dictated", None, True], ["counter_hidden", False], ["restore_now"]]],
        ["prefetch_skipped_while_the_counter_is_unknown", True, [
            _USER, ["counter_hidden", True], ["prefetch", 0], ["fire_prefetch"],
            ["counter_hidden", False], ["stage", "dictated", None, True]]],
        ["prefetch_skipped_while_a_restore_is_pending", True, [
            _USER, ["stage", "first", None, True], ["schedule_restore", 10000], ["prefetch", 0],
            ["fire_prefetch"], ["stage", "second", None, True], ["restore_now"]]],
        ["prefetch_discarded_and_replaced", True, [
            _USER, ["prefetch", 100], ["discard_prefetch"], ["fire_prefetch"],
            ["prefetch", 0], ["fire_prefetch"], ["prefetch", 10000],
            ["stage", "dictated", None, True]]],
        ["prefetch_dropped_by_a_stage_without_restore", True, [
            _USER, ["prefetch", 0], ["fire_prefetch"], ["stage", "dictated", None, False],
            ["stage", "next", None, True], ["restore_now"]]],
        ["prefetch_off_without_a_counter", False, [
            _USER, ["prefetch", 0], ["fire_prefetch"], ["stage", "dictated", None, True]]],
        ["prefetch_failure_then_stage", True, [
            _USER, ["fail_reads", True], ["prefetch", 0], ["fire_prefetch"],
            ["fail_reads", False], ["stage", "dictated", None, True], ["restore_now"]]],
        ["snapshot_failure_stages_without_a_lease", True, [
            _USER, ["fail_reads", True], ["stage", "dictated", _HTML, True],
            ["fail_reads", False]]],
        ["write_failure_stages_nothing", True, [
            _USER, ["fail_writes", True], ["stage", "dictated", None, True],
            ["write_text", "plain", ""], ["fail_writes", False]]],
        ["restore_failure_reports_the_message", True, [
            _USER, ["stage", "dictated", None, True], ["fail_writes", True], ["restore_now"],
            ["fail_writes", False], ["restore_now"]]],
        ["commit_keeps_the_rich_text_without_the_marker", True, [
            _USER, ["stage", "plain", _HTML, True], ["commit", "plain"], ["restore_now"]]],
        ["commit_after_a_user_copy_fails", True, [
            _USER, ["stage", "plain", None, True], ["copy", "new"], ["commit", "plain"]]],
        ["write_text_drops_the_pending_restore", True, [
            _USER, ["stage", "dictated", None, True], ["schedule_restore", 250],
            ["write_text", "history copy", "<i>history</i>"], ["fire_restore"],
            ["restore_now"]]],
        ["cleanup_restores_and_drops_everything", True, [
            _USER, ["prefetch", 100], ["stage", "dictated", None, True],
            ["schedule_restore", 60000], ["cleanup"]]],
        ["selection_with_a_counter", True, [
            _USER, ["select", 700, False], ["poll", 10], ["copy", "selected words"],
            ["poll", 10], ["poll", 10], ["stage", "rewritten", None, True], ["restore_now"]]],
        ["selection_with_a_counter_times_out", True, [
            _USER, ["select", 80, False], ["poll", 40], ["poll", 40], ["poll", 10]]],
        ["selection_with_a_counter_expires_while_settling", True, [
            _USER, ["select", 30, False], ["poll", 10], ["copy", "late"], ["poll", 30]]],
        ["selection_without_a_counter_signalled", False, [
            _USER, ["select", 700, False], ["poll", 10], ["copy", "selected words"],
            ["poll", 10]]],
        ["selection_without_a_counter_unsignalled", False, [
            _USER, ["select", 700, False], ["copy_quiet", "selected paragraph"], ["poll", 10],
            ["poll", 10], ["poll", 10], ["poll", 10], ["poll", 10], ["poll", 10]]],
        ["selection_without_a_counter_times_out", False, [
            _USER, ["select", 80, False], ["poll", 50], ["poll", 50]]],
        ["selection_without_a_counter_manager_reowns", False, [
            _USER, ["select", 80, False], ["reown", "user text"], ["poll", 10], ["poll", 10],
            ["poll", 70]]],
        ["selection_copy_fails", True, [
            _USER, ["select", 700, True], ["stage", "dictated", None, True]]],
        ["selection_copy_fails_without_a_counter", False, [_USER, ["select", 700, True]]],
        ["selection_one_at_a_time_and_a_stage_ends_it", True, [
            _USER, ["select", 700, False], ["copy", "partial"], ["select", 700, False],
            ["stage", "dictated", None, True], ["restore_now"]]],
        ["selection_after_a_pending_paste", True, [
            _USER, ["stage", "dictated", None, True], ["schedule_restore", 250],
            ["select", 700, False], ["copy", "selected"], ["poll", 10], ["poll", 10]]],
        ["selection_reuses_a_valid_prefetch", True, [
            _USER, ["prefetch", 0], ["fire_prefetch"], ["edit", "changed without a write"],
            ["select", 700, False], ["copy", "selected"], ["poll", 10], ["poll", 10]]],
        ["selection_snapshot_failure", True, [
            _USER, ["fail_reads", True], ["select", 700, False], ["fail_reads", False]]],
        ["selection_restore_failure", True, [
            _USER, ["select", 700, False], ["copy", "selected"], ["fail_writes", True],
            ["poll", 10], ["poll", 10], ["fail_writes", False], ["stage", "x", None, True]]],
        ["prefetch_skipped_during_a_selection", True, [
            _USER, ["prefetch", 0], ["select", 700, False], ["prefetch", 0], ["fire_prefetch"],
            ["write_text", "plain", ""]]],
    ],
}
