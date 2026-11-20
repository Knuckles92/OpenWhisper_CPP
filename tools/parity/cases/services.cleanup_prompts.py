"""Curated inputs for services/cleanup_prompts.py (M4.4 text-prompts): levels, custom prompts,
presets, the inline-lists block and the level label, and the generated sentence data compared
with config attribute by attribute.

Settings travel as plain dicts keyed by the bare setting names. A FocusSnapshot travels as
{"identity": {AppIdentity fields} or null}, or null for no snapshot. `config(name)` is
getattr(config, name): the C++ replay compares it with data/cleanup_prompts.json.
"""

_PIRATE = "Write like a pirate."
_SLACK = "  Make this a Slack message.  "
_OLD = (
    "You clean up speech-to-text transcripts. Fix punctuation and capitalization, remove "
    "filler words (um, uh, like as filler, you know), and fix obvious ASR errors. Do not invent "
    "content, do not add information that was not spoken, and preserve meaning, tone, and "
    "proper nouns. Return only the cleaned transcript text with no preamble or quotes."
)

_ON = {"transcript_cleanup_enabled": True}


def _on(**values):
    return {**_ON, **values}


_SETTINGS = [
    {},
    {"transcript_cleanup_level": "high"},
    {"transcript_cleanup_enabled": "yes"},
    {"transcript_cleanup_enabled": 1},
    {"transcript_cleanup_enabled": False, "transcript_cleanup_level": "light"},
    _ON,
    _on(transcript_cleanup_level="light"),
    _on(transcript_cleanup_level="medium"),
    _on(transcript_cleanup_level="high"),
    _on(transcript_cleanup_level="none"),
    _on(transcript_cleanup_level="HIGH"),
    _on(transcript_cleanup_level=3),
    _on(transcript_cleanup_level=None),
    {"transcript_cleanup_prompt": _PIRATE},
    _on(transcript_cleanup_prompt=_PIRATE),
    _on(transcript_cleanup_prompt=_SLACK, transcript_cleanup_level="high"),
    _on(transcript_cleanup_prompt=_OLD),
    _on(transcript_cleanup_prompt=f"\n  {_OLD}  "),
    _on(transcript_cleanup_prompt=_OLD.replace(". ", ".\n")),
    _on(transcript_cleanup_prompt="   "),
    _on(transcript_cleanup_prompt=42),
    _on(transcript_cleanup_prompt=None),
    {"transcript_cleanup_prompt": "  Keep my British spelling. ", "transcript_cleanup_level": "light"},
    _on(transcript_cleanup_prompt="Straße  und\tStraße"),
]

_NOTEPAD = {"app_id": "notepad.exe", "name": "Notepad", "pid": 7, "window": "0x7",
            "title_hint": "", "is_self": False, "platform": ""}
_TERMINAL = {"app_id": "windowsterminal.exe", "name": "Windows Terminal", "pid": 8,
             "window": "0x8", "title_hint": "", "is_self": False, "platform": ""}
_POWERSHELL = {"app_id": "powershell.exe", "name": "Windows PowerShell", "pid": 9,
               "window": "", "title_hint": "", "is_self": False, "platform": ""}
_UNKNOWN = {"app_id": "mystery-tool.exe", "name": "Mystery", "pid": None, "window": "",
            "title_hint": "", "is_self": False, "platform": ""}
_SELF = {"app_id": "openwhisper", "name": "Ours", "pid": 3, "window": "", "title_hint": "",
         "is_self": True, "platform": ""}
_SNAPSHOTS = [None, {"identity": None}, {"identity": _NOTEPAD}, {"identity": _TERMINAL},
              {"identity": _POWERSHELL}, {"identity": _UNKNOWN}, {"identity": _SELF}]

_CONFIG = [
    "TRANSCRIPT_CLEANUP_ENABLED", "TRANSCRIPT_CLEANUP_TIMEOUT_S",
    "TRANSCRIPT_CLEANUP_MAX_RETRIES", "TRANSCRIPT_CLEANUP_PROVIDER", "TRANSCRIPT_CLEANUP_MODEL",
    "TRANSCRIPT_CLEANUP_OPENROUTER_MODEL", "TRANSCRIPT_CLEANUP_MODEL_SORT",
    "TRANSCRIPT_CLEANUP_REASONING", "OPENROUTER_BASE_URL", "TRANSCRIPT_CLEANUP_PROMPT",
    "LEGACY_DEFAULT_CLEANUP_PROMPTS", "TRANSCRIPT_CLEANUP_LEVEL", "TRANSCRIPT_CLEANUP_INTRO",
    "TRANSCRIPT_CLEANUP_BASICS", "TRANSCRIPT_CLEANUP_LIGHT_ONLY", "TRANSCRIPT_CLEANUP_TIGHTEN",
    "TRANSCRIPT_CLEANUP_SPOKEN_CORRECTIONS", "TRANSCRIPT_CLEANUP_SPOKEN_LISTS",
    "TRANSCRIPT_CLEANUP_PARAGRAPHS", "TRANSCRIPT_CLEANUP_UNKNOWN_APP_LINES",
    "TRANSCRIPT_CLEANUP_TERMINAL_LINES", "TRANSCRIPT_CLEANUP_LANGUAGE", "TRANSCRIPT_CLEANUP_GUARD",
    "TRANSCRIPT_CLEANUP_LEVEL_PROMPTS", "MAX_TRANSCRIPT_CLEANUP_RULES", "MAX_DICTIONARY_TERMS",
    "MAX_RECOGNITION_PHRASES", "MAX_SNIPPETS", "MAX_TEXT_TRANSFORMS", "CONTEXT_BEFORE_CHARS",
    "CONTEXT_AFTER_CHARS", "CONTEXT_CAPTURE_DEADLINE_S", "COMMAND_SELECTION_TIMEOUT_MS",
    "TRANSCRIPT_CLEANUP_RULE_POLISH_PROMPT", "TRANSCRIPT_BATCH_CLEANUP_TIMEOUT_S",
    "TRANSCRIPT_BATCH_CLEANUP_MAX_RETRIES",
]


def _snapshot(fields):
    from services.focus_context import AppIdentity, FocusSnapshot

    if fields is None:
        return None
    identity = fields.get("identity")
    return FocusSnapshot(None if identity is None else AppIdentity(**identity))


def _config(module, name):
    from config import config

    value = getattr(config, name)
    if isinstance(value, tuple):
        return list(value)
    if not isinstance(value, (str, int, float, bool, list)):
        return dict(value)
    return value


CASES = {
    "config": [[name] for name in _CONFIG],
    "is_built_in": [[text] for text in (
        "", " ", _PIRATE, _OLD, f"  {_OLD}\n", _OLD.replace(" ", "\n"), _OLD + ".",
        _OLD.lower(),
    )],
    "resolve_level": [[settings] for settings in _SETTINGS],
    "custom_prompt": [[settings] for settings in _SETTINGS],
    "preset": [[level] for level in ("light", "medium", "high", "none", "bogus", "", "HIGH")],
    "base_prompt": [[settings, level] for settings in _SETTINGS
                    for level in ("none", "light", "medium", "high", "bogus")],
    "inline_lists_block": [[settings, snapshot] for settings in (
        {}, _ON, _on(transcript_cleanup_level="light"), _on(transcript_cleanup_level="high"),
        _on(transcript_cleanup_prompt="Bullet points only."),
        _on(transcript_cleanup_prompt=_OLD, transcript_cleanup_level="high"),
        {**_on(transcript_cleanup_level="medium"), "app_styles_enabled": False},
    ) for snapshot in _SNAPSHOTS],
    "level_label": [[settings] for settings in _SETTINGS],
}

WRAPPERS = {
    "config": _config,
    "inline_lists_block": lambda module, settings, snapshot: module.inline_lists_block(
        settings, _snapshot(snapshot)),
}
