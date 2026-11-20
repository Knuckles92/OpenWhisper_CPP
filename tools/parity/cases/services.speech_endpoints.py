"""Curated inputs for services/speech_endpoints.py (fv_speech speech/endpoints.h).

Every pure function over settings dicts. An endpoint is named as (endpoint id, settings) and
built with configured_endpoint; a result that is an endpoint is recorded as a dict of its fields
and properties. Functions whose OpenAI branch reads the date take an ISO day that pins
openai_retirement._today. The save_* functions run against a stand-in for settings_manager that
edits the given dict, and record the dict afterwards (or the exception). list_models is recorded
only for endpoints it answers without a request. normalize_base_url, validate_api_key_env and
is_loopback_url are services.text_llm's, which these functions call. The C++ replay is
tests/speech/test_endpoints.cpp.
"""
import copy
from datetime import date

BEFORE = "2026-11-14"
AFTER = "2027-02-26"

SERVERS = {"api_transcription_servers": {
    "speaches": {"base_url": "http://gpu-box:8000/", "api_key_env": "SPEACHES_API_KEY",
                 "model": " Systran/faster-whisper-large-v3 "},
    "vllm": {"base_url": "not a url", "api_key_env": "lower"},
    "localai": {"base_url": "http://127.0.0.1:8080/v1", "api_key_env": "LOCALAI_API_KEY",
                "model": "whisper-1"},
    "whisper_cpp": {"model": "   "},
    "custom": {"base_url": "https://speech.example.com/v1", "model": "tiny"},
    "groq": {"base_url": "http://127.0.0.1:1/v1", "model": "other"},
}}

SETTINGS = [
    {},
    {"api_transcription_model": "gpt-4o-transcribe"},
    {"api_transcription_model": "whisper-1"},
    {"api_transcription_model": "not-a-model"},
    {"api_transcription_model": 7},
    {"selected_model": "api_gpt4o_mini"},
    {"api_transcription_endpoint": "nonsense"},
    {"api_transcription_endpoint": 42},
    {"api_transcription_endpoint": None},
    {"api_transcription_endpoint": "custom"},
    {"api_transcription_endpoint": "groq"},
    {"api_transcription_endpoint": "speaches"},
    {"api_transcription_servers": "junk"},
    {"api_transcription_servers": {"speaches": "junk"}},
    {"api_transcription_servers": {"speaches": {"base_url": 5, "api_key_env": 7, "model": 9}}},
    {"api_transcription_servers": {"speaches": {"base_url": "http://user:pw@host/v1"}}},
    {"api_transcription_servers": {"speaches": {"base_url": "http://[::1]:8000"}}},
    {"api_transcription_servers": {"speaches": {"base_url": "http://[::1:8000/v1"}}},
    {"api_transcription_servers": {"speaches": {"base_url": "http://LOCALHOST:9/v1/"}}},
    {"api_transcription_servers": {"speaches": {"base_url": "", "api_key_env": " KEY_1 "}}},
    SERVERS,
    dict(SERVERS, api_transcription_endpoint="speaches"),
    dict(SERVERS, api_transcription_endpoint="custom"),
    dict(SERVERS, api_transcription_endpoint="whisper_cpp"),
    dict(SERVERS, api_transcription_endpoint="vllm"),
]

IDS = ["openai", "groq", "speaches", "localai", "vllm", "whisper_cpp", "custom", "nonsense"]

URLS = [
    "http://gpu-box:8000/", "http://gpu-box:8000", "https://api.example.com/v1/",
    "  http://127.0.0.1:8000/v1  ", "HTTP://Host:1/v1", "ftp://host/v1",
    "http://user:pw@host/v1", "http://user@host/v1", "http://:pw@host/v1", "http://@host/v1",
    "http://host/v1?key=1", "http://host/v1?", "http://host/v1#frag", "", "   ", "host:8000",
    "http://", "http:///v1", "http://[::1]:8000", "http://[::1:8000", "http://h/a//",
    "https://x.test/v1/audio/", "http://h\t/v1", "http://h/v1;params",
]

ENV_NAMES = ["", "  ", "OPENAI_API_KEY", " GROQ_API_KEY ", "lower", "_X", "1ABC", "A-B",
             "A B", "ÄBC", "ABCé"]


def _today(day):
    from services import openai_retirement

    pinned = date.fromisoformat(day)

    class _Pin:
        def __enter__(self):
            self.original = openai_retirement._today
            openai_retirement._today = lambda: pinned

        def __exit__(self, *_):
            openai_retirement._today = self.original

    return _Pin()


def _fields(endpoint):
    return {
        "id": endpoint.id, "name": endpoint.name, "kind": endpoint.connection.kind,
        "base_url": endpoint.base_url, "api_key_env": endpoint.connection.api_key_env,
        "builtin": endpoint.connection.builtin, "default_model": endpoint.default_model,
        "configurable": endpoint.configurable, "description": endpoint.description,
        "models_path": endpoint.models_path, "known_models": list(endpoint.known_models),
        "auto_language": endpoint.auto_language, "is_openai": endpoint.is_openai,
        "is_local": endpoint.is_local, "is_configured": endpoint.is_configured,
        "self_hosted": endpoint.self_hosted, "timeout_s": endpoint.timeout_s,
    }


class _Manager:
    """settings_manager for the save_* functions: edits one dict."""

    def __init__(self, settings):
        self.settings = settings

    def save_setting(self, key, value):
        self.settings[str(key)] = value

    def mutate_settings(self, apply):
        apply(self.settings)

    def load_all_settings(self):
        return self.settings


def _saving(module, settings, call):
    edited = copy.deepcopy(settings)
    original = module.settings_manager
    module.settings_manager = _Manager(edited)
    try:
        call()
    finally:
        module.settings_manager = original
    return {str(key): value for key, value in edited.items()}


def _configured_endpoint(module, endpoint_id, settings):
    return _fields(module.configured_endpoint(endpoint_id, settings))


def _selected_endpoint(module, settings):
    return _fields(module.selected_endpoint(settings))


def _endpoint_model(module, endpoint_id, settings, day):
    with _today(day):
        return module.endpoint_model(module.configured_endpoint(endpoint_id, settings), settings)


def _selected_model(module, settings, day):
    with _today(day):
        return module.selected_model(settings)


def _destination(module, endpoint_id, settings):
    return module.destination(module.configured_endpoint(endpoint_id, settings))


def _audio_destination_sentence(module, endpoint_id, settings):
    return module.audio_destination_sentence(module.configured_endpoint(endpoint_id, settings))


def _credential_owners(module, settings):
    return [list(owner) for owner in module.credential_owners(settings)]


def _save_selection(module, settings, endpoint_id):
    return _saving(module, settings, lambda: module.save_selection(endpoint_id))


def _save_model(module, settings, endpoint_id, model):
    return _saving(module, settings, lambda: module.save_model(endpoint_id, model))


def _save_server(module, settings, endpoint_id, base_url, api_key_env, model):
    return _saving(module, settings, lambda: module.save_server(
        endpoint_id, base_url=base_url, api_key_env=api_key_env, model=model))


def _list_models(module, endpoint_id, settings, day):
    with _today(day):
        return module.list_models(module.configured_endpoint(endpoint_id, settings))


def _normalize_base_url(module, url):
    from services.text_llm import normalize_base_url

    return normalize_base_url(url)


def _validate_api_key_env(module, name):
    from services.text_llm import validate_api_key_env

    return validate_api_key_env(name)


def _is_loopback_url(module, url):
    from services.text_llm import is_loopback_url

    return is_loopback_url(url)


WRAPPERS = {
    "configured_endpoint": _configured_endpoint,
    "selected_endpoint": _selected_endpoint,
    "endpoint_model": _endpoint_model,
    "selected_model": _selected_model,
    "destination": _destination,
    "audio_destination_sentence": _audio_destination_sentence,
    "credential_owners": _credential_owners,
    "save_selection": _save_selection,
    "save_model": _save_model,
    "save_server": _save_server,
    "list_models": _list_models,
    "normalize_base_url": _normalize_base_url,
    "validate_api_key_env": _validate_api_key_env,
    "is_loopback_url": _is_loopback_url,
}

CASES = {
    "configured_endpoint": [[i, s] for i in IDS for s in SETTINGS],
    "selected_endpoint": [[s] for s in SETTINGS],
    "endpoint_model": [[i, s, d] for i in IDS for s in SETTINGS for d in (BEFORE, AFTER)],
    "selected_model": [[s, d] for s in SETTINGS for d in (BEFORE, AFTER)],
    "valid_server_url": [[url] for url in URLS],
    "normalize_base_url": [[url] for url in URLS],
    "validate_api_key_env": [[name] for name in ENV_NAMES],
    "is_loopback_url": [[url] for url in [
        None, "", "http://127.0.0.1:8000/v1", "http://localhost/v1", "http://LOCALHOST/v1",
        "http://[::1]:9/v1", "http://127.0.0.2/v1", "https://api.groq.com/openai/v1",
        "http://user@localhost:8/v1", "localhost:8000"]],
    "destination": [[i, s] for i in IDS for s in SETTINGS[:1] + SETTINGS[15:]],
    "audio_destination_sentence": [[i, s] for i in IDS for s in SETTINGS[:1] + SETTINGS[20:21]],
    "credential_owners": [[s] for s in SETTINGS],
    "_is_speech_model": [[item] for item in [
        {}, {"id": "a"}, {"task": "automatic-speech-recognition"}, {"task": "text-to-speech"},
        {"task": None}, {"task": 5}, {"capabilities": ["transcript"]},
        {"capabilities": ["chat", "completion"]}, {"capabilities": []},
        {"capabilities": "transcript"}, {"capabilities": None},
        {"task": "automatic-speech-recognition", "capabilities": ["chat"]},
        {"task": "text-to-speech", "capabilities": ["transcript"]},
    ]],
    "save_selection": [[s, i] for s in ({}, SERVERS) for i in IDS + [""]],
    "save_model": [
        [{}, "openai", "whisper-1"], [{}, "openai", " gpt-transcribe "],
        [{}, "openai", "whisper-large-v3"], [{}, "openai", ""], [{}, "openai", "   "],
        [{}, "speaches", "Systran/faster-distil-whisper-small.en"],
        [SERVERS, "speaches", " tiny "], [SERVERS, "vllm", "m"], [{}, "groq", "x"],
        [{}, "nonsense", "x"], [{"api_transcription_servers": {"localai": "junk"}}, "localai", "m"],
        [{"api_transcription_servers": "junk"}, "custom", "m"],
    ],
    "save_server": [
        [{}, "speaches", "http://gpu-box:8000/", "SPEACHES_API_KEY", "Systran/faster-whisper-large-v3"],
        [SERVERS, "speaches", "http://gpu-box:8000/", "", "m"],
        [{}, "custom", "http://127.0.0.1:9000/v1", "", "tiny"],
        [{}, "groq", "http://127.0.0.1:1/v1", "", "m"],
        [{}, "openai", "http://127.0.0.1:1/v1", "", "m"],
        [{}, "nonsense", "http://127.0.0.1:1/v1", "", "m"],
        [{}, "custom", "ftp://host/v1", "", "m"],
        [{}, "custom", "http://user:pw@host/v1", "", "m"],
        [{}, "custom", "http://host/v1?key=1", "", "m"],
        [{}, "custom", "", "", "m"],
        [{}, "custom", "http://host/v1", "lower", "m"],
        [{}, "custom", "http://host/v1", "", ""],
        [{}, "custom", "ftp://host/v1", "lower", ""],
        [{}, "vllm", " http://h:1 ", " KEY ", " model "],
    ],
    "list_models": [[i, {}, d] for i in ("openai", "groq", "whisper_cpp") for d in (BEFORE, AFTER)],
}

PINS = {
    "today": "endpoint_model, selected_model and list_models pin openai_retirement._today to "
             "their last argument",
    "save": "save_* edit a copy of their first argument through a stand-in settings_manager",
}
