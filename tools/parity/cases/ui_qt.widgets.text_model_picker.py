"""Curated inputs for ui_qt/widgets/text_model_picker.py (fv_ui_widgets TextModelPicker, M4.6).

The picker's copy and its few decision tables are read from the module itself, so the strings the
C++ widget carries are checked against Python's. The methods that decide wording run on a
stand-in self (no QApplication is needed) whose combo, labels and icon record what they are
given; design_icon, credential_status and set_style_property are replaced for the call.

    _DESCRIPTIONS      args: []                       result: {profile id: line}
    _SORT_OPTIONS      args: []                       result: [[label, saved value], ...]
    _description       args: [profile, running]       result: the line shown for the endpoint
    _reload_provider_combo
                       args: [profiles, running]      result: one [icon file, label, data,
                                                        tooltip] per combo row
    _update_credential_status
                       args: [profiles, provider, status, running]
                                                      result: {text, tooltip, available, icon}
    _update_active_badge
                       args: [profiles, provider, active provider, active model, selected]
                                                      result: {badge, tooltip}

A profile is {id, name, kind, base_url, api_key_env, builtin}; `running` is [[base_url, server],
...] in the order server detection found them; `status` is [available, text], what
credential_status answers for the shown endpoint.
"""
from __future__ import annotations

from types import SimpleNamespace

BUILTIN = {
    "openai": ("OpenAI", None, "OPENAI_API_KEY"),
    "openrouter": ("OpenRouter", "https://openrouter.ai/api/v1", "OPENROUTER_API_KEY"),
    "ollama": ("Ollama", "http://localhost:11434/v1", ""),
    "lm_studio": ("LM Studio", "http://127.0.0.1:1234/v1", ""),
    "vllm": ("vLLM", "http://127.0.0.1:8000/v1", ""),
    "llama_cpp": ("llama.cpp", "http://127.0.0.1:9931/v1", ""),
    "localai": ("LocalAI", "http://127.0.0.1:8080/v1", ""),
    "groq": ("Groq", "https://api.groq.com/openai/v1", "GROQ_API_KEY"),
    "opencode_go": ("OpenCode Go", "https://opencode.ai/zen/go/v1", "OPENCODE_GO_API_KEY"),
    "opencode_zen": ("OpenCode Zen", "https://opencode.ai/zen/v1", "OPENCODE_ZEN_API_KEY"),
}


def _builtin(profile_id, base_url=None):
    name, default_url, env = BUILTIN[profile_id]
    return {"id": profile_id, "name": name, "kind": profile_id,
            "base_url": default_url if base_url is None else base_url,
            "api_key_env": env, "builtin": True}


def _custom(profile_id, name, base_url, env=""):
    return {"id": profile_id, "name": name, "kind": "custom", "base_url": base_url,
            "api_key_env": env, "builtin": False}


ALL = [_builtin(i) for i in BUILTIN]
LOCALS = [_builtin(i) for i in ("ollama", "lm_studio", "vllm", "llama_cpp", "localai")]
CUSTOM = _custom("custom_a1b2c3d4", "Gateway", "https://gateway.test/v1", "GATEWAY_KEY")
LOCAL_CUSTOM = _custom("custom_0000beef", "Home LiteLLM", "http://127.0.0.1:4000/v1")
PROFILES = ALL + [CUSTOM, LOCAL_CUSTOM]


def _profile(fields):
    from services.text_llm import TextLLMProfile

    return TextLLMProfile(id=fields["id"], name=fields["name"], kind=fields["kind"],
                          base_url=fields["base_url"], api_key_env=fields["api_key_env"],
                          builtin=fields["builtin"])


class _Combo:
    """Records what the picker puts in a combo box."""

    def __init__(self):
        self.rows = []

    def blockSignals(self, _blocked):
        return False

    def clear(self):
        self.rows = []

    def count(self):
        return len(self.rows)

    def addItem(self, icon, label, data):
        self.rows.append([icon, label, data, None])

    def setItemData(self, index, value, _role):
        self.rows[index][3] = value


class _Label:
    def __init__(self):
        self.text = self.tooltip = self.pixmap = None

    def setText(self, text):
        self.text = text

    def setToolTip(self, text):
        self.tooltip = text

    def setPixmap(self, pixmap):
        self.pixmap = pixmap


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


def _icon(name):
    return SimpleNamespace(pixmap=lambda _width, _height: name)


def _stub(module, profiles, running, provider="openai"):
    cls = module.TextModelPicker
    stub = SimpleNamespace(
        _profiles=[_profile(p) for p in profiles], _running_servers=dict(running),
        _DESCRIPTIONS=cls._DESCRIPTIONS, _CUSTOM_OPTION=cls._CUSTOM_OPTION, provider=provider,
        provider_combo=_Combo(), provider_requirement=_Label(),
        provider_credential_icon=_Label())
    stub._is_running = lambda profile: cls._is_running(stub, profile)
    stub._running_profiles = lambda: cls._running_profiles(stub)
    stub._description = lambda profile: cls._description(stub, profile)
    stub.current_profile = lambda: next(
        (p for p in stub._profiles if p.id == stub.provider), None)
    return stub


def _DESCRIPTIONS(module):
    return dict(module.TextModelPicker._DESCRIPTIONS)


def _SORT_OPTIONS(module):
    return [[label, value] for label, value in module.TextModelPicker._SORT_OPTIONS]


def _description(module, profile, running):
    stub = _stub(module, [profile], running)
    return module.TextModelPicker._description(stub, stub._profiles[0])


def _reload_provider_combo(module, profiles, running):
    stub = _stub(module, profiles, running)
    with _Patched(module, design_icon=lambda name: name):
        module.TextModelPicker._reload_provider_combo(stub)
    return [list(row) for row in stub.provider_combo.rows]


def _update_credential_status(module, profiles, provider, status, running):
    stub = _stub(module, profiles, running, provider)
    shown = {}
    with _Patched(module, design_icon=_icon,
                  credential_status=lambda _profile: (status[0], status[1]),
                  set_style_property=lambda _widget, _name, value: shown.update(available=value)):
        module.TextModelPicker._update_credential_status(stub)
    return {"text": stub.provider_requirement.text, "tooltip": stub.provider_requirement.tooltip,
            "available": shown["available"], "icon": stub.provider_credential_icon.pixmap}


def _update_active_badge(module, profiles, provider, active_provider, active_model, selected):
    stub = _stub(module, profiles, [], provider)
    stub._active_provider = active_provider
    stub._active_model = active_model
    badge, tip = [], []
    stub.model_combo = SimpleNamespace(
        currentText=lambda: selected, set_badge=badge.append, setToolTip=tip.append)
    stub._active_profile_name = lambda: module.TextModelPicker._active_profile_name(stub)
    module.TextModelPicker._update_active_badge(stub)
    return {"badge": badge[0], "tooltip": tip[0]}


WRAPPERS = {
    "_DESCRIPTIONS": _DESCRIPTIONS,
    "_SORT_OPTIONS": _SORT_OPTIONS,
    "_description": _description,
    "_reload_provider_combo": _reload_provider_combo,
    "_update_credential_status": _update_credential_status,
    "_update_active_badge": _update_active_badge,
}

# Detected servers: Ollama at its own address, LM Studio answering at another port, vLLM only
# somewhere else, an Ollama found at a second address.
RUNNING = [["http://localhost:11434/v1", "ollama"], ["http://127.0.0.1:1235/v1", "lm_studio"]]
ELSEWHERE = [["http://127.0.0.1:8001/v1", "vllm"], ["http://127.0.0.1:11435/v1", "ollama"],
             ["http://127.0.0.1:11436/v1", "ollama"]]
OPENROUTER = _builtin("openrouter")
OPENAI = _builtin("openai")
GROQ = _builtin("groq")
OLLAMA = _builtin("ollama")
LM_STUDIO = _builtin("lm_studio")

CASES = {
    "_DESCRIPTIONS": [[]],
    "_SORT_OPTIONS": [[]],
    "_description": (
        [[_builtin(i), []] for i in BUILTIN]
        + [[CUSTOM, []], [LOCAL_CUSTOM, []],
           # Running at its own address, and a custom endpoint at an address a server answers.
           [OLLAMA, RUNNING], [LM_STUDIO, RUNNING], [OPENAI, RUNNING],
           [_custom("custom_ab", "My Ollama", "http://localhost:11434/v1"), RUNNING],
           # Answering elsewhere: the first address of its kind is named; custom never is.
           [_builtin("vllm"), ELSEWHERE], [OLLAMA, ELSEWHERE], [LM_STUDIO, ELSEWHERE],
           [CUSTOM, ELSEWHERE],
           # A running server of another kind does not mark this one.
           [_builtin("localai"), RUNNING]]),
    "_reload_provider_combo": [
        [ALL, []], [PROFILES, []], [ALL, RUNNING], [PROFILES, RUNNING], [ALL, ELSEWHERE],
        [LOCALS, RUNNING], [[OPENAI], []],
    ],
    "_update_credential_status": [
        # A key found, and no key needed.
        [ALL, "openrouter", [True, "OpenRouter API key found"], []],
        [ALL, "ollama", [True, "No API key required"], []],
        [ALL, "openai", [True, "OpenAI API key found"], RUNNING],
        # A key missing: OpenAI and OpenRouter keep their own line, another built-in points at
        # Settings, a custom endpoint keeps its own.
        [ALL, "openai", [False, "Requires OpenAI API key — add it in Settings → App → API keys"],
         []],
        [ALL, "openrouter",
         [False, "Requires OpenRouter API key — add it in Settings → App → API keys"], []],
        [ALL, "groq", [False, "Requires Groq API key — add it in Settings → App → API keys"],
         []],
        [PROFILES, "custom_a1b2c3d4",
         [False, "Requires GATEWAY_KEY — add it in Settings → App → API keys"], []],
        # A key missing while a local server runs: the line points at the first running one,
        # but not for an endpoint on this computer.
        [ALL, "openai", [False, "Requires OpenAI API key — add it in Settings → App → API keys"],
         RUNNING],
        [ALL, "groq", [False, "Requires Groq API key — add it in Settings → App → API keys"],
         RUNNING],
        [PROFILES, "custom_a1b2c3d4",
         [False, "Requires GATEWAY_KEY — add it in Settings → App → API keys"], RUNNING],
        [PROFILES, "custom_0000beef", [False, "Requires nothing"], RUNNING],
        [ALL, "ollama", [False, "Requires nothing"], RUNNING],
        # A server answering elsewhere does not count as running.
        [ALL, "openai", [False, "Requires OpenAI API key — add it in Settings → App → API keys"],
         ELSEWHERE],
    ],
    "_update_active_badge": [
        # The shown endpoint and model are the assignment: badge and the "in use" line.
        [ALL, "openrouter", "openrouter", "openrouter/free", "openrouter/free"],
        [ALL, "openrouter", "openrouter", "openrouter/free", "  openrouter/free  "],
        # Another model, another endpoint, no assignment yet.
        [ALL, "openrouter", "openrouter", "openrouter/free", "anthropic/claude-sonnet-4.5"],
        [ALL, "openai", "openrouter", "openrouter/free", "openrouter/free"],
        [ALL, "openai", "", "", "gpt-6-luna"],
        [ALL, "openai", "openai", "", ""],
        # The assignment names an endpoint that is not listed: a built-in, a stored custom id.
        [[OPENAI], "openai", "groq", "llama-3.3-70b-versatile", "gpt-6-luna"],
        [ALL, "openai", "custom_gone", "some-model", "gpt-6-luna"],
        [PROFILES, "custom_a1b2c3d4", "custom_a1b2c3d4", "gateway-model", "gateway-model"],
        [PROFILES, "openai", "custom_a1b2c3d4", "gateway-model", "gpt-6-luna"],
    ],
}
