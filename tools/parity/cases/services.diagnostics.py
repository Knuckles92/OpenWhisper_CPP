"""Curated inputs for services/diagnostics.py (M0.5, fv_log failure journal and metrics)."""

import builtins
import logging

import ow_isolation

NAN = float("nan")


def _event(utc="2026-11-14T10:00:00+00:00", component="recorder", category="OSError", line=12,
           **extra):
    return dict(utc=utc, component=component, category=category, line=line, **extra)


CASES = {
    # Persisted events are revalidated one by one: component and category allowlists, an ISO
    # timestamp normalised through fromisoformat().isoformat(), and a line kept only when it is
    # an int (not a bool) in 1..99999. Only the last 50 entries are looked at, before filtering.
    "_failure_events": [
        [None], ["not a list"], [{}], [[]], [[1, "x", None, []]],
        [[_event()]],
        [[_event(utc="2026-11-14T10:00:00Z")]],
        [[_event(utc="2026-11-14T10:00:00")]],
        [[_event(utc="2026-11-14T10:00:00.250000+00:00")]],
        [[_event(utc="2026-11-14T10:00:00.000000+00:00")]],
        [[_event(utc="2026-11-14 10:00:00+01:30")]],
        [[_event(utc="2026-11-14")]],
        [[_event(utc="bad")]], [[_event(utc="")]], [[_event(utc=None)]], [[_event(utc=12)]],
        [[_event(line=0)]], [[_event(line=1)]], [[_event(line=99999)]], [[_event(line=100000)]],
        [[_event(line=-5)]], [[_event(line=True)]], [[_event(line="3")]], [[_event(line=3.0)]],
        [[_event(line=None)]], [[{k: v for k, v in _event().items() if k != "line"}]],
        [[_event(component=c) for c in (
            "recorder", "audio_processor", "application_controller", "database",
            "history_manager", "runtime.transcription", "components", "local_asr",
            "streaming_transcriber", "app_update", "remote_engine", "transcriber", "ui",
            "meeting", "application", "services.recorder", "Recorder", "", None, 3)]],
        [[_event(category=c) for c in (
            "TimeoutError", "PermissionError", "FileNotFoundError", "MemoryError",
            "ConnectionError", "OSError", "ValueError", "RuntimeError", "Error", "LoggedError",
            "KeyError", "ConnectionResetError", "oserror", "", None)]],
        [[_event(message="private text", traceback="private")]],
        [[_event(line=n) for n in range(1, 61)]],
        [[_event(line=n) for n in range(1, 51)] + ["junk"] * 5],
        [["junk"] * 10 + [_event(line=n) for n in range(1, 51)]],
    ],
    # Unknown names and non-list values are dropped; samples are the last 100 before filtering,
    # kept when int or float (not bool), finite and in 0..1e12.
    "_metric_samples": [
        [None], [[]], ["x"], [{}],
        [{"stop_to_result_s": [1, 2.5]}],
        [{"secret": [1], "stop_to_result_s": [1]}],
        [{"stop_to_result_s": "1"}], [{"stop_to_result_s": None}], [{"stop_to_result_s": {}}],
        [{"stop_to_result_s": [NAN, float("inf"), float("-inf"), 3]}],
        [{"stop_to_result_s": [1e12, 1e12 + 1, 1e13, 10 ** 12, 10 ** 12 + 1, 10 ** 13]}],
        [{"stop_to_result_s": [0, -0.0, -1, -1e-9, 0.0]}],
        [{"captured_frames": [True, False, 1, 0]}],
        [{"cancel_s": ["1", None, [], {}, 2]}],
        [{"shutdown_s": list(range(150))}],
        [{"shutdown_s": list(range(99)) + [NAN] * 51}],
        [{name: [1] for name in (
            "startup_to_window_s", "stop_to_result_s", "cancel_s", "shutdown_s",
            "captured_frames", "dropped_frames", "device_switches", "input_overflows",
            "peak_rss_mb", "context_capture_ms", "context_capture_timeouts",
            "mouse_hook_callback_ms", "unknown_metric")}],
        [{"dropped_frames": [], "peak_rss_mb": [512.25]}],
    ],
    # Percentiles interpolate linearly over the sorted samples; latest is the last recorded.
    "_metric_summary": [
        [None], [{}], [{"stop_to_result_s": []}], [{"stop_to_result_s": [NAN]}],
        [{"stop_to_result_s": [5]}],
        [{"stop_to_result_s": [5.5]}],
        [{"stop_to_result_s": [3, 1]}],
        [{"stop_to_result_s": [3, 1, 2]}],
        [{"stop_to_result_s": [2, 2.0, 1]}],
        [{"stop_to_result_s": [7, 3, 9, 1, 12, 5, 8, 2, 20, 4, 6, 11, 10, 15, 13, 14, 19, 17,
                               16, 18]}],
        [{"stop_to_result_s": [0.1 * n for n in range(20)]}],
        [{"stop_to_result_s": list(range(110))}],
        [{"captured_frames": [1000, 2000], "secret": [1], "cancel_s": [0.25, NAN]}],
        [{"shutdown_s": [1e12, 0]}],
    ],
    # FailureCapture's component and category mapping, through a LogRecord built as the logging
    # module builds it. The result is the event without its time.
    "FailureCapture": [
        [name, exception]
        for name in ("services.recorder", "services.recorder.x", "services.recorderx",
                     "services.runtime.transcription", "services.runtime.transcription.job",
                     "services.runtime", "services.remote_engine", "services", "transcriber.y",
                     "transcriber", "ui_qt.z", "ui_qt", "meeting.a", "meeting", "other", "root",
                     "qt", "")
        for exception in (None,)
    ] + [
        ["services.recorder", exception]
        for exception in ("TimeoutError", "FileNotFoundError", "PermissionError",
                          "ConnectionResetError", "ConnectionRefusedError", "BrokenPipeError",
                          "ConnectionError", "OSError", "MemoryError", "ValueError",
                          "UnicodeDecodeError", "RuntimeError", "NotImplementedError",
                          "RecursionError", "KeyError", "IndexError", "TypeError", "Exception",
                          None)
    ],
}


def _exception(name):
    if name is None:
        return None
    if name == "UnicodeDecodeError":
        return UnicodeDecodeError("utf-8", b"\xff", 0, 1, "invalid start byte")
    return getattr(builtins, name)()


def _failure_capture(module, logger_name, exception_name):
    folder = ow_isolation.SCRATCH / "diagnostics-cases"
    folder.mkdir(parents=True, exist_ok=True)
    path = folder / "journal.json"
    if path.exists():
        path.unlink()
    handler = module.FailureCapture(path)
    error = _exception(exception_name)
    exc_info = (type(error), error, None) if error is not None else None
    record = logging.LogRecord(logger_name, logging.ERROR, "private/path.py", 44, "message", (),
                               exc_info)
    handler.handle(record)
    event = dict(handler.events[-1])
    del event["utc"]
    return event


WRAPPERS = {"FailureCapture": _failure_capture}

# _metric_samples is left out: the stalled-storage test calls it once per recorded value, a
# thousand single-sample dicts that add nothing to the curated cases.
TRACE = {
    "tests": ["tests/test_diagnostics.py"],
    "functions": ["_failure_events", "_metric_summary"],
}

PINS = {
    "failure_capture_note": "FailureCapture cases return the appended event without its utc; "
                            "the record's line is 44",
}
