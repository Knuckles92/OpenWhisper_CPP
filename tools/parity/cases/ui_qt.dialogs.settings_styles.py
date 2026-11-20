"""Curated inputs for ui_qt/dialogs/settings_styles.py (M4.4 text-prompts): the copy the Apps &
styles page and Home compute.

Settings travel as plain dicts keyed by the bare setting names. `reads_text_near_cursor` and
`privacy_note` take a second argument, what focus_context.text_reading_supported() answers (the
wrapper patches it, so the non-Windows branch is covered). `privacy_note` returns {"provider":
{"name": profile_display_name, "local": the profile's is_local}, "result": the note}: the C++
side is handed the resolved provider.
"""

import itertools

_TONES = ("formal", "casual", "very_casual")
_CATEGORIES = ("email", "work", "personal", "other")

_SETTINGS = [
    {},
    {"transcript_cleanup_enabled": True},
    {"transcript_cleanup_enabled": True, "app_context_enabled": False},
    {"transcript_cleanup_enabled": True, "app_styles_enabled": False},
    {"transcript_cleanup_enabled": True, "app_style_tones": {"work": "casual"}},
    {"transcript_cleanup_enabled": True,
     "app_style_tones": {"email": "formal", "work": "casual", "personal": "casual",
                         "other": "formal"}},
    {"app_context_enabled": False},
    {"app_context_read_text": True},
    {"app_context_read_text": True, "transcript_cleanup_enabled": True},
    {"app_context_read_text": True, "app_context_enabled": False},
    {"transcript_cleanup_enabled": True, "transcript_cleanup_provider": "ollama",
     "transcript_cleanup_model": "llama3.2"},
    {"transcript_cleanup_enabled": True, "transcript_cleanup_provider": "openai",
     "transcript_cleanup_model": "gpt-4o-mini"},
    {"transcript_cleanup_enabled": True, "transcript_cleanup_provider": "lm_studio",
     "transcript_cleanup_model": "qwen"},
    {"transcript_cleanup_enabled": True, "transcript_cleanup_provider": "custom_gone",
     "transcript_cleanup_model": "m"},
    {"transcript_cleanup_enabled": True, "transcript_cleanup_provider": "custom_mine",
     "transcript_cleanup_model": "m",
     "text_llm_profiles": [{"id": "custom_mine", "name": "My server",
                            "base_url": "http://127.0.0.1:9000/v1", "api_key_env": "",
                            "protocol": "chat_completions"}]},
    {"transcript_cleanup_enabled": True, "transcript_cleanup_provider": "custom_far",
     "transcript_cleanup_model": "m",
     "text_llm_profiles": [{"id": "custom_far", "name": "Far server",
                            "base_url": "https://llm.example.com/v1", "api_key_env": "",
                            "protocol": "chat_completions"}]},
]

_OVERRIDES = [
    [],
    [["Notion", "work"]],
    [["Notion", "work"], ["Linear", "work"], ["Gmail", "personal"]],
    [["slack", "email"], ["Teams", "other"]],
    [["A", "work"], ["B", "work"], ["C", "work"], ["D", "work"], ["E", "work"]],
    [["Outlook", "other"], ["Gmail", "work"], ["Proton Mail", "personal"]],
    [["WhatsApp", "work"], ["whatsapp", "personal"]],
]


def _reads(module, settings, supported):
    original = module.text_reading_supported
    module.text_reading_supported = lambda: supported
    try:
        return module.reads_text_near_cursor(settings)
    finally:
        module.text_reading_supported = original


def _privacy(module, settings, supported):
    original = module.text_reading_supported
    module.text_reading_supported = lambda: supported
    try:
        result = module.privacy_note(settings)
    finally:
        module.text_reading_supported = original
    provider = module.resolve_transcript_cleanup_provider(settings)
    profile = module.get_profile(provider, settings)
    return {
        "provider": {"name": module.profile_display_name(provider, settings),
                     "local": bool(profile is not None and profile.is_local)},
        "result": result,
    }


CASES = {
    "cleanup_on": [[settings] for settings in _SETTINGS],
    "reads_text_near_cursor": [[settings, supported] for settings in _SETTINGS
                               for supported in (True, False)],
    "styles_blocker": [[settings] for settings in _SETTINGS],
    "rail_value": [[settings] for settings in _SETTINGS],
    "home_copy": [[{"app_style_tones": dict(zip(_CATEGORIES, tones))}]
                  for tones in itertools.product(_TONES, repeat=4)]
    + [[{}], [{"app_style_tones": {"work": "shouty"}}]],
    "apps_summary": [[category, overrides] for category in _CATEGORIES
                     for overrides in _OVERRIDES],
    "privacy_note": [[settings, supported] for settings in _SETTINGS
                     for supported in (True, False)],
    "_excluded": [[settings] for settings in (
        {}, {"app_context_excluded_apps": ["  Slack ", "slack", "", 3, None, "Outlook"]},
        {"app_context_excluded_apps": "oops"},
    )],
}

WRAPPERS = {
    "reads_text_near_cursor": _reads,
    "privacy_note": _privacy,
}
