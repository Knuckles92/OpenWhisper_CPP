"""Curated inputs for ui_qt/dialogs/settings_dialog.py (M2.2 appcore/rail_values.h and
appcore/retention_policy.h).

Only the dialog's pure decisions are recorded, each run on a stand-in rather than a built
window: the General, Appearance, Hotkeys and Developer rail values, and the retention commit,
its confirmation and the usage caption. _render_rail_values computes the Appearance and
Developer values inline, so their wrappers evaluate the same expressions over the module's own
resolvers and labels. The methods (_hotkey_rail_value, _commit_retention,
_confirm_recording_removal, _apply_recordings_usage) run as themselves on a SimpleNamespace that
records what they would show, ask and save; history_manager and QMessageBox are replaced for the
call, and the confirmation answers as the vector says.
"""
from __future__ import annotations

from types import SimpleNamespace


class _Patched:
    def __init__(self, module, **values):
        self.module, self.values, self.saved = module, values, {}

    def __enter__(self):
        for name, value in self.values.items():
            self.saved[name] = getattr(self.module, name)
            setattr(self.module, name, value)

    def __exit__(self, *_exc):
        for name, value in self.saved.items():
            setattr(self.module, name, value)


def _appearance(module, settings):
    return (f"{module.UiTheme.LABELS[module.resolve_ui_theme(settings)]} · "
            f"{module.RecordingOverlayLook.LABELS[module.resolve_recording_overlay_look(settings)]}")


def _developer(module, settings):
    return "Developer mode on" if module.resolve_developer_mode(settings) else "Developer mode off"


def _cleanup_rail_value(module, settings):
    """M4.12: the AI cleanup rail value, as _render_rail_values computes it inline."""
    model = module.resolve_transcript_cleanup_model(settings)
    cleanup = module.cleanup_prompts.level_label(settings)
    if module.cleanup_prompts.resolve_level(settings) != module.CleanupLevel.NONE and model:
        cleanup = f"{cleanup} · {model}"
    return cleanup


def _hotkey_rail_value(module, hotkeys):
    stand_in = SimpleNamespace(current_hotkeys=hotkeys)
    raw = (hotkeys or {}).get("record_toggle", "")
    return {"display": module.format_hotkey_display(raw),
            "value": module.SettingsDialog._hotkey_rail_value(stand_in)}


class _Value:
    def __init__(self, value):
        self._value = value

    def currentData(self):
        return self._value

    def value(self):
        return self._value


def _commit_retention(module, saved, mode, count, mb, answer):
    events = {"confirm": None, "persisted": None, "applied": False, "reloaded": None}

    def confirm(max_recordings, max_bytes):
        events["confirm"] = [max_recordings, max_bytes]
        return answer

    def persist_many(edits, drops=()):
        events["persisted"] = {"" + key: value for key, value in edits.items()}
        return True

    stand_in = SimpleNamespace(
        _retention_commit_timer=SimpleNamespace(stop=lambda: None),
        _loading=False,
        _confirming_retention=False,
        recording_retention_combo=_Value(mode),
        max_recordings_spinbox=_Value(count),
        max_recordings_mb_spinbox=_Value(mb),
        _confirm_recording_removal=confirm,
        _persist_many=persist_many,
        _apply_retention_limit=lambda: events.update(applied=True),
        _load_retention_settings=lambda settings: events.update(reloaded=settings),
    )
    manager = SimpleNamespace(load_all_settings=lambda: dict(saved))
    with _Patched(module, settings_manager=manager):
        module.SettingsDialog._commit_retention(stand_in)
    return events


def _confirm_recording_removal(module, max_recordings, max_bytes, sizes, answer):
    from PyQt6.QtWidgets import QMessageBox

    asked = []

    class _Box:
        StandardButton = QMessageBox.StandardButton

        @staticmethod
        def question(_parent, title, text, _buttons, default):
            asked.append({"title": title, "text": text, "default": default.name})
            return QMessageBox.StandardButton.Yes if answer else QMessageBox.StandardButton.Cancel

    calls = []

    def over_limit(count, size):
        calls.append([count, size])
        return [SimpleNamespace(size_bytes=value) for value in sizes]

    stand_in = SimpleNamespace(_confirming_retention=False,
                               message_label=SimpleNamespace(setText=lambda _text: None))
    with _Patched(module, QMessageBox=_Box,
                  history_manager=SimpleNamespace(recordings_over_limit=over_limit)):
        result = module.SettingsDialog._confirm_recording_removal(stand_in, max_recordings,
                                                                  max_bytes)
    return {"result": result, "previewed": calls, "asked": asked[0] if asked else None}


def _apply_recordings_usage(module, count, total_bytes):
    shown = []
    stand_in = SimpleNamespace(_recording_query_generation=3,
                               recordings_usage_label=SimpleNamespace(setText=shown.append))
    module.SettingsDialog._apply_recordings_usage(stand_in, 3, "", (count, total_bytes), "")
    return shown[0]


def _where_strip(module, settings):
    """The where strip of Home: SettingsDialog._refresh_home on a stand-in whose model page reports
    the cleanup provider the settings name, with the meeting and speaker-label answers fixed
    (those items are other packages'). get_text_llm_profile reads the case's settings, as it reads
    the saved ones in the app."""
    from services import text_llm
    from services.settings import resolve_transcript_cleanup_provider

    shown = {}
    models = SimpleNamespace(
        voice_is_remote=lambda: False, voice_is_cloud=lambda: False, voice_detail=lambda: "",
        active_text_provider=resolve_transcript_cleanup_provider(settings),
        meeting_intelligence_is_remote=lambda _is_remote: False,
        speaker_id_is_remote=lambda: False, runtime_summary=lambda: "CPU",
    )
    downloads = SimpleNamespace(
        storage_summary=lambda: {"downloaded": 0, "total": 0, "bytes": 0, "by_backend": {}},
        is_checking=lambda: False)
    home = SimpleNamespace(
        refresh=lambda: None,
        update_summary=lambda summary: shown.update(local=list(summary.local_items),
                                                    cloud=list(summary.cloud_items)))
    stand_in = SimpleNamespace(
        _pages_ready=True, home=home, models=models, downloads=downloads,
        _settings_snapshot=lambda: settings,
        _provider_is_remote=module.SettingsDialog._provider_is_remote)
    patches = {
        "component_coordinator": SimpleNamespace(list_components=lambda: []),
        "get_text_llm_profile": lambda provider: text_llm.get_profile(provider, settings),
    }
    with _Patched(module, **patches):
        module.SettingsDialog._refresh_home(stand_in)
    return {"provider": models.active_text_provider, "local": shown["local"],
            "cloud": shown["cloud"]}


WRAPPERS = {
    "SettingsDialog._refresh_home": _where_strip,
    "appearance_rail_value": _appearance,
    "developer_rail_value": _developer,
    "cleanup_rail_value": _cleanup_rail_value,
    "SettingsDialog._hotkey_rail_value": _hotkey_rail_value,
    "SettingsDialog._commit_retention": _commit_retention,
    "SettingsDialog._confirm_recording_removal": _confirm_recording_removal,
    "SettingsDialog._apply_recordings_usage": _apply_recordings_usage,
}

_GENERAL = [
    {}, {"auto_paste": True}, {"auto_paste": False}, {"auto_paste": False, "copy_clipboard": False},
    {"auto_paste": False, "copy_clipboard": True}, {"auto_paste": "yes"}, {"auto_paste": 0},
    {"auto_paste": None, "copy_clipboard": False}, {"auto_paste": 1, "copy_clipboard": False},
]

_APPEARANCE = [
    {}, {"ui_theme": "light"}, {"ui_theme": "system", "recording_overlay_look": "ribbon"},
    {"ui_theme": "omarchy", "recording_overlay_look": "legacy"},
    {"ui_theme": "unknown", "recording_overlay_look": "orb"},
    {"recording_overlay_look": "bubbles"}, {"recording_overlay_look": "live"},
    {"recording_overlay_look": "glow", "ui_theme": "dark"}, {"recording_overlay_look": 7},
    {"recording_overlay_look": "fish"}, {"recording_overlay_look": "fizz"},
    {"recording_overlay_look": "wave"}, {"recording_overlay_look": "aurora"},
    {"recording_overlay_look": "dots"}, {"recording_overlay_look": "baitball"},
    {"recording_overlay_look": "pulse"},
]

_DEVELOPER = [{}, {"developer_mode": True}, {"developer_mode": False}, {"developer_mode": "true"},
              {"developer_mode": 1}]

_HOTKEYS = [
    {"record_toggle": "kp *"}, {"record_toggle": ""}, {"record_toggle": "ctrl+alt+r"},
    {"cancel": "kp -"}, {"record_toggle": "ctrl+shift+f12"}, {"record_toggle": "x1"},
    {"record_toggle": "alt+space"}, {"record_toggle": "f9"},
]

_SAVED = [
    {},
    {"recording_retention_mode": "keep_all"},
    {"recording_retention_mode": "custom", "max_saved_recordings": 50},
    {"recording_retention_mode": "size_limit", "max_saved_recordings_mb": 2048},
]
_EDITS = [
    ["custom", 20, 1024], ["custom", 10, 1024], ["custom", 50, 1024], ["keep_all", 20, 1024],
    ["size_limit", 20, 1024], ["size_limit", 20, 2048], ["size_limit", 20, 100],
    ["custom", 0, 1024], ["bogus", 20, 1024],
]

# M4.10: where the AI cleanup and text-near-cursor items of Home's where strip go.
_LOCAL_PROFILE = {"text_llm_profiles": [
    {"id": "custom_box", "name": "Box", "base_url": "http://127.0.0.1:5000/v1"}]}
_REMOTE_PROFILE = {"text_llm_profiles": [
    {"id": "custom_cloud", "name": "Cloud", "base_url": "https://llm.example.test/v1",
     "api_key_env": "CLOUD_KEY"}]}
_OPENROUTER = {"transcript_cleanup_provider": "openrouter",
               "transcript_cleanup_model": "openrouter/free"}
_OLLAMA = {"transcript_cleanup_provider": "ollama", "transcript_cleanup_model": "llama3.2"}
_WHERE = [
    {},
    {**_OPENROUTER, "transcript_cleanup_enabled": True},
    {**_OLLAMA, "transcript_cleanup_enabled": True},
    {**_OLLAMA, "transcript_cleanup_enabled": True, "transcript_cleanup_level": "high"},
    {**_OLLAMA, "transcript_cleanup_enabled": False, "transcript_cleanup_level": "high"},
    {**_OPENROUTER, "app_context_read_text": True},
    {**_OLLAMA, "app_context_read_text": True},
    {**_OPENROUTER, "app_context_read_text": True, "app_context_enabled": False},
    {"app_context_read_text": True, "app_context_enabled": True},
    {"transcript_cleanup_provider": "claude_code", "transcript_cleanup_model": "sonnet",
     "app_context_read_text": True},
    {"transcript_cleanup_provider": "bogus", "transcript_cleanup_model": "x"},
    {"transcript_cleanup_provider": "custom_box", "transcript_cleanup_model": "m",
     "transcript_cleanup_enabled": True, **_LOCAL_PROFILE},
    {"transcript_cleanup_provider": "custom_cloud", "transcript_cleanup_model": "m",
     "app_context_read_text": True, **_REMOTE_PROFILE},
    {"transcript_cleanup_provider": "custom_gone", "transcript_cleanup_model": "m",
     "app_context_read_text": True},
    {"transcript_cleanup_provider": "lm_studio", "transcript_cleanup_model": "m",
     "transcript_cleanup_enabled": True, "app_context_read_text": True},
    {"transcript_cleanup_provider": "ollama", "transcript_cleanup_model": "m",
     "ollama_base_url": "http://gpu-box:11434/v1", "app_context_read_text": True},
]

_CUSTOM_PROMPT = "Rewrite it as a friendly Slack message."
_CLEANUP_RAIL = [
    {},
    {"transcript_cleanup_enabled": True},
    {"transcript_cleanup_enabled": True, "transcript_cleanup_level": "high"},
    {"transcript_cleanup_enabled": True, "transcript_cleanup_level": "light",
     "transcript_cleanup_provider": "openai", "transcript_cleanup_model": "gpt-test"},
    {"transcript_cleanup_enabled": True, "transcript_cleanup_prompt": _CUSTOM_PROMPT,
     "transcript_cleanup_provider": "openai", "transcript_cleanup_model": "gpt-test"},
    {"transcript_cleanup_prompt": _CUSTOM_PROMPT, "transcript_cleanup_level": "light"},
    {"transcript_cleanup_enabled": True, "transcript_cleanup_prompt": "   "},
    {"transcript_cleanup_enabled": True, "transcript_cleanup_level": "bogus"},
    {"transcript_cleanup_enabled": True, "transcript_cleanup_provider": "ollama",
     "transcript_cleanup_model": "  llama3.2 "},
    {"transcript_cleanup_enabled": True, "transcript_cleanup_provider": "openai",
     "transcript_cleanup_model": "gpt-4o-mini"},
    {"transcript_cleanup_enabled": True, "transcript_cleanup_provider": "openai",
     "transcript_cleanup_model": "   "},
    {"transcript_cleanup_enabled": True, "transcript_cleanup_provider": "claude_code",
     "transcript_cleanup_model": ""},
    {"transcript_cleanup_enabled": False, "transcript_cleanup_level": "high",
     "transcript_cleanup_provider": "openai", "transcript_cleanup_model": "gpt-test"},
]

CASES = {
    "cleanup_rail_value": [[settings] for settings in _CLEANUP_RAIL],
    "SettingsDialog._refresh_home": [[settings] for settings in _WHERE],
    "_general_rail_value": [[settings] for settings in _GENERAL],
    "appearance_rail_value": [[settings] for settings in _APPEARANCE],
    "developer_rail_value": [[settings] for settings in _DEVELOPER],
    "SettingsDialog._hotkey_rail_value": [[hotkeys] for hotkeys in _HOTKEYS],
    "SettingsDialog._commit_retention": [
        [saved, mode, count, mb, answer]
        for saved in _SAVED for mode, count, mb in _EDITS for answer in (True, False)
    ],
    "SettingsDialog._confirm_recording_removal": [
        [None, None, [100], True],
        [20, None, [], True],
        [20, None, [1048576], True],
        [20, None, [1048576], False],
        [5, None, [1000, 2000, 3000], True],
        [None, 104857600, [1536, 512], False],
        [None, 1073741824, [5368709120], True],
        [1, None, [0], True],
    ],
    "SettingsDialog._apply_recordings_usage": [
        [0, 0], [1, 512], [2, 1536], [12, 1048576], [1, 5368709120],
    ],
}

PINS = {
    "note": "Appearance and Developer rail values are the expressions of _render_rail_values; "
            "the methods run on SimpleNamespace stand-ins (see the case file)",
}
