"""Curated inputs for ui_qt/dialogs/settings_hooks.py (M5.17 hooks-page): the Hooks rail value over
the saved webhooks, programs and text filters.
"""

_HOOK = {"id": "hook01", "name": "Home", "url": "https://example.com/in",
         "events": ["transcription.completed"], "enabled": True}
_OFF = dict(_HOOK, id="hook02", name="Office", enabled=False)
_PROGRAM = {"id": "prog01", "name": "Journal", "program": "C:\\Tools\\journal.exe",
            "args": ["--append"], "events": ["history.saved"], "enabled": True, "timeout_s": 30}
_FILTER = {"id": "filt01", "name": "Names", "kind": "http", "stage": "after_cleanup",
           "url": "http://127.0.0.1:5005/fix", "timeout_ms": 1500, "enabled": False}

CASES = {
    "rail_value": [[settings] for settings in (
        {},
        {"automation_webhooks": []},
        {"automation_webhooks": [_HOOK]},
        {"automation_webhooks": [_OFF]},
        {"automation_webhooks": [_HOOK, _OFF]},
        {"automation_webhooks": [_HOOK], "automation_programs": [_PROGRAM],
         "automation_filters": [_FILTER]},
        {"automation_programs": [_PROGRAM]},
        {"automation_filters": [_FILTER]},
        {"automation_filters": [_FILTER, dict(_FILTER, id="filt02", name="More", enabled=True)]},
        {"automation_webhooks": [_HOOK, dict(_HOOK, id="hook01", name="Again")]},
        {"automation_webhooks": [dict(_HOOK, url="ftp://example.com")]},
        {"automation_webhooks": "oops", "automation_programs": None},
        {"automation_webhooks": [_HOOK, "oops", 7]},
        {"automation_webhooks": [dict(_HOOK, enabled="yes")]},
    )],
}

PINS = {
    "program_path_note": "a program's path is C:\\Tools\\journal.exe, absolute on Windows, where "
                         "the vectors are made",
}
