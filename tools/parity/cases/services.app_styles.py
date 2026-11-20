"""Curated inputs for services/app_styles.py (M4.4 text-prompts): tones, overrides, the style
of the catalogue's apps and sites, the prompt block and the mutators.

Settings travel as plain dicts keyed by the bare setting names. A FocusSnapshot travels as
{"identity": {AppIdentity fields} or null}, or null for no snapshot. An AppStyle travels as
{category, tone, app_name, surface}. A mutator runs on a deep copy of its settings and returns
{"result": its return value, "settings": the settings after} or {"error": the ValueError's
message}.
"""

import copy

_NEW_INSTALL = {"app_style_tones": {"email": "formal", "work": "casual", "personal": "casual",
                                    "other": "formal"}}
_VERY = {"app_style_tones": {"email": "casual", "work": "very_casual", "personal": "very_casual",
                             "other": "casual"}}


def _identity(app_id, name, title_hint="", is_self=False):
    return {"identity": {"app_id": app_id, "name": name, "pid": 1, "window": "0x1",
                         "title_hint": title_hint, "is_self": is_self, "platform": ""}}


_SNAPSHOTS = [
    None,
    {"identity": None},
    _identity("outlook.exe", "Outlook"),
    _identity("chrome.exe", "Chrome", "Gmail"),
    _identity("chrome.exe", "Chrome", "Slack"),
    _identity("chrome.exe", "Chrome"),
    _identity("slack.exe", "Slack"),
    _identity("whatsapp.exe", "WhatsApp"),
    _identity("notion.exe", "Notion"),
    _identity("windowsterminal.exe", "Windows Terminal"),
    _identity("code.exe", "VS Code"),
    _identity("notepad.exe", "Notepad"),
    _identity("mystery-tool.exe", "Mystery"),
    _identity("", ""),
    _identity("openwhisper", "Ours", is_self=True),
]

_SETTINGS = [
    {},
    _NEW_INSTALL,
    _VERY,
    {**_NEW_INSTALL, "app_styles_enabled": False},
    {**_NEW_INSTALL, "app_style_overrides": [
        {"match": "notion", "category": "work"},
        {"match": "Gmail", "category": "personal"},
        {"match": "Windows Terminal", "category": "email"},
    ]},
    {"app_styles_enabled": False,
     "app_style_overrides": [{"match": "Notion", "category": "work"}]},
    {"app_style_tones": {"work": "shouty", "email": "casual"}},
    {"app_style_tones": "oops"},
    {"app_style_tones": None},
    {"app_style_tones": {"email": ["casual"], "other": 3}},
    {"app_style_overrides": "oops"},
    {"app_style_overrides": [None, {"match": "", "category": "work"},
                             {"match": "Slack", "category": "nope"},
                             {"match": "slack", "category": "email"},
                             {"match": "SLACK", "category": "personal"}]},
]


def _snapshot(fields):
    from services.focus_context import AppIdentity, FocusSnapshot

    if fields is None:
        return None
    identity = fields.get("identity")
    return FocusSnapshot(None if identity is None else AppIdentity(**identity))


def _style(fields):
    from services.app_styles import AppStyle

    return None if fields is None else AppStyle(**fields)


def _plain(style):
    if style is None:
        return None
    return {"category": style.category, "tone": style.tone, "app_name": style.app_name,
            "surface": style.surface}


def _mutator(name):
    def run(module, settings, *args):
        changed = copy.deepcopy(settings)
        try:
            result = getattr(module, name)(changed, *args)
        except ValueError as exc:
            return {"error": str(exc)}
        return {"result": result, "settings": changed}

    return run


_STYLES = [None] + [
    {"category": category, "tone": tone, "app_name": "Slack", "surface": surface}
    for category in ("email", "work", "personal", "other", "unknown")
    for tone in ("formal", "casual", "very_casual", "shouty")
    for surface in ("text", "code", "terminal")
]

CASES = {
    "resolve_tones": [[settings] for settings in _SETTINGS],
    "resolve_overrides": [[settings] for settings in _SETTINGS],
    "category_for": [[snapshot, settings] for snapshot in _SNAPSHOTS for settings in _SETTINGS],
    "style_for": [[snapshot, settings] for snapshot in _SNAPSHOTS for settings in _SETTINGS],
    "prompt_block": [[style] for style in _STYLES],
    "set_tone": [[settings, category, tone] for settings in ({}, _NEW_INSTALL, _SETTINGS[6])
                 for category in ("email", "work", "other", "chat")
                 for tone in ("formal", "very_casual", "shouty")],
    "set_override": [[settings, match, category] for settings in (
        {},
        {"app_style_overrides": [{"match": "notion", "category": "work"},
                                 {"match": "Linear", "category": "work"}]},
        {"app_style_overrides": [{"match": f"App {i}", "category": "work"} for i in range(100)]},
    ) for match, category in (
        (" Notion ", "work"), ("notion", "personal"), ("Notion", "other"), ("LINEAR", "email"),
        ("Slack", "work"), ("Slack", "email"), ("Gmail", "email"), ("  ", "work"), ("", "work"),
        ("Notion", "chat"), ("x" * 130, "work"), ("App 99", "email"),
    )],
    "remove_override": [[settings, match] for settings in (
        {},
        {"app_style_overrides": [{"match": "notion", "category": "work"},
                                 {"match": "Linear", "category": "work"}]},
        {"app_style_overrides": "oops"},
    ) for match in ("LINEAR", " notion ", "missing", "")],
}

WRAPPERS = {
    "resolve_overrides": lambda module, settings: [
        list(pair) for pair in module.resolve_overrides(settings)],
    "category_for": lambda module, snapshot, settings: module.category_for(
        _snapshot(snapshot), settings),
    "style_for": lambda module, snapshot, settings: _plain(
        module.style_for(_snapshot(snapshot), settings)),
    "prompt_block": lambda module, style: module.prompt_block(_style(style)),
    "set_tone": _mutator("set_tone"),
    "set_override": _mutator("set_override"),
    "remove_override": _mutator("remove_override"),
}
