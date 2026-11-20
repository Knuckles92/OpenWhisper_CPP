"""Curated inputs for services/_hotkey_pynput.py (M1.5 hotkeys): the macOS and Linux grammar and
display only (the listener, Carbon and X11 parts are other-platform).

format_hotkey_display picks macOS symbols by sys.platform; the wrapper pins the platform the
case names, as OpenWhisper's tests do. The C++ replay is tests/hotkeys/test_golden.cpp.
"""

import importlib.util
from pathlib import Path
from unittest.mock import patch

_CASES_DIR = Path(__file__).resolve().parent
_spec = importlib.util.spec_from_file_location(
    "cases_hotkey_common_spellings", _CASES_DIR / "services._hotkey_common.py")
_common = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(_common)
SPELLINGS = _common.SPELLINGS


def _parse(module, hotkey):
    modifiers, main_key = module.parse_hotkey(hotkey)
    return [sorted(modifiers), main_key]


def _format(module, modifiers, main_key):
    return module.format_hotkey(frozenset(modifiers), main_key)


def _display(module, hotkey, platform):
    with patch.object(module.sys, "platform", platform):
        return module.format_hotkey_display(hotkey)


WRAPPERS = {
    "parse_hotkey": _parse,
    "format_hotkey": _format,
    "format_hotkey_display": _display,
}

CASES = {
    "parse_hotkey": [[s] for s in SPELLINGS],
    "format_hotkey": [[[], "kp *"], [["alt", "ctrl", "cmd"], "r"], [["shift", "cmd"], "esc"],
                      [["win"], "x"], [["ctrl"], None], [[], ""]],
    "format_hotkey_display": [[s, p] for p in ("linux", "darwin") for s in SPELLINGS],
}
