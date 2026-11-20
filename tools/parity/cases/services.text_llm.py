"""Curated inputs for services/text_llm.py (fv_llm llm/profiles.h, llm/credentials.h,
llm/catalog_client.h).

A profile argument is a built-in id (get_profile(id, {})) or TextLLMProfile fields. A profile in
a result is its fields and properties. A ValueError a function raises for the user is recorded as
{"raises": "ValueError", "message": str(exc)}, so its sentence is pinned too. upsert records the
settings it leaves and the profile it returns, with a new random id replaced by "<new>".
save_local_server runs against a stand-in settings_manager that edits a copy of its first
argument. credential_status and resolve_api_key see only the keys their last argument lists
(text_llm.resolve_credential is replaced), never the environment or a store.

verify_api_key and list_chat_models run the real OpenAI SDK client over an httpx MockTransport
that answers a named scenario (no network; the SDK's retry sleeps are skipped). They record the
result (or the exception's class and str) and every request: method, path, query, whether the
Bearer key was the one given, and the Accept, X-Title, x-opencode-session and (when it is not the
SDK's own) User-Agent headers. Self-hosted profiles are left to services.local_text_servers'
vectors. The C++ replay is tests/llm/test_profiles.cpp and test_catalog_client.cpp.
"""
import copy

SECRET = "sk-test-SECRET-0123456789"

URLS = [
    "http://127.0.0.1:1234/v1/", "http://127.0.0.1:1234/v1", "ftp://127.0.0.1/v1", "not-a-url",
    "", "   ", "  https://api.example.com/v1//  ", "HTTP://Host:1/V1/", "http://host/v1?key=1",
    "http://host/v1#frag", "http://host/v1?k=1#f", "http://", "http:///v1", "http://[::1]:8000/v1",
    "http://[::1:8000/v1", "http://user:pw@host/v1", "https://h", "http://h/a//", "http://h\t/v1",
    "localhost:1234", "http://localhost", "http://h/v1;p",
]

NAMES = ["  LM Studio  ", "   ", "", "x" * 80, "x" * 81, "é" * 80, "é" * 81, "Home\tLab", "　name　"]
ENV_NAMES = ["", "  ", "  LMSTUDIO_API_KEY ", "api-key", "1KEY", "_X", "A1_B", "ÄBC", "A B",
             "OPENAI_API_KEY", "lower"]
PROTOCOLS = ["chat", "responses", "anthropic", "", None, 5, ["chat"], "Chat"]

CUSTOM = {"id": "custom_abcd1234", "name": "Local", "base_url": "http://127.0.0.1:1234/v1",
          "api_key_env": ""}
CUSTOM_REMOTE = {"id": "custom_deadbeef", "name": "Work gateway",
                 "base_url": "https://llm.example.com/v1", "api_key_env": "WORK_LLM_KEY",
                 "protocol": "responses"}

SETTINGS = [
    {},
    {"vllm_base_url": "http://192.168.1.50:8000/v1/"},
    {"vllm_base_url": "not a url", "vllm_api_key_env": "lower"},
    {"vllm_base_url": 5, "ollama_base_url": "   ", "lm_studio_base_url": ["http://x/v1"]},
    {"llama_cpp_api_key_env": "LLAMA_SERVER_KEY", "localai_api_key_env": " SPACED ",
     "ollama_api_key_env": 7, "lm_studio_api_key_env": "bad name"},
    {"localai_base_url": "http://user:pw@h:1/v1", "ollama_base_url": "http://host-a:11434/v1"},
    {"text_llm_profiles": [CUSTOM, CUSTOM_REMOTE]},
    {"text_llm_profiles": [CUSTOM, dict(CUSTOM, name="Duplicate"), {"id": "openai", "name": "x",
                                                                   "base_url": "http://x/v1"}]},
    {"text_llm_profiles": "junk"},
    {"text_llm_profiles": [None, 5, "x", {}, {"id": 5}, {"id": "custom_x"},
                           {"id": "custom_y", "name": 7, "base_url": "http://y/v1"},
                           {"id": "custom_z", "name": True, "base_url": "http://z/v1",
                            "api_key_env": 7},
                           {"id": "custom_w", "name": [1, "a"], "base_url": "http://w/v1",
                            "protocol": None},
                           {"id": "custom_v", "name": {"a": 1.5}, "base_url": "http://v/v1",
                            "protocol": "responses"},
                           {"id": "custom_u", "name": "U", "base_url": "ftp://u/v1"}]},
    {"text_llm_profiles": [{"id": f"custom_{i:03d}", "name": f"E{i}",
                            "base_url": f"http://e{i}.test/v1"} for i in range(53)]},
]

RAW_PROFILES = [
    None, "x", [], {}, CUSTOM, CUSTOM_REMOTE, {"id": "custom_1", "name": "n", "base_url": "http://h"},
    {"id": "custom_1", "name": "", "base_url": "http://h"}, {"id": "openai", "name": "n",
                                                             "base_url": "http://h"},
    {"id": "custom_1", "name": "n", "base_url": "http://h", "api_key_env": "bad"},
    {"id": "custom_1", "name": "n", "base_url": "http://h", "protocol": "anthropic"},
    {"id": "custom_1", "name": 3.0, "base_url": "http://h", "protocol": "chat"},
    {"id": "custom_1", "name": False, "base_url": "http://h"},
    {"id": "custom_1", "name": "n", "base_url": "http://h/v1/", "api_key_env": "  KEY  "},
]

PROFILES = [
    "openai", "openrouter", "ollama", "lm_studio", "vllm", "llama_cpp", "localai", "groq",
    "opencode_go", "opencode_zen",
    {"id": "custom_abcd1234", "name": "LM Studio", "kind": "custom",
     "base_url": "http://127.0.0.1:1234/v1", "api_key_env": ""},
    {"id": "custom_deadbeef", "name": "Work gateway", "kind": "custom",
     "base_url": "https://llm.example.com/v1", "api_key_env": "WORK_LLM_KEY"},
    {"id": "custom_ipv6", "name": "IPv6", "kind": "custom", "base_url": "http://[::1]:8080/v1",
     "api_key_env": "", "protocol": "responses"},
    {"id": "custom_nohost", "name": "No host", "kind": "custom", "base_url": None,
     "api_key_env": "X_KEY"},
    {"id": "llama_cpp", "name": "llama.cpp", "kind": "llama_cpp",
     "base_url": "http://192.168.1.9:9931/v1", "api_key_env": "LLAMA_SERVER_KEY", "builtin": True},
    {"id": "opencode_go", "name": "Renamed", "kind": "opencode_go",
     "base_url": "https://opencode.ai/zen/go/v1", "api_key_env": "", "builtin": True},
]

SNAPSHOTS = [
    None, "", "   ", "{not json", "[1, 2]", "5", '{"profile_id": "openai", "kind": "openai"}',
    {}, {"profile_id": "openai", "name": "OpenAI", "kind": "openai", "base_url": None,
         "api_key_env": "OPENAI_API_KEY"},
    {"profile_id": "custom_deadbeef", "name": "Deleted Lab", "kind": "custom",
     "base_url": "http://10.0.0.8:8080/v1", "api_key_env": "LAB_KEY"},
    {"id": " custom_x ", "name": "  ", "base_url": "http://x/v1/"},
    {"profile_id": "", "id": "openrouter", "kind": "openrouter",
     "base_url": "https://openrouter.ai/api/v1"},
    {"profile_id": 5, "base_url": "http://x/v1"},
    {"profile_id": "p", "kind": "weird", "base_url": "http://x/v1", "api_key_env": "bad key"},
    {"profile_id": "p", "kind": "custom"},
    {"profile_id": "p", "kind": "custom", "base_url": "ftp://x"},
    {"profile_id": "p", "kind": "openai", "base_url": "   "},
    {"profile_id": "p", "kind": "custom", "base_url": "http://x", "protocol": "bogus"},
    {"profile_id": "p", "kind": "custom", "base_url": "http://x", "protocol": "responses"},
    {"profile_id": "p", "kind": "custom", "base_url": "http://x", "model_metadata": "junk"},
    {"profile_id": "p", "kind": "custom", "base_url": "http://x", "model_metadata": {"model": 5}},
    {"profile_id": "p", "kind": "custom", "base_url": "http://x", "model_metadata": {}},
    {"profile_id": "p", "kind": "custom", "base_url": "http://x",
     "model_metadata": {"model": "m", "protocol": "chat", "context_window": 1}},
    {"profile_id": "p", "kind": "custom", "base_url": "http://x",
     "model_metadata": {"model": "m", "protocol": "nope"}},
    {"profile_id": "ollama", "name": "Ollama", "kind": "ollama", "base_url": "http://host-a:11434/v1",
     "api_key_env": "", "model_metadata": {"model": "llama3.2", "protocol": "chat",
                                           "thinking_levels": {"off": "none"}, "pi_compat": None}},
    '{"profile_id": "opencode_zen", "kind": "opencode_zen", "base_url": "https://opencode.ai/zen/v1", "name": "Zen"}',
]


def _profile(spec):
    from services.text_llm import TextLLMProfile, get_profile

    if isinstance(spec, str):
        return get_profile(spec, {})
    return TextLLMProfile(**spec)


def _fields(profile):
    if profile is None:
        return None
    return {
        "id": profile.id, "name": profile.name, "kind": profile.kind,
        "base_url": profile.base_url, "api_key_env": profile.api_key_env,
        "builtin": profile.builtin, "model_metadata": profile.model_metadata,
        "protocol": profile.protocol, "requires_api_key": profile.requires_api_key,
        "is_local": profile.is_local, "self_hosted": profile.self_hosted,
        "refuses_unsupported_tools": profile.refuses_unsupported_tools,
    }


def _value_error(call):
    try:
        return call()
    except ValueError as exc:
        return {"raises": "ValueError", "message": str(exc)}


def _settings_out(settings, new_id=None):
    out = copy.deepcopy(settings)
    if new_id:
        for entry in out.get("text_llm_profiles", []):
            if entry.get("id") == new_id:
                entry["id"] = "<new>"
    return {str(k): v for k, v in out.items()}


def _upsert(module, settings, kwargs):
    edited = copy.deepcopy(settings)
    try:
        profile = module.upsert_custom_profile(edited, **kwargs)
    except ValueError as exc:
        return {"raises": "ValueError", "message": str(exc)}
    new_id = profile.id if profile.id != kwargs.get("profile_id") else None
    fields = _fields(profile)
    if new_id:
        fields["id"] = "<new>"
    return {"profile": fields, "settings": _settings_out(edited, new_id),
            "new_id_shape": bool(new_id) and new_id.startswith("custom_") and len(new_id) == 15}


def _remove(module, settings, profile_id):
    edited = copy.deepcopy(settings)
    try:
        removed = module.remove_custom_profile(edited, profile_id)
    except ValueError as exc:
        return {"raises": "ValueError", "message": str(exc)}
    return {"removed": removed, "settings": _settings_out(edited)}


class _Manager:
    def __init__(self, settings):
        self.settings = settings

    def update_settings(self, patch):
        self.settings.update(patch)


def _save_local_server(module, settings, profile_id, url, api_key_env):
    from services import settings as settings_module

    edited = copy.deepcopy(settings)
    original = settings_module.settings_manager
    settings_module.settings_manager = _Manager(edited)
    try:
        module.save_local_server(profile_id, url, api_key_env)
    except ValueError as exc:
        return {"raises": "ValueError", "message": str(exc)}
    finally:
        settings_module.settings_manager = original
    return _settings_out(edited)


def _with_keys(module, keys, call):
    original = module.resolve_credential
    module.resolve_credential = lambda name: keys.get(name)
    try:
        return call()
    finally:
        module.resolve_credential = original


def _snapshot(module, raw):
    try:
        snapshot = module.snapshot_from_mapping(raw)
    except (ValueError, TypeError, OverflowError) as exc:
        return {"raises": type(exc).__name__, "message": str(exc)}
    if snapshot is None:
        return None
    return {"dict": snapshot.to_dict(), "profile": _fields(snapshot.to_profile())}


# Scenario -> what the fake provider answers every request with.
MODEL_IDS = ["z-model", "gpt-4o-mini", "whisper-1", "o4-mini", "a-model", "gpt-4o-audio-preview",
             "dall-e-3", "llama-guard-4", "distil-whisper", "llama-3.3-70b", "Compound-beta",
             "chatgpt-4o-latest", "text-embedding-3-small"]
SCENARIOS = {
    "ok": (200, {"object": "list", "data": [{"id": m, "object": "model"} for m in MODEL_IDS]}),
    "ok_odd": (200, {"data": [{"id": "b"}, {"x": 1}, "junk", {"id": 5}, {"id": "a"}]}),
    "ok_key": (200, {"data": {"label": "sk-...", "limit": None}}),
    "401": (401, {"error": {"message": f"Incorrect API key provided: {SECRET}",
                            "type": "invalid_request_error", "code": "invalid_api_key"}}),
    "403": (403, {"error": {"message": "Project does not have access", "code": None}}),
    "404": (404, {"error": "not found"}),
    "429": (429, {"error": {"message": "Rate limit", "type": "requests"}}),
    "500": (500, {"error": {"message": "boom"}}),
    "502_text": (502, "upstream down\n"),
    "401_empty": (401, ""),
    "200_text": (200, "plain"),
    "connect": "connect",
    "timeout": "timeout",
}


def _run_sdk(module, scenario, call):
    import time

    import httpx
    import openai

    seen = []

    def handler(request):
        user_agent = request.headers.get("user-agent", "")
        seen.append({
            "method": request.method, "path": request.url.path,
            "query": request.url.query.decode("ascii"),
            "bearer": request.headers.get("authorization") == f"Bearer {SECRET}",
            "accept": request.headers.get("accept"),
            "x-title": request.headers.get("x-title"),
            "x-opencode-session": request.headers.get("x-opencode-session"),
            "user-agent": None if user_agent.startswith("OpenAI/") else user_agent,
        })
        answer = SCENARIOS[scenario]
        if answer == "connect":
            raise httpx.ConnectError("[WinError 10061] refused", request=request)
        if answer == "timeout":
            raise httpx.ReadTimeout("timed out", request=request)
        status, body = answer
        if isinstance(body, str):
            return httpx.Response(status, text=body)
        return httpx.Response(status, json=body)

    def factory(**kwargs):
        return openai.OpenAI(**kwargs, http_client=httpx.Client(
            transport=httpx.MockTransport(handler)))

    original_class, original_sleep = module.OpenAI, time.sleep
    module.OpenAI = factory
    time.sleep = lambda _seconds: None
    try:
        result = call()
    except Exception as exc:  # noqa: BLE001 - the vector records what the caller sees
        result = {"raises": type(exc).__name__, "message": str(exc)}
    finally:
        module.OpenAI, time.sleep = original_class, original_sleep
    return {"result": result, "requests": seen}


def _verify(module, profile, key, scenario):
    return _run_sdk(module, scenario, lambda: list(module.verify_api_key(_profile(profile), key)))


def _list(module, profile, key, sort, scenario):
    return _run_sdk(module, scenario, lambda: module.list_chat_models(
        _profile(profile), api_key=key, sort=sort))


WRAPPERS = {
    "normalize_base_url": lambda m, url: _value_error(lambda: m.normalize_base_url(url)),
    "validate_profile_name": lambda m, name: _value_error(lambda: m.validate_profile_name(name)),
    "validate_api_key_env": lambda m, name: _value_error(lambda: m.validate_api_key_env(name)),
    "validate_text_protocol": lambda m, p: _value_error(lambda: m.validate_text_protocol(p)),
    "normalize_local_server_url": lambda m, url: _value_error(
        lambda: m.normalize_local_server_url(url)),
    "builtin_profiles": lambda m: [_fields(p) for p in m.builtin_profiles()],
    "list_profiles": lambda m, settings: [_fields(p) for p in m.list_profiles(settings)],
    "parse_custom_profile": lambda m, raw: _fields(m.parse_custom_profile(raw)),
    "get_profile": lambda m, profile_id, settings: _fields(m.get_profile(profile_id, settings)),
    "known_profile_ids": lambda m, settings: list(m.known_profile_ids(settings)),
    "upsert_custom_profile": _upsert,
    "remove_custom_profile": _remove,
    "save_local_server": _save_local_server,
    "snapshot_from_mapping": _snapshot,
    "snapshot_from_meeting": lambda m, meeting, settings, fallback: m.snapshot_from_meeting(
        meeting, settings, fallback_provider=fallback).to_dict(),
    "profile_from_agent_config": lambda m, provider, endpoint, settings: _fields(
        m.profile_from_agent_config(provider, endpoint, settings)),
    "profile_fields": lambda m, profile: _fields(_profile(profile)),
    "credential_label": lambda m, profile: m.credential_label(_profile(profile)),
    "credential_status": lambda m, profile, keys: _with_keys(
        m, keys, lambda: list(m.credential_status(_profile(profile)))),
    "resolve_api_key": lambda m, profile, keys: _with_keys(
        m, keys, lambda: m.resolve_api_key(_profile(profile))),
    "consent_destination": lambda m, profile: m.consent_destination(_profile(profile)),
    "destination_is_remote": lambda m, profile: m.destination_is_remote(_profile(profile)),
    "profile_display_name": lambda m, profile_id, settings: m.profile_display_name(
        profile_id, settings),
    "default_model_for_profile": lambda m, profile: m.default_model_for_profile(_profile(profile)),
    "chat_request_options": lambda m, profile, reasoning: m.chat_request_options(
        _profile(profile), reasoning),
    "provider_headers": lambda m, profile, session: m.provider_headers(_profile(profile), session),
    "verify_api_key": _verify,
    "list_chat_models": _list,
}

KEYS = [{}, {"OPENROUTER_API_KEY": "sk-or"}, {"OPENAI_API_KEY": ""},
        {"WORK_LLM_KEY": "k", "LLAMA_SERVER_KEY": "k", "X_KEY": "k"}]

HOSTED = ["openai", "openrouter", "groq", "opencode_go", "opencode_zen",
          {"id": "custom_deadbeef", "name": "Work gateway", "kind": "custom",
           "base_url": "https://llm.example.com/v1/", "api_key_env": "WORK_LLM_KEY"},
          {"id": "custom_nohost", "name": "No host", "kind": "custom", "base_url": "http:///v1",
           "api_key_env": "X_KEY"}]

CASES = {
    "normalize_base_url": [[u] for u in URLS],
    "validate_profile_name": [[n] for n in NAMES],
    "validate_api_key_env": [[n] for n in ENV_NAMES],
    "validate_text_protocol": [[p] for p in PROTOCOLS],
    "normalize_local_server_url": [[u] for u in URLS],
    "builtin_profiles": [[]],
    "list_profiles": [[s] for s in SETTINGS],
    "parse_custom_profile": [[r] for r in RAW_PROFILES],
    "get_profile": [[i, s] for i in ["openai", " vllm ", "lm_studio", "ollama", "localai",
                                     "llama_cpp", "custom_abcd1234", "custom_missing", "", None, 5]
                    for s in SETTINGS[:7]],
    "known_profile_ids": [[s] for s in SETTINGS],
    "upsert_custom_profile": [
        [{}, {"name": "LM Studio", "base_url": "http://127.0.0.1:1234/v1/", "api_key_env": ""}],
        [{}, {"name": "Gateway", "base_url": "https://gateway.test/v1", "protocol": "responses"}],
        [{}, {"name": "Invalid", "base_url": "https://g/v1", "protocol": "unknown"}],
        [{}, {"name": "  ", "base_url": "ftp://x", "api_key_env": "bad"}],
        [{}, {"name": "N", "base_url": "ftp://x", "api_key_env": "bad"}],
        [{}, {"name": "N", "base_url": "http://x", "api_key_env": "bad"}],
        [SETTINGS[6], {"name": "Home Lab", "base_url": "http://127.0.0.1:8000/v1",
                       "api_key_env": "HOME_LLM_KEY", "profile_id": "custom_abcd1234"}],
        [SETTINGS[6], {"name": "Renamed", "base_url": "https://llm.example.com/v1",
                       "profile_id": "custom_deadbeef"}],
        [SETTINGS[6], {"name": "Renamed", "base_url": "https://llm.example.com/v1",
                       "profile_id": "custom_deadbeef", "protocol": "chat"}],
        [SETTINGS[6], {"name": "X", "base_url": "http://x", "profile_id": "openai"}],
        [SETTINGS[6], {"name": "X", "base_url": "http://x", "profile_id": "custom_missing"}],
        [SETTINGS[6], {"name": "X", "base_url": "http://x", "profile_id": ""}],
        [SETTINGS[10], {"name": "One more", "base_url": "http://x"}],
        [SETTINGS[9], {"name": "Clean", "base_url": "http://c/v1"}],
    ],
    "remove_custom_profile": [
        [SETTINGS[6], "custom_abcd1234"], [SETTINGS[6], "custom_missing"], [{}, "openai"],
        [SETTINGS[9], "custom_y"], [SETTINGS[7], "custom_abcd1234"], [{}, "custom_x"],
    ],
    "save_local_server": [
        [{}, "lm_studio", "http://localhost:1234/", ""],
        [{}, "lm_studio", "http://user:secret@localhost", ""],
        [{}, "lm_studio", "http://localhost/?key=secret", ""],
        [{}, "lm_studio", "http://localhost:1234/v1", "not a name"],
        [{}, "openai", "http://localhost:1234/v1", ""],
        [{"keep": 1}, "vllm", "  http://gpu:8000  ", "  VLLM_KEY "],
        [{}, "ollama", "http://h:11434/api#x", ""],
        [{}, "localai", "ftp://h", ""],
        [{}, "llama_cpp", "http://user@h", ""],
    ],
    "snapshot_from_mapping": [[s] for s in SNAPSHOTS],
    "snapshot_from_meeting": [
        [{"agent_provider": "openai"}, {}, "openrouter"],
        [{"agent_provider": "custom_deleted"},
         {"meeting_llm_provider": "ollama", "meeting_llm_model": "llama3.2"}, "custom_deleted"],
        [{"agent_provider": "openai", "agent_endpoint_json": SNAPSHOTS[9]}, {}, "openrouter"],
        [{}, {}, "openrouter"], [None, {}, "openrouter"], [{"agent_provider": 5}, {}, "groq"],
        [{"agent_provider": ""}, {}, "groq"],
        [{"agent_provider": "custom_abcd1234"}, SETTINGS[6], "openrouter"],
        [{"agent_endpoint_json": "{bad"}, {}, "vllm"],
    ],
    "profile_from_agent_config": [
        ["openai", SNAPSHOTS[9], {}],
        ["custom_deleted", None, {"meeting_llm_provider": "ollama", "meeting_llm_model": "m"}],
        ["custom_abcd1234", None, SETTINGS[6]], ["openai", "{bad", {}], ["vllm", None, SETTINGS[1]],
    ],
    "profile_fields": [[p] for p in PROFILES],
    "credential_label": [[p] for p in PROFILES],
    "credential_status": [[p, k] for p in PROFILES for k in KEYS],
    "resolve_api_key": [[p, k] for p in PROFILES for k in KEYS],
    "consent_destination": [[p] for p in PROFILES],
    "destination_is_remote": [[p] for p in PROFILES],
    "profile_display_name": [[i, s] for i in ["openai", "claude_code", "codex", "opencode_cli",
                                              "missing_profile", "  spaced  ", "", None,
                                              "custom_abcd1234", "lm_studio"]
                             for s in ({}, SETTINGS[6])],
    "default_model_for_profile": [[p] for p in PROFILES],
    "filter_openai_chat_models": [
        [["gpt-4o-mini", "whisper-1", "gpt-4o-audio-preview", "o4-mini", "dall-e-3"]],
        [["gpt-4o-mini", "whisper-1", "gpt-4o-audio-preview", "text-embedding-3-small", "o4-mini",
          "dall-e-3", "gpt-4o-mini-tts", "gpt-4o-realtime-preview"]],
        [["GPT-4O", "O1-preview", "gpt-image-1", "gpt-3.5-turbo-instruct", "chatgpt-4o-latest",
          "o3-deep-research", "omni-moderation-latest", "gpt-4o-search-preview", "o4", "babbage-002",
          "gpt-5", "codex-mini-latest", "ft:gpt-4o:org", "İgpt"]],
        [[]],
    ],
    "chat_request_options": [[p, r] for p in ["openai", "openrouter", "groq", "ollama",
                                              PROFILES[10]]
                             for r in ["off", "low", "high", "xhigh"]],
    "provider_headers": [[p, s] for p in ["openai", "openrouter", "groq", "opencode_go",
                                          "opencode_zen", PROFILES[15]]
                         for s in ("", "meeting-1")],
    "verify_api_key": (
        [[p, SECRET, s] for p in HOSTED[:5] for s in ["ok", "401", "403", "500"]]
        + [["openai", SECRET, s] for s in ["ok_key", "404", "429", "502_text", "401_empty",
                                           "200_text", "connect", "timeout"]]
        + [[HOSTED[5], SECRET, "ok"], [HOSTED[5], SECRET, "401"], [HOSTED[6], SECRET, "ok"],
           [HOSTED[6], SECRET, "connect"], ["openrouter", SECRET, "connect"]]
    ),
    "list_chat_models": (
        [[p, SECRET, None, "ok"] for p in HOSTED[:6]]
        + [["openrouter", SECRET, s, "ok"] for s in ["top-weekly", "alphabetical", "", "most-popular",
                                                    "pricing low&high"]]
        + [["openai", SECRET, "top-weekly", "ok"], [HOSTED[5], SECRET, "top-weekly", "ok"]]
        + [["openai", SECRET, None, s] for s in ["ok_odd", "401", "403", "404", "429", "500",
                                                 "502_text", "401_empty", "200_text", "connect",
                                                 "timeout"]]
    ),
}

PINS = {
    "secret": "the key verify_api_key and list_chat_models send; the results never contain it",
    "upsert": "a new random custom id is written as <new>",
}
