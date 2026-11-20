"""Curated inputs for ui_qt/dialogs/settings_api_keys.py (M2.2 appcore/api_keys_model.h).

The C++ model takes the credential owners as a list instead of reading the text-model profiles
and speech endpoints itself, so each vector records the owners Python derives from the settings
([env_name, owner, use]: the text profiles of list_profiles as "text", the speech endpoints of
speech_endpoints.credential_owners as "speech", then the fixed TypeSafe row as "decisions") next
to the function's answer. Credential state is injected: credential_source, resolve_credential,
environment_shadowed and credential_store are replaced for the call, and _render_status runs on
a stand-in section whose labels and icon record what they are given.
"""
from __future__ import annotations

from types import SimpleNamespace


def _owners(module, settings):
    from services import speech_endpoints

    owners = [[p.api_key_env, p.name, "text"] for p in module.list_profiles(settings)
              if p.api_key_env]
    owners += [[env, name, "speech"] for env, name in speech_endpoints.credential_owners(settings)]
    owners.append([module.TYPESAFE_CREDENTIAL_ENV, "TypeSafe", "decisions"])
    return owners


class _Patched:
    """Replaces module attributes for one call."""

    def __init__(self, module, **values):
        self.module, self.values, self.saved = module, values, {}

    def __enter__(self):
        for name, value in self.values.items():
            self.saved[name] = getattr(self.module, name)
            setattr(self.module, name, value)

    def __exit__(self, *_exc):
        for name, value in self.saved.items():
            setattr(self.module, name, value)


def _store(available, backend, reason):
    from services.credentials import CredentialStoreStatus

    status = CredentialStoreStatus(available, backend, reason)
    return lambda: SimpleNamespace(status=lambda: status)


def _key_entries(module, settings):
    return {"owners": _owners(module, settings),
            "entries": [list(entry) for entry in module.key_entries(settings)]}


def _rail_value(module, settings, sources):
    with _Patched(module, credential_source=lambda name: sources.get(name, "none")):
        return {"entries": [list(entry) for entry in module.key_entries(settings)],
                "value": module.rail_value(settings)}


def _store_copy(module, available, backend, reason):
    with _Patched(module, credential_store=_store(available, backend, reason)):
        return {"copy": module.store_copy(), "detail": module.store_detail()}


class _Label:
    def __init__(self):
        self.text, self.visible = "", True

    def setText(self, text):
        self.text = text

    def setVisible(self, visible):
        self.visible = bool(visible)


def _render_status(module, name, source, key, shadowed, backend):
    shown = {}
    stand_in = SimpleNamespace(
        selected_name=lambda: name,
        api_key_status=_Label(),
        api_key_status_icon=SimpleNamespace(setPixmap=lambda icon: shown.update(icon=icon)),
        _render_uses=lambda _name: None,
        update_controls=lambda: None,
    )
    patches = {
        "credential_store": _store(True, backend, ""),
        "credential_source": lambda _name: source,
        "resolve_credential": lambda _name: key,
        "environment_shadowed": lambda _name: shadowed,
        "design_icon": lambda icon: SimpleNamespace(pixmap=lambda *_size: icon),
        "set_style_property": lambda _widget, prop, value: shown.update({prop: value}),
    }
    with _Patched(module, **patches):
        module.ApiKeysSection._render_status(stand_in)
    return {"text": stand_in.api_key_status.text, "tone": shown["tone"], "icon": shown["icon"]}


def _render_uses(module, env_name, settings):
    caption = _Label()
    stand_in = SimpleNamespace(_load_settings=lambda: settings, api_key_uses_caption=caption)
    module.ApiKeysSection._render_uses(stand_in, env_name)
    return {"owners": _owners(module, settings), "text": caption.text,
            "visible": caption.visible}


WRAPPERS = {
    "key_entries": _key_entries,
    "rail_value": _rail_value,
    "store_copy": _store_copy,
    "ApiKeysSection._render_status": _render_status,
    "ApiKeysSection._render_uses": _render_uses,
}

_CUSTOM = {"api_transcription_servers": {
    "custom": {"base_url": "https://speech.example.test/v1", "api_key_env": "SPEECH_GATEWAY_KEY"},
}}
_SPEACHES = {"api_transcription_servers": {
    "speaches": {"base_url": "http://127.0.0.1:8000/v1", "api_key_env": "SPEACHES_KEY"},
    "custom": {"base_url": "http://127.0.0.1:9000/v1", "api_key_env": "SPEACHES_KEY"},
}}
_SHARED = {"api_transcription_servers": {
    "custom": {"base_url": "https://speech.example.test/v1", "api_key_env": "GROQ_API_KEY"},
}}
_KEYLESS = {"api_transcription_servers": {"custom": {"base_url": "http://127.0.0.1:9000/v1"}}}
_CUSTOM_PROFILE = {
    **_CUSTOM,
    "text_llm_profiles": [
        {"id": "custom_mine", "name": "My gateway", "base_url": "https://llm.example.test/v1",
         "api_key_env": "SPEECH_GATEWAY_KEY"},
    ],
}

# M4.10: text-model owners. A local server's saved key variable, custom endpoints (one with its
# own variable, one sharing OpenRouter's and one sharing a speech server's), an endpoint without
# a key, and the Team gateway of the text-owners reference.
_LOCAL_SERVER_KEYS = {"ollama_api_key_env": "OLLAMA_KEY", "lm_studio_api_key_env": "LMSTUDIO_KEY",
                      "vllm_base_url": "http://gpu-box:8000/v1"}
_TEAM_GATEWAY = {"text_llm_profiles": [
    {"id": "custom_team", "name": "Team gateway", "base_url": "https://llm.example.test/v1",
     "api_key_env": "TEAM_KEY"},
]}
_TEXT_SHARED = {
    **_SHARED,
    "text_llm_profiles": [
        {"id": "custom_proxy", "name": "My proxy", "base_url": "https://proxy.example.test/v1",
         "api_key_env": "OPENROUTER_API_KEY"},
        {"id": "custom_keyless", "name": "Bench", "base_url": "http://127.0.0.1:5000/v1"},
        {"id": "custom_dup", "name": "Groq mirror", "base_url": "https://groq.example.test/v1",
         "api_key_env": "GROQ_API_KEY"},
    ],
}
_TEXT_AND_SPEECH = {**_TEAM_GATEWAY, **_CUSTOM, **_LOCAL_SERVER_KEYS}

SETTINGS = [{}, _CUSTOM, _SPEACHES, _SHARED, _KEYLESS, _CUSTOM_PROFILE, _LOCAL_SERVER_KEYS,
            _TEAM_GATEWAY, _TEXT_SHARED, _TEXT_AND_SPEECH]

CASES = {
    "key_entries": [[settings] for settings in SETTINGS],
    "rail_value": [
        [{}, {}],
        [{}, {"OPENAI_API_KEY": "stored"}],
        [{}, {"OPENAI_API_KEY": "environment", "GROQ_API_KEY": "dotenv",
              "TYPESAFE_API_KEY": "stored"}],
        [_CUSTOM, {"SPEECH_GATEWAY_KEY": "environment"}],
        [_SPEACHES, {"SPEACHES_KEY": "none", "OPENAI_API_KEY": "stored"}],
        [_TEAM_GATEWAY, {"OPENROUTER_API_KEY": "stored", "TEAM_KEY": "environment"}],
        [_TEXT_SHARED, {"OPENROUTER_API_KEY": "dotenv", "GROQ_API_KEY": "stored"}],
        [_LOCAL_SERVER_KEYS, {"OLLAMA_KEY": "stored", "LMSTUDIO_KEY": "none"}],
    ],
    "store_copy": [
        [True, "Windows Credential Manager", ""],
        [False, "Windows Credential Manager", ""],
        [False, "Windows Credential Manager", "The store did not answer (error 1312)."],
        [True, "Windows Credential Manager", "ignored while available"],
    ],
    "ApiKeysSection._render_status": [
        [name, source, key, shadowed, "Windows Credential Manager"]
        for name in ("", "OPENAI_API_KEY", "SPEECH_GATEWAY_KEY")
        for source, key, shadowed in (
            ("stored", "sk-proj-1234567890abcd", False),
            ("stored", "sk-proj-1234567890abcd", True),
            ("stored", "short-key", False),
            ("environment", "gsk_abcdefghijklmnop", False),
            ("dotenv", "dotenv-secret-0001", False),
            ("dotenv", None, False),
            ("none", None, False),
        )
    ],
    "ApiKeysSection._render_uses": [
        [env, settings]
        for env in ("", "OPENAI_API_KEY", "OPENROUTER_API_KEY", "TYPESAFE_API_KEY",
                    "GROQ_API_KEY", "SPEECH_GATEWAY_KEY", "SPEACHES_KEY", "UNUSED_KEY",
                    "ANTHROPIC_API_KEY", "TEAM_KEY", "OLLAMA_KEY")
        for settings in ({}, _CUSTOM, _SPEACHES, _SHARED, _CUSTOM_PROFILE, _LOCAL_SERVER_KEYS,
                         _TEAM_GATEWAY, _TEXT_SHARED, _TEXT_AND_SPEECH)
    ],
}

PINS = {
    "note": "owners are [env_name, owner, use] in Python's order: list_profiles (text), "
            "speech_endpoints.credential_owners (speech), then TypeSafe (decisions); "
            "credential state is injected per call",
}
