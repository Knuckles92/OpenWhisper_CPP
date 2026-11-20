"""Curated inputs for services/thinking_levels.py (fv_llm llm/thinking_levels.h).

levels_for runs over Claude Code, OpenCode's CLI and every endpoint with models whose catalog
entries add or remove levels, with settings holding a custom endpoint. Codex is left out: its
levels come from the owner's installed Codex cache, which these vectors must not read; the C++
tests inject Codex's efforts instead. The server spec cache is empty, so a server the user runs
offers nothing. The C++ replay is tests/llm/test_thinking_levels.cpp.
"""

SETTINGS = {"text_llm_profiles": [
    {"id": "custom_resp", "name": "Gateway", "base_url": "https://gateway.test/v1",
     "api_key_env": "", "protocol": "responses"},
    {"id": "custom_chat", "name": "Chat", "base_url": "https://chat.test/v1", "api_key_env": ""},
]}

PAIRS = [
    ("claude_code", "haiku"), ("claude_code", ""), ("opencode_cli", "any"),
    ("openai", "gpt-5"), ("openai", "gpt-5-mini"), ("openai", "gpt-4.1"), ("openai", "gpt-6-luna"),
    ("openai", "o3-mini"), ("openai", ""), ("openai", "   "),
    ("openrouter", "openrouter/free"), ("openrouter", ""),
    ("opencode_go", "glm-5.2"), ("opencode_go", "deepseek-v4-pro"), ("opencode_go", "kimi-k2.6"),
    ("opencode_go", "hy3"), ("opencode_go", "gpt-5.6-luna"), ("opencode_go", "minimax-m3"),
    ("opencode_go", "future-model"),
    ("opencode_zen", "gpt-5"), ("opencode_zen", "gpt-5.4-pro"), ("opencode_zen", "claude-opus-5"),
    ("opencode_zen", "gemini-3.1-pro"), ("opencode_zen", "grok-build-0.1"),
    ("opencode_zen", "kimi-k2.6"), ("opencode_zen", "big-pickle"),
    ("groq", "llama-3.3-70b-versatile"), ("ollama", "qwen3"), ("lm_studio", ""),
    ("custom_resp", "o3-mini"), ("custom_resp", "llama"), ("custom_chat", "o3-mini"),
    ("custom_missing", "x"), ("", "x"),
]

LEVELS = ["off", "minimal", "low", "medium", "high", "xhigh", "max", "ultra", "extreme", ""]
OFFERED = [
    [], ["low", "medium", "high", "xhigh", "max"], ["off", "low", "medium", "high"], ["high"],
    ["off"], ["minimal", "max"], ["low", "xhigh"], ["max", "low"], ["ultra"],
]

CASES = {
    "levels_for": [[p, m, s] for p, m in PAIRS for s in ({}, SETTINGS)],
    "nearest": [[level, offered] for level in LEVELS for offered in OFFERED],
    "effective_level": [[level, offered] for level in LEVELS for offered in OFFERED],
}

WRAPPERS = {
    "levels_for": lambda module, provider, model, settings: list(
        module.levels_for(provider, model, settings)),
    "nearest": lambda module, level, offered: module.nearest(level, tuple(offered)),
    "effective_level": lambda module, level, offered: module.effective_level(level, tuple(offered)),
}

PINS = {"codex": "not called: its levels read the installed Codex cache"}
