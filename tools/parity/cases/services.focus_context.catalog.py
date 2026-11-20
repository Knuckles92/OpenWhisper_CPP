"""Curated inputs for services/focus_context/catalog.py (M1.6): app and site lookups, names,
browser-title sites, overrides and classification.

AppIdentity travels as a JSON object of its fields (null for None). Results that are not JSON
are turned into it by the wrappers: an AppKind is [category, surface, name], a KnownApp is
{name, category, surface, ids, browser}, app_keys is the sorted list of the set, override pairs
are [match, category] lists.
"""

from services.focus_context import AppIdentity

_TITLES = [
    "Inbox (3) - dylan@example.com - Gmail - Google Chrome",
    "Inbox (3,021) - dylan@example.com - Gmail",
    "Mail - Dylan - Outlook - Personal - Microsoft\u200b Edge",
    "Inbox - Gmail and 3 more pages - Work - Microsoft\u200b Edge",
    "general (Channel) - Acme - Slack — Mozilla Firefox",
    "Chat | Microsoft Teams - Google Chrome",
    "Discord | #general | Friends - Brave",
    "(2) WhatsApp - Google Chrome",
    "Messenger | Facebook - Opera",
    "Telegram Web - Google Chrome",
    "(5) Messaging | LinkedIn - Google Chrome",
    "Roadmap notes - Google Docs - Google Chrome",
    "Notes about Slack - Google Docs",
    "[ABC-12] Fix login - Jira - Google Chrome",
    "Pull requests · acme/app · GitHub - Arc",
    "Weather in Paris - Google Search - Google Chrome",
    "",
    "Gmail",
    "gmail",
    "  Gmail  ",
    "● (12) Gmail - Chrome",
    "• Slack - Chrome",
    "* (1+) Discord",
    "(1.5) WhatsApp",
    "Inbox - GMAIL AND 12 MORE PAGES - Edge",
    "Inbox - Gmail and 1 more page",
    "Gmail-Chrome",
    "Gmail -Chrome",
    "Proton Mail - protonmail - Firefox",
    "outlook web app - Edge",
    "Doc – Google Sheets – Vivaldi",
    "Doc / Google Slides / Vivaldi",
    "Chat · Google Chat",
    "\u200eWhatsApp\u200f",
    "\ufeffClaude",
    "ChatGPT | Notion",
    "x" * 600 + " - Gmail",
    "Gmail - " + "y" * 600,
    "Superhuman - Superhuman",
    "Zoho Mail | iCloud Mail",
    "a - b - c - Linear",
    "(3) Instagram • Direct",
]

_IDS = [
    "OUTLOOK.EXE", "olk.exe", "thunderbird", "com.apple.mail", "slack.exe", "ms-teams.exe",
    "com.tinyspeck.slackmacgap", "WhatsApp.exe", "com.apple.MobileSMS", "discord", "Code.exe",
    "pycharm64.exe", "devenv.exe", "WindowsTerminal.exe", "conhost.exe", "OpenConsole.exe",
    "pwsh.exe", "com.googlecode.iterm2", "winword.exe", "chrome.exe", " Chrome.EXE ",
    "msedge.exe", "firefox", "sumatra_pdf.exe", "com.example.FooBar", "my-tool", "my_tool.app",
    "Some App.exe", "already Mixed.exe", "", "  ", "a.b.c", "tool.BIN", "UPPER.EXE",
    "éditeur.exe", "STRASSE.exe", "foo__bar--baz", "notepad.exe", "powershell_ise.exe",
    "applicationframehost.exe", "whatsapp.root.exe", "soffice.bin", "claude.exe",
]

_IDENTITIES = [
    None,
    {"app_id": "slack.exe", "name": "Slack"},
    {"app_id": "firefox.exe", "name": "Firefox", "title_hint": "Gmail"},
    {"app_id": "chrome.exe", "name": "Chrome", "title_hint": "Gmail"},
    {"app_id": "chrome.exe", "name": "Chrome"},
    {"app_id": "chrome.exe", "name": "Chrome", "title_hint": "Recipes"},
    {"app_id": "chrome.exe", "name": "Chrome", "title_hint": " gmail "},
    {"app_id": "notion.exe", "name": "Notion"},
    {"app_id": "winword.exe", "name": "Word"},
    {"app_id": "sumatra_pdf.exe", "name": ""},
    {"app_id": "whatever.exe", "name": "Whatever Studio"},
    {"app_id": "", "name": ""},
    {"app_id": "", "name": "Named"},
    {"app_id": "OUTLOOK.EXE", "name": "Microsoft Outlook"},
    {"app_id": "com.mitchellh.ghostty", "name": "com.mitchellh.ghostty"},
    {"app_id": "conhost.exe", "name": "Console"},
    {"app_id": "keepassxc.exe", "name": "KeePassXC"},
    {"app_id": "openwhisper", "name": "OpenWhisper", "is_self": True},
    {"app_id": "Code.exe", "name": "Visual Studio Code"},
]

_OVERRIDE_SETS = [
    [],
    [{"match": "notion", "category": "work"}, {"match": "WINWORD", "category": "email"},
     {"match": "Gmail", "category": "personal"}],
    [{"match": "chrome", "category": "work"}],
    [{"match": "Recipes", "category": "personal"}, {"match": "Chrome", "category": "email"}],
    [{"match": " slack ", "category": "personal"}, {"match": "Slack", "category": "email"}],
    [{"match": "Microsoft Outlook", "category": "work"}],
    [{"match": "keepassxc", "category": "work"}],
    [{"match": "Sumatra Pdf", "category": "work"}],
]

_RAW_OVERRIDES = [
    [
        {"match": " Slack ", "category": "personal"},
        {"match": "slack", "category": "email"},
        {"match": "", "category": "work"},
        {"match": "Zoom", "category": "loud"},
        {"match": 3, "category": "work"},
        "Teams",
        {"match": "x" * 500, "category": "other"},
    ],
    "nope",
    None,
    {"match": "Slack", "category": "work"},
    [{"match": f"app{index}", "category": "work"} for index in range(300)],
    [{"match": "  ", "category": "work"}, {"match": "STRASSE", "category": "work"},
     {"match": "straße", "category": "email"}],
    [{"match": "a", "category": ["work"]}, {"match": "b", "category": None},
     {"match": "c", "category": "Work"}, {"match": "d"}, {"category": "work"}],
    [{"match": "é" * 130, "category": "personal"}],
    [],
]

_MATCH_VALUES = ["SLACK.EXE", "slack", "teams", "  ", "", "Slack", "gmail", "Firefox",
                 "firefox.exe", "firefox", " Gmail ", "chrome", "Chrome", "Whatever Studio",
                 "whatever", "Sumatra Pdf", "sumatra_pdf"]

_NAMES = ["Outlook", "Gmail", "Mail", "Thunderbird", "Slack", "Teams", "Google Chat", "Zoom",
          "WhatsApp", "Messages", "Telegram", "Discord", "Word", "Notion", "Google Docs",
          "VS Code", "Unknown thing", "", " slack ", "OUTLOOK", "microsoft teams", "LinkedIn",
          "Chrome", "Console", "iTerm2"]


def _identity(fields):
    return None if fields is None else AppIdentity(**fields)


def _classify(module, identity, raw):
    kind = module.classify(_identity(identity), module.parse_overrides(raw))
    return None if kind is None else [kind.category, kind.surface, kind.name]


def _lookup(module, app_id):
    app = module.lookup(app_id)
    if app is None:
        return None
    return {"name": app.name, "category": app.category, "surface": app.surface,
            "ids": list(app.ids), "browser": app.browser}


CASES = {
    "site_from_title": [[title] for title in _TITLES],
    "title_hint": [[app_id, title] for app_id in ("chrome.exe", "msedge.exe", "code.exe",
                                                  "winword.exe", "Firefox", "")
                   for title in _TITLES[:12]],
    "classify": [[identity, raw] for identity in _IDENTITIES for raw in _OVERRIDE_SETS],
    "app_name": [[app_id, fallback] for app_id in _IDS
                 for fallback in ("", "Whatever Studio", "  Padded  ")],
    "pretty_name": [[app_id] for app_id in _IDS],
    "matches": [[identity, value] for identity in _IDENTITIES for value in _MATCH_VALUES],
    "parse_overrides": [[raw] for raw in _RAW_OVERRIDES],
    "lookup": [[app_id] for app_id in _IDS],
    "app_keys": [[identity] for identity in _IDENTITIES if identity is not None],
    "is_browser": [[app_id] for app_id in _IDS],
    "is_terminal": [[identity] for identity in _IDENTITIES],
    "known_names": [[]],
    "builtin_category": [[name] for name in _NAMES],
}

WRAPPERS = {
    "classify": _classify,
    "lookup": _lookup,
    "matches": lambda module, identity, value: module.matches(_identity(identity), value),
    "parse_overrides": lambda module, raw: [list(pair) for pair in module.parse_overrides(raw)],
    "app_keys": lambda module, identity: sorted(module.app_keys(_identity(identity))),
    "is_terminal": lambda module, identity: module.is_terminal(_identity(identity)),
}

PINS = {
    "classify_note": "classify's second argument is the raw stored overrides, passed through "
                     "parse_overrides first; app keys are tried in set order, which no case "
                     "here makes ambiguous",
}
