"""Curated inputs for ui_qt/dialogs/settings_dictionary.py (M4.4 text-prompts): what the
Dictionary page and Home say about the dictation engine and AI cleanup.

Settings travel as plain dicts keyed by the bare setting names. A backend travels as
{"is_remote", "available", "recognition_support", "name", "host_name"} or null (the stub the
page's backend provider returns). Every function returns {"endpoint": the selected API speech
endpoint as {"name", "local"} when the engine is "api", else null; "remote": the backend's
{"support", "name", "host_name"} when it is remote and available, else null; "engine":
_engine_support's (support, name, destination); "result": the function's answer}: the C++ side
is handed the endpoint and the remote backend already resolved.
"""

from types import SimpleNamespace

_TERM = {"id": "ksenia", "term": "Ksenia", "starred": False, "heard": ["Ksenya"]}
_ON = {"transcript_cleanup_enabled": True}

_SETTINGS = [
    {},
    _ON,
    {"dictionary_steer_recognition": False},
    {"dictation_dictionary": [_TERM]},
    {**_ON, "dictation_dictionary": [_TERM]},
    {"selected_model": "api"},
    {**_ON, "selected_model": "api"},
    {"selected_model": "api", "dictionary_steer_recognition": False},
    {"selected_model": "api", "dictation_dictionary": [_TERM]},
    {"selected_model": "api", "dictionary_steer_recognition": False,
     "dictation_dictionary": [_TERM]},
    {"selected_model": "api", "api_transcription_endpoint": "groq"},
    {"selected_model": "api", "api_transcription_endpoint": "speaches"},
    {"selected_model": "api", "api_transcription_endpoint": "custom"},
    {"selected_model": "api", "api_transcription_endpoint": "custom",
     "api_transcription_endpoints": {"custom": {"base_url": "https://asr.example.com/v1"}}},
    {"selected_model": "remote"},
    {**_ON, "selected_model": "remote"},
    {"selected_model": "remote", "dictation_dictionary": [_TERM]},
    {"selected_model": "parakeet", "transcript_cleanup_enabled": "yes"},
    {"selected_model": "api_whisper"},
    {"selected_model": "bogus"},
]

_BACKENDS = [
    None,
    {"is_remote": True, "available": True, "recognition_support": "model",
     "name": "Whisper on desk", "host_name": "desk"},
    {"is_remote": True, "available": True, "recognition_support": "after",
     "name": "Parakeet on desk", "host_name": "desk"},
    {"is_remote": True, "available": False, "recognition_support": "model",
     "name": "Whisper on desk", "host_name": "desk"},
    {"is_remote": False, "available": True, "recognition_support": "model",
     "name": "Local", "host_name": ""},
]


def _backend(spec):
    if spec is None:
        return None
    return SimpleNamespace(is_remote=spec["is_remote"], is_available=lambda: spec["available"],
                           recognition_support=spec["recognition_support"], name=spec["name"],
                           host_name=spec["host_name"])


def _inputs(module, settings, spec):
    from services import speech_endpoints
    from services.settings import SettingsKey

    engine = SettingsKey.SELECTED_MODEL.read(settings)
    endpoint = None
    if engine == "api":
        found = speech_endpoints.selected_endpoint(settings)
        endpoint = {"name": found.name, "local": bool(found.is_local)}
    remote = None
    if spec is not None and spec["is_remote"] is True and spec["available"]:
        remote = {"support": spec["recognition_support"], "name": spec["name"],
                  "host_name": spec["host_name"]}
    return {"endpoint": endpoint, "remote": remote,
            "engine": list(module._engine_support(settings, _backend(spec)))}


def _with_backend(name):
    def run(module, settings, spec):
        record = _inputs(module, settings, spec)
        record["result"] = getattr(module, name)(settings, _backend(spec))
        return record

    return run


def _home_detail(module, settings, spec):
    record = _inputs(module, settings, spec)
    original = module._backend_provider
    module.set_backend_provider(None if spec is None else (lambda: _backend(spec)))
    try:
        record["result"] = module.home_detail(settings)
    finally:
        module.set_backend_provider(original)
    return record


_PAIRS = [[settings, spec] for settings in _SETTINGS for spec in _BACKENDS]

CASES = {
    "cannot_steer": _PAIRS,
    "sounds_like_only": _PAIRS,
    "engine_line": _PAIRS,
    "home_detail": _PAIRS,
    "rail_value": [[settings] for settings in _SETTINGS],
}

WRAPPERS = {
    "cannot_steer": _with_backend("cannot_steer"),
    "sounds_like_only": _with_backend("sounds_like_only"),
    "engine_line": _with_backend("engine_line"),
    "home_detail": _home_detail,
}
