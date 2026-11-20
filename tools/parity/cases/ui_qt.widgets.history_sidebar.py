"""The display helpers of ui_qt/widgets/history_sidebar.py (M1.15).

Entries are given as dicts of TranscriptionHistory columns (missing ones are None), plus the
transient stored_on, also_on and remote_audio when a case names them. The cleanup helpers get
settings={} so no settings file is read; a custom profile's name then falls back to its id,
which is what OpenWhisper_CPP shows until M4 ports the profile store.

    _format_model_name(model)
    _format_cleanup_info(entry), _cleanup_tooltip(entry), _entry_was_cleaned(entry),
    _kind_label(entry), _location_chip(entry) -> [text, tooltip]
    _failed_recording_hint(has_audio)

OpenWhisper_CPP's own mapping of "parakeet (<label> | <device>)" model strings is not OpenWhisper's
and is tested in tests/ui_history/test_golden.cpp, so no case here has a "parakeet" base.
"""

from types import SimpleNamespace

_COLUMNS = (
    "id", "text", "raw_text", "timestamp", "model", "audio_file", "transcription_time",
    "audio_duration", "file_size", "cleanup_provider", "cleanup_model", "source_name", "title",
    "origin_device_id", "origin_device_name", "app_id", "app_name", "app_category",
    "cleanup_level", "entry_kind", "cleaned_text", "language",
)
_TRANSIENT = ("stored_on", "also_on", "remote_audio")


def _entry(fields):
    values = {name: fields.get(name) for name in _COLUMNS}
    # TranscriptionHistory.text is NOT NULL.
    if values["text"] is None:
        values["text"] = ""
    entry = SimpleNamespace(**values)
    for name in _TRANSIENT:
        if name in fields:
            setattr(entry, name, fields[name])
    return entry


_MODELS = [
    "local_whisper",
    "local_whisper (turbo | cuda (float16))",
    "local_whisper (base.en | cpu (int8))",
    "local_whisper ()",
    "api",
    "api_whisper",
    "api_gpt4o",
    "api_gpt4o_mini (whisper-1)",
    "  api_gpt4o  (x | y | z)",
    "base",
    "base.en",
    "remote (devbox | Parakeet TDT 0.6B v3)",
    "Parakeet TDT 0.6B v3",
    "",
    "(only detail)",
    "name (detail without close",
    "name (a) (b)",
    "café (ünïcode | x)",
]

_CLEANUP = [
    {},
    {"raw_text": "hi"},
    {"raw_text": "hi", "cleanup_level": "medium"},
    {"raw_text": "hi", "cleanup_level": "MEDIUM", "cleanup_model": "gpt-4o-mini"},
    {"raw_text": "hi", "cleanup_level": "light", "cleanup_model": "gpt-4o-mini",
     "cleanup_provider": "openai"},
    {"raw_text": "hi", "cleanup_level": "unknown", "cleanup_model": "gpt-4o-mini",
     "cleanup_provider": "openai"},
    {"raw_text": "hi", "cleanup_level": "", "cleanup_model": "gpt-4o-mini",
     "cleanup_provider": "openai"},
    {"raw_text": "hi", "cleanup_model": "gpt-4o-mini", "cleanup_provider": "openai"},
    {"raw_text": "hi", "cleanup_model": "gpt-4o-mini", "cleanup_provider": "openrouter"},
    {"raw_text": "hi", "cleanup_model": "llama3", "cleanup_provider": "ollama"},
    {"raw_text": "hi", "cleanup_model": "llama3", "cleanup_provider": "lm_studio"},
    {"raw_text": "hi", "cleanup_model": "m", "cleanup_provider": "groq"},
    {"raw_text": "hi", "cleanup_model": "m", "cleanup_provider": "opencode_zen"},
    {"raw_text": "hi", "cleanup_model": "m", "cleanup_provider": "claude_code"},
    {"raw_text": "hi", "cleanup_model": "m", "cleanup_provider": "codex"},
    {"raw_text": "hi", "cleanup_model": "m", "cleanup_provider": " openai "},
    {"raw_text": "hi", "cleanup_model": "m", "cleanup_provider": "custom_abc"},
    {"raw_text": "hi", "cleanup_model": "m", "cleanup_provider": ""},
    {"raw_text": "hi", "cleanup_model": "", "cleanup_provider": "openai"},
    {"cleanup_model": "gpt-4o-mini", "cleanup_provider": "openai", "cleanup_level": "high"},
    {"text": "hi", "raw_text": "hi", "cleaned_text": "Hi.", "cleanup_model": "m",
     "cleanup_level": "custom"},
    {"text": "Hi.", "raw_text": "hi", "cleaned_text": "Hi.", "cleanup_model": "m",
     "cleanup_level": "profile"},
    {"text": "hi", "raw_text": "hi"},
    {"text": "hi", "raw_text": ""},
]

_KINDS = [
    {},
    {"entry_kind": "failed_recording"},
    {"entry_kind": "command", "source_name": "Quick Record"},
    {"entry_kind": "walkthrough", "source_name": "x"},
    {"entry_kind": "transform", "source_name": "Transform · Polish"},
    {"entry_kind": "transform", "source_name": "Polish"},
    {"entry_kind": "transform", "source_name": "  Polish  "},
    {"entry_kind": "transform", "source_name": "Transform · "},
    {"entry_kind": "transform"},
    {"entry_kind": "action", "source_name": "Action · Send to Slack"},
    {"entry_kind": "action", "source_name": "Send"},
    {"entry_kind": "action", "source_name": ""},
    {"entry_kind": "dictation", "source_name": "Quick Record"},
    {"entry_kind": "file", "source_name": "talk.mp3"},
    {"entry_kind": "Command"},
    {"entry_kind": " command"},
]

_LOCATIONS = [
    {},
    {"stored_on": "devbox"},
    {"stored_on": "", "also_on": "devbox"},
    {"also_on": "devbox", "origin_device_name": "Laptop"},
    {"origin_device_name": "Laptop"},
    {"stored_on": "devbox", "origin_device_name": "Laptop"},
    {"origin_device_name": ""},
]

CASES = {
    "_format_model_name": [[model] for model in _MODELS],
    "_format_cleanup_info": [[fields] for fields in _CLEANUP],
    "_cleanup_tooltip": [[fields] for fields in _CLEANUP],
    "_entry_was_cleaned": [[fields] for fields in _CLEANUP],
    "_kind_label": [[fields] for fields in _KINDS],
    "_location_chip": [[fields] for fields in _LOCATIONS],
    "_failed_recording_hint": [[True], [False]],
}

WRAPPERS = {
    "_format_cleanup_info": lambda module, fields: module._format_cleanup_info(
        _entry(fields), settings={}),
    "_cleanup_tooltip": lambda module, fields: module._cleanup_tooltip(
        _entry(fields), settings={}),
    "_entry_was_cleaned": lambda module, fields: module._entry_was_cleaned(_entry(fields)),
    "_kind_label": lambda module, fields: module._kind_label(_entry(fields)),
    "_location_chip": lambda module, fields: list(module._location_chip(_entry(fields))),
}
