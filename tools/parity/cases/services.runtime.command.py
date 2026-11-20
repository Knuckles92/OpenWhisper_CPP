"""Curated inputs for services/runtime/command.py (M4.13 command-runtime): how Command Mode and
transforms decide where a selection comes from, before anything is copied.

A snapshot travels as {"identity", "text"} (FocusSnapshot's fields, each an object of the
dataclass's fields or null), or null for no capture. _refusal's UnreadSelection comes back as
{"message"}, or null.
"""

from services.focus_context import AppIdentity, FocusSnapshot, TextContext


def _snapshot(fields):
    if fields is None:
        return None
    identity = fields.get("identity")
    text = fields.get("text")
    return FocusSnapshot(None if identity is None else AppIdentity(**identity),
                         None if text is None else TextContext(**text))


def _refusal(module, snapshot):
    refused = module._refusal(_snapshot(snapshot))
    return None if refused is None else {"message": refused.message}


def _known_selection(module, snapshot):
    return module._known_selection(_snapshot(snapshot))


def _constants(module):
    return {"NO_PROVIDER_MESSAGE": module.NO_PROVIDER_MESSAGE,
            "KEYS_HELD_MESSAGE": module.KEYS_HELD_MESSAGE,
            "PASSWORD_MESSAGE": module.PASSWORD_MESSAGE,
            "CLIPBOARD_HIDDEN_MESSAGE": module.CLIPBOARD_HIDDEN_MESSAGE,
            "SELECTION_WAIT_S": module.SELECTION_WAIT_S}


WRAPPERS = {
    "_refusal": _refusal,
    "_known_selection": _known_selection,
    "constants": _constants,
}

_NOTEPAD = {"app_id": "notepad.exe", "name": "Notepad", "pid": 4, "window": "0x4"}
_KEEPASS = {"app_id": "keepassxc.exe", "name": "KeePassXC", "pid": 21, "window": "0x21"}
_SLACK = {"app_id": "slack.exe", "name": "Slack", "pid": 11}
_CHROME_GMAIL = {"app_id": "chrome.exe", "name": "Google Chrome", "pid": 9, "window": "0x9",
                 "title_hint": "Gmail"}
_NAMELESS = {"app_id": "", "name": ""}
_UNKNOWN = {"app_id": "frobnicate.exe", "name": ""}


def _excluded(identity):
    return {"identity": identity, "text": {"blocked": True, "source": "excluded"}}


CASES = {
    "_looks_like_copied_line": [
        ["def main():\r\n"], ["\n"], ["main"], ["line one\nline two\n"], [""], ["\r\n"],
        ["a\r\n"], ["a\n\n"], ["a\r\r\n"], ["  x  \n"], ["x\n "], ["\n\n"], ["a\rb\n"],
        ["café — ok\n"], ["\r"], ["one\r\ntwo\r\n"],
    ],
    "_refusal": [
        [None],
        [{"identity": _NOTEPAD, "text": None}],
        [{"identity": _NOTEPAD, "text": {"selected": "x", "selection_known": True}}],
        [{"identity": _NOTEPAD, "text": {"blocked": True, "source": "uia"}}],
        [{"identity": None, "text": {"blocked": True, "source": ""}}],
        [_excluded(_KEEPASS)],
        [_excluded(_SLACK)],
        [_excluded(_CHROME_GMAIL)],
        [_excluded(None)],
        [_excluded(_NAMELESS)],
        [_excluded(_UNKNOWN)],
        [{"identity": _KEEPASS, "text": {"blocked": False, "source": "excluded"}}],
    ],
    "_known_selection": [
        [None],
        [{"identity": _NOTEPAD, "text": None}],
        [{"identity": _NOTEPAD, "text": {"selected": "old words", "selection_known": False}}],
        [{"identity": _NOTEPAD, "text": {"selected": "", "selection_known": True}}],
        [{"identity": _NOTEPAD, "text": {"selected": "old words", "selection_known": True}}],
        [{"identity": None, "text": {"selected": "  spaced  ", "selection_known": True}}],
        [{"identity": _NOTEPAD, "text": {"before": "x", "caret_known": True}}],
    ],
    "constants": [[]],
}
