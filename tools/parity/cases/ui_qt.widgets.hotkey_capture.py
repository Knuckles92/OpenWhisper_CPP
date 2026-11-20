"""Curated inputs for ui_qt/widgets/hotkey_capture.py (M2.6 hotkeys-mouse-capture): the side
button notes, the Escape test and the Windows capture's result.

mouse_shortcut_note reads the backend the platform loaded; the wrapper pins it per case, as
OpenWhisper's test does ("windows": the `keyboard` backend; "pynput": the macOS and Linux one,
with that backend's parse_hotkey). OpenWhisper_CPP runs the Windows rows; the pynput rows are recorded
for completeness and replayed through Vocabulary::Pynput.

capture_run runs HotkeyCaptureThread.run (the Windows variant) synchronously over scripted
`keyboard` events, with a stub `keyboard` whose hook hands nothing on: the events are queued
before the run, then a None ends a run that saw no key-up. get_hotkey_name is the real one, which
only normalizes the names it is given and installs nothing. An event is [type, name or null,
is_keypad]; the result is {"captured": [...], "cancelled": n, "failed": [...]}. The C++ replay is
tests/hotkeys/test_golden_mouse.cpp.

M2.17 hotkeys-page adds qt_modifier_names over modifier sets and platforms (OpenWhisper_CPP replays the
win32 rows), and the Hotkeys page's standard row table and record-row descriptions, generated
from settings_dialog.py (hotkey_rows, record_hotkey_description); the C++ replay is
tests/ui_hotkeys/test_golden.cpp.
"""

import importlib
import types
from unittest.mock import patch


def _note(module, hotkey, backend):
    if backend == "windows":
        with patch.object(module, "USE_PYNPUT_BACKEND", False):
            return module.mouse_shortcut_note(hotkey)
    pynput = importlib.import_module("services._hotkey_pynput")
    with patch.object(module, "USE_PYNPUT_BACKEND", True), \
            patch.object(module, "parse_hotkey", pynput.parse_hotkey):
        return module.mouse_shortcut_note(hotkey)


def _capture_run(module, events):
    import keyboard

    stub = types.SimpleNamespace(
        KEY_UP="up", hook=lambda callback, suppress: "capture-hook",
        unhook=lambda _hook: None, get_hotkey_name=keyboard.get_hotkey_name,
    )
    result = {"captured": [], "cancelled": 0, "failed": []}
    with patch.object(module, "keyboard", stub):
        thread = module.HotkeyCaptureThread()
        thread.captured.connect(result["captured"].append)
        thread.cancelled.connect(lambda: result.__setitem__("cancelled", result["cancelled"] + 1))
        thread.failed.connect(result["failed"].append)
        for kind, name, keypad in events:
            thread._events.put(types.SimpleNamespace(event_type=kind, name=name, is_keypad=keypad))
        thread._events.put(None)
        thread.run()
    return result


def _qt_modifier_names(module, names, platform):
    """qt_modifier_names over Qt modifier flags named "control", "meta", "alt", "shift" and
    "keypad", with sys.platform pinned for the call; the frozenset comes back sorted."""
    import types

    from PyQt6.QtCore import Qt

    flags = Qt.KeyboardModifier.NoModifier
    for name in names:
        flags |= {
            "control": Qt.KeyboardModifier.ControlModifier,
            "meta": Qt.KeyboardModifier.MetaModifier,
            "alt": Qt.KeyboardModifier.AltModifier,
            "shift": Qt.KeyboardModifier.ShiftModifier,
            "keypad": Qt.KeyboardModifier.KeypadModifier,
        }[name]
    with patch.object(module, "sys", types.SimpleNamespace(platform=platform)):
        return sorted(module.qt_modifier_names(flags))


def _settings_dialog():
    return importlib.import_module("ui_qt.dialogs.settings_dialog")


def _record_hotkey_description(module, mode, hands_free):
    """M2.17: SettingsDialog._record_hotkey_description, the record row's text by trigger mode."""
    return _settings_dialog().SettingsDialog._record_hotkey_description(mode, hands_free)


def _hotkey_rows(module):
    """M2.17: the standard rows of Settings > Hotkeys, read from settings_dialog.py's own source
    (_build_hotkeys_page, with _add_walkthrough_shortcut_rows in place), in page order: each
    group heading as {"group"}, the record-mode card as {"card"}, each row as {"key", "title",
    "description", "required"}. The record row's description is the one the page builds it
    with (toggle mode)."""
    import ast
    import inspect

    dialog_module = _settings_dialog()
    dialog = dialog_module.SettingsDialog
    rows = []

    def literal(node):
        if isinstance(node, ast.Call) and getattr(node.func, "attr", "") == \
                "_record_hotkey_description":
            mode = ast.literal_eval(node.args[0]) if isinstance(node.args[0], ast.Constant) \
                else getattr(dialog_module.RecordingTriggerMode, node.args[0].attr)
            return dialog._record_hotkey_description(mode)
        return ast.literal_eval(node)

    def walk(function):
        tree = ast.parse(inspect.cleandoc("\n" + inspect.getsource(function)))
        calls = [node for node in ast.walk(tree) if isinstance(node, ast.Call)
                 and isinstance(node.func, ast.Attribute)]
        for call in sorted(calls, key=lambda node: (node.lineno, node.col_offset)):
            name = call.func.attr
            if name == "_hotkey_group_title":
                rows.append({"group": literal(call.args[0])})
            elif name == "_build_recording_trigger_mode_row":
                rows.append({"card": "record_mode"})
            elif name == "_add_walkthrough_shortcut_rows":
                walk(dialog._add_walkthrough_shortcut_rows)
            elif name == "_hotkey_shortcut_row":
                keywords = {k.arg: k.value for k in call.keywords}
                optional = keywords.get("optional")
                rows.append({
                    "key": literal(call.args[0]),
                    "title": literal(call.args[1]),
                    "description": literal(call.args[2]),
                    "required": not (optional is not None and ast.literal_eval(optional)),
                })

    walk(dialog._build_hotkeys_page)
    return rows


WRAPPERS = {"mouse_shortcut_note": _note, "capture_run": _capture_run,
            "qt_modifier_names": _qt_modifier_names,
            "record_hotkey_description": _record_hotkey_description,
            "hotkey_rows": _hotkey_rows}

MODIFIER_SETS = [[], ["control"], ["meta"], ["alt"], ["shift"], ["keypad"],
                 ["control", "meta"], ["control", "alt", "shift"],
                 ["control", "meta", "alt", "shift"], ["meta", "shift"], ["alt", "keypad"],
                 ["control", "meta", "alt", "shift", "keypad"]]

NOTE_HOTKEYS = [
    "mouse4", "mouse5", "ctrl+mouse4", "alt+mouse5", "ctrl+shift+mouse4", "win+mouse5",
    "windows+mouse4", "cmd+mouse4", "super+mouse5", "Mouse4", " mouse5 ", "MOUSE5",
    "foo+mouse4", "ctrl+r", "kp *", "mouse6", "mouse", "", "+", "mouse4+ctrl",
]


def down(name, keypad=False):
    return ["down", name, keypad]


def up(name, keypad=False):
    return ["up", name, keypad]


CAPTURES = [
    [[down("windows"), down("f9"), up("f9")]],
    [[down("esc"), up("esc")]],
    [[down("escape"), up("escape")]],
    [[down("ctrl"), down("alt"), down("esc"), up("esc")]],
    [[down("ctrl"), down("alt"), down("*", True), up("*", True)]],
    [[down("*", True), up("*", True)]],
    [[down("-", True), up("-", True)]],
    [[down("+", True), up("+", True)]],
    [[down("enter", True), up("enter", True)]],
    [[down("5", True), up("5", True)]],
    [[down("ctrl"), down("+"), up("+")]],
    [[down("left ctrl"), down("right shift"), down("a"), up("a")]],
    [[down("shift"), down("!"), up("1")]],
    [[down("ctrl"), up("ctrl")]],
    [[down("shift"), down("ctrl"), up("shift")]],
    [[down("a"), down("b"), up("b")]],
    [[down("page up"), up("page up")]],
    [[down("ctrl"), down("Page_Down"), up("Page_Down")]],
    [[down("alt gr"), down("e"), up("e")]],
    [[down("ctrl"), down("ctrl"), down("ctrl"), down("k"), up("k")]],
    [[down("windows"), down("left windows"), down("l"), up("l")]],
    [[down("f13"), up("f13")]],
    [[down("ctrl"), down(None), up(None)]],
    [[down(None, True), up(None, True)]],
    [[down("esc"), down("esc"), up("esc")]],
    [[down("esc"), up("x")]],
    [[down("ctrl"), down("alt")]],
    [[]],
    [[down("A"), up("A")]],
    [[down("ctrl"), down("É"), up("É")]],
    [[down("caps lock"), up("caps lock")]],
    [[down("ctrl"), down("print screen"), up("print screen")]],
]

CASES = {
    "mouse_shortcut_note": [[h, b] for b in ("windows", "pynput") for h in NOTE_HOTKEYS],
    "is_escape_key": [["esc"], ["Escape"], ["ESC"], [" esc "], ["\tescape\n"], ["f8"], [""],
                      [None], [" "], ["escape key"], ["kp esc"], [" esc"]],
    "capture_run": CAPTURES,
    "qt_modifier_names": [[names, platform] for platform in ("win32", "darwin", "linux")
                          for names in MODIFIER_SETS],
    "record_hotkey_description": [[mode, hands_free] for mode in ("toggle", "push_hold", "other")
                                  for hands_free in (False, True)],
    "hotkey_rows": [[]],
}
