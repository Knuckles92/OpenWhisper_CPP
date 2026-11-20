"""Curated inputs for services/_hotkey_common.py (M1.5 hotkeys): the shortcut grammar shared by
both backends, run with each backend's alias tables, and the Debouncer.

parse_hotkey_string returns a frozenset, which the vector format cannot hold, so the wrapper
records the modifiers sorted. The C++ replay is tests/hotkeys/test_golden.cpp.
"""

import importlib
import time
from unittest.mock import patch

#: Every spelling the tests, the defaults and the capture fields write, and the edges.
SPELLINGS = [
    "kp *", "kp -", "kp /", "kp 5", "kp enter", "kp_*", "ctrl+alt+kp *", "ctrl+alt+kp_-",
    "ctrl+alt+m", "ctrl+shift+f12", "ctrl+alt+r", "ctrl+alt+escape", "ctrl+alt+shift+r",
    "page up", "page down", "page_up", "pgup", "pgdn", "mouse4", "mouse5", "ctrl+mouse4",
    "ctrl+shift+mouse4", "alt+mouse5", "win+f9", "windows+f9", "super+f9", "cmd+f9", "meta+f9",
    "command+k", "option+k", "opt+k", "control+shift+a", "Ctrl+Alt+R", " ctrl + alt + r ",
    "ctrl++", "+", "", " ", "ctrl+", "ctrl", "a", "A", "f1", "esc", "escape", "return", "enter",
    "del", "ins", "space", "tab", "backspace", "delete", "shift+!", "ctrl+shift+!", "ctrl+1",
    "hyper+x", "ctrl+alt+shift+win+x", "alt+shift+ctrl+x", "ctrl+ctrl+x", "ß",
    "ctrl+é", "print_screen", "caps_lock", "vk65", "ÉCLAIR", "ctrl+alt+k",
    "ctrl+alt+s", "ctrl+alt+l", "ctrl+alt+o", "ctrl+shift+f9", "ctrl+alt+p", "f13",
]

VOCABULARIES = ("windows", "pynput")


def _backend(vocabulary):
    if vocabulary == "windows":
        module = importlib.import_module("services._hotkey_keyboard")
        return module._MODIFIER_ALIASES, None, module._ALL_MODIFIERS
    module = importlib.import_module("services._hotkey_pynput")
    return module._MODIFIER_ALIASES, module._MAIN_KEY_ALIASES, module._ALL_MODIFIERS


def _parse(module, hotkey, vocabulary):
    modifier_aliases, main_key_aliases, _order = _backend(vocabulary)
    modifiers, main_key = module.parse_hotkey_string(hotkey, modifier_aliases, main_key_aliases)
    return [sorted(modifiers), main_key]


def _format(module, modifiers, main_key, vocabulary):
    return module.format_hotkey_string(frozenset(modifiers), main_key, _backend(vocabulary)[2])


def _debouncer(module, interval_ms, times):
    """should_trigger at each time (None: reset() instead)."""
    debouncer = module.Debouncer(interval_ms)
    results = []
    for now in times:
        if now is None:
            debouncer.reset()
            results.append(None)
            continue
        with patch.object(time, "monotonic", return_value=now):
            results.append(debouncer.should_trigger())
    return results


WRAPPERS = {
    "parse_hotkey_string": _parse,
    "format_hotkey_string": _format,
    "Debouncer": _debouncer,
}

CASES = {
    "parse_hotkey_string": [[s, v] for v in VOCABULARIES for s in SPELLINGS],
    "format_hotkey_string": [
        [mods, main, v] for v in VOCABULARIES for mods, main in [
            [[], "kp *"], [["ctrl", "alt"], "kp *"], [["alt", "ctrl"], "m"],
            [["win", "shift", "alt", "ctrl"], "x"], [["cmd", "shift", "alt", "ctrl"], "x"],
            [["ctrl"], None], [["ctrl"], ""], [[], None], [["bogus", "ctrl"], "a"],
        ]
    ],
    "is_mouse_key": [["mouse4"], ["mouse5"], ["m"], ["mouse6"], [None], [""]],
    "Debouncer": [
        [300, [100.0, 100.05, 100.4]],
        [300, [100.0, 100.3, 100.30000001, 100.6]],
        [300, [100.0, None, 100.01, 100.02]],
        [300, [5.0, 4.0, 5.31]],
    ],
}
