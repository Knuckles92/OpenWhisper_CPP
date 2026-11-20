"""Curated inputs for services/automation/catalog.py.

user_started (M1.19 dictation-core) decides whether a job may read what is on screen. The rest
(M5.3 automation-core): every payload builder over fake jobs and actions, known_types, and the
catalogue's tables in order.

A job is given as a dict of DictationJob fields (a real DictationJob is built from it) or None;
an action as {"id", "name", "kind"}; a cleanup as {"provider", "model", "level"} or None.
"""

from types import SimpleNamespace


def _job(spec):
    if spec is None:
        return None
    from services.dictation_pipeline import DictationJob

    return DictationJob(**spec)


def _action(spec):
    return SimpleNamespace(**spec)


def _cleanup(spec):
    return None if spec is None else SimpleNamespace(**spec)


WRAPPERS = {
    "job_fields": lambda m, job: m.job_fields(_job(job)),
    "recording_started": lambda m, job: m.recording_started(_job(job)),
    "recording_stopped": lambda m, job, duration: m.recording_stopped(_job(job), duration),
    "recording_canceled": lambda m, job: m.recording_canceled(_job(job)),
    "transcription_preview": lambda m, job, text, final: m.transcription_preview(
        _job(job), text, final),
    "transcription_completed": lambda m, job, text, raw_text, fields, cleanup, audio_s: (
        m.transcription_completed(_job(job), text=text, raw_text=raw_text, fields=fields,
                                  cleanup_info=_cleanup(cleanup), audio_s=audio_s)),
    "transcription_failed": lambda m, job, error, message: m.transcription_failed(
        _job(job), error, message=message),
    "action_ran": lambda m, job, action, ok, message, output, text: m.action_ran(
        _job(job), _action(action), ok=ok, message=message, output=output, text=text),
    "channel_message": lambda m, job, action, text, app: m.channel_message(
        _job(job), _action(action), text, app),
    "route_decided": lambda m, job, action_id, confidence, fallback: m.route_decided(
        _job(job), action_id, confidence, fallback),
    "event_types": lambda m: [[name, text] for name, text in m.EVENT_TYPES.items()],
    "public_types": lambda m: list(m.PUBLIC_TYPES),
    "constants": lambda m: {
        "SOURCES": list(m.SOURCES), "USER_SOURCES": sorted(m.USER_SOURCES),
        "FAILURES": sorted(m.FAILURES), "ORIGINS": [m.ORIGIN_DICTATED, m.ORIGIN_PROGRAM],
        "FILE_MODE": m.FILE_MODE,
        "GAPS": [m.GAP_EVICTED, m.GAP_RESTARTED, m.GAP_SLOW]},
}

_DICTATION = {"id": "0123456789abcdef", "mode": "dictation", "source": "hotkey"}
_ACTION_JOB = {"id": "aaaabbbbccccdddd", "mode": "action", "action_id": "a1", "source": "api",
               "route": "", "routed": True}
_ROUTED = {"id": "1111222233334444", "mode": "action", "action_id": "r", "source": "hotkey",
           "route": "hotkey", "routed": True}
_COMMAND = {"id": "c0c0c0c0c0c0c0c0", "mode": "command", "source": "ui"}
_TRANSFORM = {"id": "7777777777777777", "mode": "transform", "source": "dbus"}
_WALKTHROUGH = {"id": "5555555555555555", "mode": "walkthrough", "source": "hotkey"}
_PROGRAM_CHANGED = {"id": "9999999999999999", "mode": "action", "action_id": "c",
                    "source": "hotkey", "changed_by_program": True}
_UNKNOWN_SOURCE = {"id": "8888888888888888", "mode": "dictation", "source": "dbus2"}
_MCP = {"id": "6666666666666666", "mode": "action", "action_id": "c", "source": "mcp"}
_JOBS = [None, _DICTATION, _ACTION_JOB, _ROUTED, _COMMAND, _TRANSFORM, _WALKTHROUGH,
         _PROGRAM_CHANGED, _UNKNOWN_SOURCE, _MCP]

_HOOK = {"id": "w1", "name": "Home", "kind": "webhook"}
_SCRIPTS = {"id": "s", "name": "Send to my scripts", "kind": "scripts"}
_CHANNEL = {"id": "c", "name": "Tell my Claude Code session", "kind": "channel"}

_FIELDS = {"app_id": "notepad.exe", "app_name": "Notepad", "app_category": "docs",
           "language": "en"}

CASES = {
    "user_started": [
        ["hotkey"], ["ui"], ["dbus"], ["api"], ["mcp"], [""], ["HOTKEY"], ["hotkey "],
        ["program"], [None], [1],
    ],
    "job_fields": [[job] for job in _JOBS],
    "recording_started": [[job] for job in _JOBS],
    "recording_stopped": [
        [None, None], [_DICTATION, 1.23456], [_DICTATION, 2.0005], [_DICTATION, 0.0],
        [_ACTION_JOB, 12], [_COMMAND, 2.675], [_DICTATION, 1e-05],
    ],
    "recording_canceled": [[job] for job in _JOBS],
    "transcription_preview": [
        [_DICTATION, "hel", False], [_DICTATION, "hello world", True], [None, "", False],
        [_ACTION_JOB, None, True], [_DICTATION, "caf\u00e9 \U0001f600", False],
    ],
    "transcription_completed": [
        [None, "hi", "hi", None, None, None],
        [None, "Hi there.", "hi there", None, None, 3.0],
        [_ACTION_JOB, "Hi.", "hi", _FIELDS, None, 1.23456],
        [_DICTATION, "Hello.", "hello", {"language": "de"},
         {"provider": "openrouter", "model": "m", "level": "medium"}, 0.0],
        [_DICTATION, "Same", "Same", {}, None, None],
        [_DICTATION, "Text", "", {"app_id": "", "app_name": "X"}, None, 2.5],
        [_DICTATION, "Text", None, {"app_id": "a.exe"}, None, 0.0004],
        [_COMMAND, "Rewritten", "selected text", _FIELDS, None, 1.0],
        [_TRANSFORM, "Rewritten", "selected text", None,
         {"provider": "", "model": "", "level": ""}, None],
        [_WALKTHROUGH, "Doc", "doc", None, None, 60.0],
        [_ROUTED, "Turn on the porch light", "home turn on the porch light", _FIELDS, None, 2.0],
        [_UNKNOWN_SOURCE, "", None, None, None, None],
        [_DICTATION, None, "raw", None, None, None],
    ],
    "transcription_failed": [
        [None, "no_speech", None], [_DICTATION, "canceled", None],
        [_COMMAND, "refused", "Select text first"], [_COMMAND, "refused", None],
        [_COMMAND, "refused", ""], [_DICTATION, "failed", "C:\\secret\\path.wav"],
        [_DICTATION, "timeout", "anything"], [_ACTION_JOB, "refused", "x" * 600],
        [_DICTATION, "", None],
    ],
    "history_saved": [
        ["abc", "12", "dictation"], ["", "7", ""], [None, "3", None], ["j", "", "action"],
    ],
    "app_from_fields": [
        [None], [{}], [_FIELDS], [{"app_id": "x.exe"}], [{"app_id": ""}],
        [{"app_id": "y.exe", "app_name": None, "app_category": ""}],
    ],
    "known_types": [
        [["history.saved", "nope", "history.saved", "events.gap"]],
        [[]], [["meeting.segments", "action.ran", "recording.started", "ACTION.RAN", ""]],
        [["events.gap"]],
        [["transcription.preview", "transcription.completed", "transcription.failed",
          "channel.message", "route.decided", "filter.failed", "meeting.started",
          "meeting.ended", "meeting.voice_command", "recording.stopped",
          "recording.canceled"]],
    ],
    "action_ran": [
        [_ACTION_JOB, _HOOK, True, "Sent to Home \u2014 nothing came back", "none", None],
        [None, _HOOK, False, "", "", None],
        [_DICTATION, _SCRIPTS, True, None, None, "the words"],
        [_ROUTED, _CHANNEL, False, "y" * 700, "notice", ""],
        [_MCP, _CHANNEL, True, "ok", "scratchpad", None],
    ],
    "channel_message": [
        [_DICTATION, _CHANNEL, "fix the failing test", None],
        [_ROUTED, _CHANNEL, "words", {"id": "code.exe", "name": "Code", "category": "code"}],
        [_ACTION_JOB, _CHANNEL, "from a program", {}],
        [_PROGRAM_CHANGED, _CHANNEL, "changed", None],
        [_UNKNOWN_SOURCE, _CHANNEL, "unknown", None],
        [None, _CHANNEL, "no job", None],
        [_MCP, _CHANNEL, None, None],
    ],
    "route_decided": [
        [_ROUTED, "r", 0.87654321, False], [_DICTATION, None, None, True],
        [_DICTATION, "", 0.5, True], [None, "x", 1, False], [_ROUTED, "r", 0.00005, False],
        [_ROUTED, "r", 0.12345, False],
    ],
    "filter_failed": [
        ["f1", "Fix names", "after_cleanup", "timed out"],
        ["f2", "Local", "before_cleanup", "z" * 250],
        ["", "", "", ""],
    ],
    "meeting_started": [["m1", "Weekly sync"], ["", ""], [None, None]],
    "meeting_ended": [["m1", "completed"], ["m2", ""], [None, None]],
    "meeting_segments": [
        ["m1", []],
        ["m1", [{"segment_id": "s1", "speaker": "Ann", "text": "Hello", "start_s": 0.5,
                 "end_s": 1.25}]],
        ["m1", [{"id": 42, "text": "No speaker"}, {"segment_id": "", "id": "x"},
                {"segment_id": 7, "speaker": None, "text": None, "start_s": 3}]],
        [None, [{}]],
    ],
    "meeting_voice_command": [["m1", "take a note"], ["", None]],
    "events_gap": [[12, "evicted"], [0, "restarted"], [1790000000124, "slow"]],
    "event_types": [[]],
    "public_types": [[]],
    "constants": [[]],
}
