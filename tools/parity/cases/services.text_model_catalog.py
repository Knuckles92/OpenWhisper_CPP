"""Curated inputs for services/text_model_catalog.py (fv_llm llm/model_catalog.h).

model_spec runs over every built-in profile (get_profile(id, {})) with every model the OpenCode
route table names, a set of OpenAI ids and a few others, and over profiles given as their
TextLLMProfile fields (custom Responses endpoints, saved meeting metadata). The server spec cache
is empty throughout, so a self-hosted profile reads the modest default. A result is
TextModelSpec.to_dict(); a ValueError is recorded as {"raises": "ValueError", "message": ...}.
The C++ replay is tests/llm/test_model_catalog.cpp.
"""

OPENAI_IDS = [
    "gpt-4o", "gpt-4o-audio-preview", "ft:gpt-4.1:org:x", "o3-mini", "gpt-6-luna", "gpt-5",
    "GPT-6-LUNA", "ft:gpt-6-luna:org:x", "gpt-4o-mini", "gpt-4o-2024-08-06", "gpt-4.1-mini",
    "ft:gpt-4.1-mini-2025-04-14:org:custom:123", "gpt-4", "gpt-3.5-turbo", "chatgpt-4o-latest",
    "gpt-4o-search-preview", "gpt-4o-mini-transcribe", "gpt-4o-realtime-preview", "gpt-4o-mini-tts",
    "o1", "o4-mini", "gpt-5.6-luna", "o3:custom", "ft:o3-mini:org", "",
]
OTHER_IDS = ["openrouter/free", "openai/gpt-6-luna", "llama-3.3-70b-versatile", "whisper-large-v3",
             "playai-tts", "llama-guard-4", "future-model", "  ", "qwen3"]

BUILTINS = ["openai", "openrouter", "ollama", "lm_studio", "vllm", "llama_cpp", "localai", "groq",
            "opencode_go", "opencode_zen"]

PROFILES = [
    {"id": "custom_resp", "name": "Gateway", "kind": "custom",
     "base_url": "https://gateway.test/v1", "api_key_env": "", "protocol": "responses"},
    {"id": "custom_resp_local", "name": "Local", "kind": "custom",
     "base_url": "http://localhost:1234/v1", "api_key_env": "", "protocol": "responses"},
    {"id": "custom_chat", "name": "Chat", "kind": "custom",
     "base_url": "https://gateway.test/v1", "api_key_env": "", "protocol": "chat"},
    {"id": "custom_local", "name": "Local chat", "kind": "custom",
     "base_url": "http://127.0.0.1:9000/v1", "api_key_env": ""},
    {"id": "custom_saved", "name": "Saved", "kind": "custom", "base_url": "https://lab.test/v1",
     "api_key_env": "", "model_metadata": {"model": "lab-model", "protocol": "anthropic",
                                           "context_window": 500, "max_output_tokens": 999999,
                                           "reasoning": True, "reasoning_format": "qwen",
                                           "thinking_levels": {"off": None}}},
    {"id": "openai", "name": "OpenAI", "kind": "openai", "base_url": None,
     "api_key_env": "OPENAI_API_KEY", "builtin": True,
     "model_metadata": {"model": "o4-mini", "protocol": "chat", "reasoning": True}},
    {"id": "opencode_zen", "name": "OpenCode Zen", "kind": "opencode_zen",
     "base_url": "https://opencode.ai/zen/v1", "api_key_env": "OPENCODE_ZEN_API_KEY",
     "builtin": True, "model_metadata": {"model": "future-model", "protocol": "google"}},
    {"id": "ollama", "name": "Ollama", "kind": "ollama", "base_url": "http://host-a:11434/v1",
     "api_key_env": "", "model_metadata": {"model": "llama3.2", "protocol": "chat",
                                           "context_window": 4096}},
    {"id": "groq", "name": "Groq", "kind": "groq", "base_url": "https://api.groq.com/openai/v1",
     "api_key_env": "GROQ_API_KEY", "model_metadata": {}},
]

PROFILE_MODELS = ["synthetic-model", "o3-mini", "gpt-6-luna", "ft:o4-mini:x", "lab-model",
                  "o4-mini", "future-model", "llama3.2", "whisper-1", "qwen3"]

MAPPINGS = [
    {}, {"protocol": "chat"}, {"protocol": "responses"}, {"protocol": "anthropic"},
    {"protocol": "google"}, {"protocol": "other"}, {"protocol": None}, {"protocol": ["chat"]},
    {"context_window": 10}, {"context_window": 5000000}, {"context_window": 2000000},
    {"context_window": 1024}, {"context_window": "8192"}, {"context_window": " 12 "},
    {"context_window": 3.9}, {"context_window": True}, {"context_window": None},
    {"context_window": "abc"}, {"context_window": [1]}, {"context_window": -7},
    {"max_output_tokens": 1}, {"max_output_tokens": 200000}, {"max_output_tokens": 4096.5},
    {"max_output_tokens": "1e5"}, {"max_output_tokens": {}},
    {"tools": False}, {"tools": 1}, {"tools": "true"}, {"tools": True},
    {"reasoning": True}, {"reasoning": 1}, {"reasoning": "yes"},
    {"reasoning_format": "openai"}, {"reasoning_format": "deepseek"}, {"reasoning_format": "qwen"},
    {"reasoning_format": "anthropic"}, {"reasoning_format": None}, {"reasoning_format": ["openai"]},
    {"pi_compat": {"a": 1}}, {"pi_compat": [1]}, {"pi_compat": "x"},
    {"thinking_levels": {"off": None, "high": "high"}}, {"thinking_levels": []},
    {"thinking_levels": "off"},
    {"model": "m", "protocol": "responses", "tools": True, "context_window": 1050000,
     "max_output_tokens": 16384, "reasoning": True, "reasoning_format": "openai",
     "pi_compat": {"sessionAffinityFormat": "openai-nosession"},
     "thinking_levels": {"off": None, "low": "low"}},
]


def _profile(fields):
    from services.text_llm import TextLLMProfile, get_profile

    if isinstance(fields, str):
        return get_profile(fields, {})
    return TextLLMProfile(**fields)


def _model_spec(module, profile, model):
    try:
        return module.model_spec(_profile(profile), model).to_dict()
    except ValueError as exc:
        return {"raises": "ValueError", "message": str(exc)}


def _spec_from_mapping(module, raw):
    try:
        return module.spec_from_mapping(raw).to_dict()
    except (ValueError, TypeError, OverflowError) as exc:
        return {"raises": type(exc).__name__, "message": str(exc)}


class _LazyCases(dict):
    """Filled when first read, after ow_isolation.isolate() has made the import safe."""

    def _fill(self):
        if dict.__len__(self):
            return
        from services import text_model_catalog as catalog

        routed = []
        for routes in catalog._ROUTES.values():
            for models in routes.values():
                for model in models:
                    if model not in routed:
                        routed.append(model)
        models = OPENAI_IDS + OTHER_IDS + routed
        dict.__setitem__(self, "model_spec", [[p, m] for p in BUILTINS for m in models]
                         + [[p, m] for p in PROFILES for m in PROFILE_MODELS])
        dict.__setitem__(self, "spec_from_mapping", [[m] for m in MAPPINGS])

    def __iter__(self):
        self._fill()
        return dict.__iter__(self)

    def __len__(self):
        self._fill()
        return dict.__len__(self)

    def get(self, key, default=None):
        self._fill()
        return dict.get(self, key, default)

    def __getitem__(self, key):
        self._fill()
        return dict.__getitem__(self, key)


CASES = _LazyCases()

WRAPPERS = {
    "model_spec": _model_spec,
    "spec_from_mapping": _spec_from_mapping,
}

PINS = {
    "profiles": "a string profile is get_profile(id, {}); a mapping is TextLLMProfile(**fields)",
    "server_specs": "empty: no discovery runs while these vectors are made",
}
