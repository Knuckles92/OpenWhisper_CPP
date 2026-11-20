"""Curated inputs for services/_hotkey_keyboard.py (M1.5 hotkeys): the Windows grammar and
display, and traces of HotkeyManager._handle_keyboard_event.

A trace runs a copy of the backend loaded with a stub `keyboard` module, exactly as
OpenWhisper's tests load it (tests/test_push_hold_recording.py), so nothing here installs a hook:
the real `keyboard.is_pressed` would start the package's listener. The stub answers is_pressed
from the step's "held" modifiers; virtual-key input (a negative scan code) reads the step's
"native" modifiers in place of the OS state; time.monotonic is the step's "t"; and the callbacks
a press would start on a thread of their own run inline, so their order is recorded.

A trace is [hotkey overrides, steps]; each step is one of
    {"op": "key", "type": "down"|"up", "name", "scan" (or null), "keypad", "t", "held",
     "native" (or null)}
    {"op": "mode", "mode"}, {"op": "enabled", "value"}, {"op": "suspend", "value"},
    {"op": "dynamic", "family", "items": [[id, hotkey], ...]}, {"op": "update", "hotkeys"},
    {"op": "rehook"}, {"op": "hold_active", "now"}
and the result has one entry per step: a key step gives [passes, [callbacks fired]], a
hold_active step its answer, the others null. The C++ replay is tests/hotkeys/test_golden.cpp.
"""

import importlib.util
import sys
import time
import types
from pathlib import Path
from unittest.mock import patch

_CASES_DIR = Path(__file__).resolve().parent
_spec = importlib.util.spec_from_file_location(
    "cases_hotkey_common_spellings", _CASES_DIR / "services._hotkey_common.py")
_common = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(_common)
SPELLINGS = _common.SPELLINGS


class _FakeMouseHook:
    def __init__(self, claims, on_button):
        self.running = False

    def start(self):
        self.running = True

    def stop(self):
        self.running = False


def _stub_backend(held):
    import services._hotkey_keyboard as real

    keyboard_stub = types.SimpleNamespace(
        KEY_DOWN="down",
        KEY_UP="up",
        hook=lambda *args, **kwargs: None,
        is_pressed=lambda name: name in held,
        unhook_all=lambda: None,
    )
    spec = importlib.util.spec_from_file_location("golden_hotkey_keyboard", real.__file__)
    module = importlib.util.module_from_spec(spec)
    with patch.dict(sys.modules, {"keyboard": keyboard_stub}):
        spec.loader.exec_module(module)
    module.MouseButtonHook = _FakeMouseHook
    module._spawn = lambda callback, *args: callback(*args)
    return module


def _trace(module, overrides, steps):
    from config import config
    from services import synthetic_keys

    held = set()
    native = [None]
    backend = _stub_backend(held)
    hotkeys = {**config.DEFAULT_HOTKEYS, **overrides}
    manager = backend.HotkeyManager(hotkeys)
    fired = []

    def stamped(name):
        return lambda at: fired.append(f"{name}@{at!r}")

    def plain(name):
        return lambda: fired.append(name)

    manager.set_callbacks(
        on_record_toggle=plain("record_toggle"),
        on_record_press=stamped("record_press"),
        on_record_release=stamped("record_release"),
        on_command_press=stamped("command_press"),
        on_command_release=stamped("command_release"),
        on_cancel=plain("cancel"),
        on_minimize_tray=plain("minimize_tray"),
        on_meeting_toggle=plain("meeting_toggle"),
        on_scratchpad_toggle=plain("scratchpad_toggle"),
        on_cycle_language=plain("cycle_language"),
        on_paste_last_original=plain("paste_last_original"),
        on_route_record=plain("route_record"),
        on_walkthrough=plain("walkthrough"),
        on_screenshot=plain("screenshot"),
        on_paste_walkthrough=plain("paste_walkthrough"),
        on_status_update_auto_hide=lambda status: fired.append(f"status:{status}"),
    )

    def native_modifiers():
        return None if native[0] is None else frozenset(native[0])

    results = []
    with patch.object(synthetic_keys, "native_windows_modifiers", native_modifiers):
        for step in steps:
            op = step["op"]
            if op == "key":
                held.clear()
                held.update(step.get("held", []))
                native[0] = step.get("native")
                event = types.SimpleNamespace(
                    event_type=step["type"], name=step["name"], is_keypad=step["keypad"])
                if step.get("scan") is not None:
                    event.scan_code = step["scan"]
                fired.clear()
                with patch.object(time, "monotonic", return_value=step["t"]):
                    passes = manager._handle_keyboard_event(event)
                results.append([passes, list(fired)])
            elif op == "mode":
                manager.set_record_mode(step["mode"])
                results.append(None)
            elif op == "enabled":
                manager.program_enabled = step["value"]
                results.append(None)
            elif op == "suspend":
                manager.set_capture_suspended(step["value"])
                results.append(None)
            elif op == "dynamic":
                family = step["family"]
                manager.set_dynamic_hotkeys(
                    family, dict(step["items"]),
                    lambda item_id, family=family: fired.append(f"{family}:{item_id}"))
                results.append(None)
            elif op == "update":
                manager.update_hotkeys(step["hotkeys"])
                results.append(None)
            elif op == "rehook":
                manager.rehook()
                results.append(None)
            elif op == "hold_active":
                results.append(manager.keyboard_hold_active(step["now"]))
            else:
                raise ValueError(op)
    return results


def _parse(module, hotkey):
    modifiers, main_key = module.parse_hotkey(hotkey)
    return [sorted(modifiers), main_key]


def _format(module, modifiers, main_key):
    return module.format_hotkey(frozenset(modifiers), main_key)


WRAPPERS = {
    "parse_hotkey": _parse,
    "format_hotkey": _format,
    "handle_keyboard_event": _trace,
}


def key(kind, name, t, scan=None, keypad=False, held=(), native=None):
    return {"op": "key", "type": kind, "name": name, "scan": scan, "keypad": keypad, "t": t,
            "held": list(held), "native": native}


def tap(name, t, scan=None, keypad=False, held=(), up_name=None, up_held=None, native=None):
    return [key("down", name, t, scan, keypad, held, native),
            key("up", up_name or name, t + 0.08, scan, keypad,
                held if up_held is None else up_held, native)]


CTRL_ALT = ("ctrl", "alt")
NEW_ACTIONS = {"command_mode": "ctrl+alt+k", "scratchpad_toggle": "ctrl+alt+s",
               "cycle_language": "ctrl+alt+l", "paste_last_original": "ctrl+alt+o"}

TRACES = [
    # Toggle: the numpad star claims down and up; the main-row star passes.
    [{}, [*tap("*", 1.0, 55, True), *tap("*", 1.2, 9, False, ("shift",)),
          *tap("*", 1.25, 55, True), *tap("*", 1.6, 55, True)]],
    # Push-and-hold, with repeats and a release after the modifiers.
    [{"record_toggle": "ctrl+alt+r"},
     [{"op": "mode", "mode": "push_hold"}, key("down", "r", 1.0, 19, held=CTRL_ALT),
      key("down", "r", 1.5, 19, held=CTRL_ALT), key("down", "r", 1.53, 19),
      key("up", "r", 2.0, 19), key("up", "r", 2.1, 19)]],
    # Enable/disable while disabled, the gate, and a release that survives disabling.
    [NEW_ACTIONS,
     [key("down", "k", 1.0, 37, held=CTRL_ALT), {"op": "enabled", "value": False},
      key("up", "k", 1.1, 37), *tap("*", 1.2, 55, True),
      key("down", "*", 1.4, 55, True, CTRL_ALT), key("down", "*", 1.45, 55, True, CTRL_ALT),
      key("up", "*", 1.5, 55, True), *tap("*", 1.6, 55, True),
      key("down", "*", 2.0, 55, True, CTRL_ALT), key("up", "*", 2.1, 55, True)]],
    # Press actions in table order, an unbound key, and once per physical press.
    [NEW_ACTIONS,
     [*tap("s", 1.0, 31, held=CTRL_ALT), *tap("l", 1.2, 38, held=CTRL_ALT),
      *tap("q", 1.4, 16, held=CTRL_ALT), key("down", "o", 2.0, 24, held=CTRL_ALT),
      key("down", "o", 2.03, 24, held=CTRL_ALT), key("down", "o", 2.06, 24, held=CTRL_ALT),
      key("up", "o", 2.1, 24, held=CTRL_ALT), key("down", "o", 2.2, 24, held=CTRL_ALT),
      *tap("-", 3.0, 74, True), *tap("m", 3.2, 50, held=CTRL_ALT)]],
    # Shift symbols named "!" down and "1" up, and their repeats.
    [{"scratchpad_toggle": "ctrl+shift+!"},
     [key("down", "ctrl", 1.0, 29), key("down", "shift", 1.01, 42, held=("ctrl",)),
      key("down", "!", 1.1, 2, held=("ctrl", "shift")),
      key("down", "!", 1.15, 2, held=("ctrl", "shift")),
      key("up", "shift", 1.2, 42, held=("ctrl", "shift")),
      key("down", "1", 1.25, 2, held=("ctrl",)), key("up", "1", 1.3, 2, held=("ctrl",)),
      key("up", "ctrl", 1.31, 29, held=("ctrl",)), key("down", "!", 1.5, 2, held=("shift",)),
      key("up", "1", 1.6, 2)]],
    # Missed key-ups behind an elevated window, slow repeats and the repeat window's edge.
    [NEW_ACTIONS,
     [key("down", "m", 1.0, 50, held=CTRL_ALT), key("down", "m", 6.0, 50),
      key("up", "m", 6.1, 50), key("down", "o", 7.0, 24, held=CTRL_ALT),
      key("down", "o", 9.0, 24), key("down", "o", 11.5, 24), key("down", "o", 14.0000001, 24),
      {"op": "hold_active", "now": 14.5}]],
    # Command Mode on Ctrl+Num* beside record on Num*: repeats stay with Command Mode.
    [{"record_toggle": "kp *", "command_mode": "ctrl+kp *"},
     [key("down", "*", 1.0, 55, True, ("ctrl",)), key("down", "*", 1.03, 55, True, ("ctrl",)),
      key("down", "*", 1.06, 55, True), {"op": "hold_active", "now": 1.07},
      {"op": "hold_active", "now": 3.7}, key("up", "*", 1.1, 55, True),
      {"op": "hold_active", "now": 1.11}]],
    # Numpad and main-row keys with one scan code; events with no scan code.
    [{"record_toggle": "kp /"},
     [{"op": "mode", "mode": "push_hold"}, key("down", "/", 1.0, 53, True),
      key("up", "/", 1.1, 53, False), key("up", "/", 1.2, 53, True),
      key("down", "/", 1.3, None, True), key("down", "/", 1.31, None, True),
      key("up", "/", 1.4, None, True), {"op": "hold_active", "now": 1.5}]],
    # Families beside profiles; debounce per item; a family emptied; record toggle debounce.
    [{},
     [{"op": "dynamic", "family": "profile", "items": [["ticket", "ctrl+alt+t"]]},
      {"op": "dynamic", "family": "transform",
       "items": [["polish", "ctrl+alt+p"], ["short", "ctrl+alt+t"]]},
      *tap("p", 1.0, 25, held=CTRL_ALT), *tap("p", 1.2, 25, held=CTRL_ALT),
      *tap("p", 1.5, 25, held=CTRL_ALT), *tap("t", 2.0, 20, held=CTRL_ALT),
      {"op": "dynamic", "family": "transform", "items": []},
      *tap("p", 3.0, 25, held=CTRL_ALT), *tap("*", 4.0, 55, True),
      *tap("*", 4.2, 55, True), *tap("*", 4.4, 55, True)]],
    # Virtual-key input: the OS state decides its modifiers.
    [{"record_toggle": "ctrl+alt+r"},
     [key("down", "r", 1.0, -82, held=(), native=["ctrl", "alt"]),
      key("up", "r", 1.05, -82, native=["ctrl", "alt"]),
      key("down", "r", 1.2, -82, held=CTRL_ALT, native=[]),
      key("down", "r", 1.4, -82, held=CTRL_ALT, native=None),
      key("up", "r", 1.5, -82), *tap("f9", 2.0, -120, native=["windows"])]],
    # The Windows key as keyboard.get_hotkey_name spells it.
    [{"record_toggle": "windows+f9"}, [*tap("f9", 1.0, 67, held=("win",))]],
    # Capture suspension, mode changes and rehooks forget holds.
    [NEW_ACTIONS,
     [{"op": "mode", "mode": "push_hold"}, key("down", "*", 1.0, 55, True),
      {"op": "rehook"}, key("down", "*", 1.05, 55, True), key("up", "*", 1.1, 55, True),
      key("down", "*", 1.2, 55, True), {"op": "suspend", "value": True},
      key("up", "*", 1.3, 55, True), *tap("s", 1.4, 31, held=CTRL_ALT),
      {"op": "suspend", "value": False}, key("down", "k", 1.6, 37, held=CTRL_ALT),
      {"op": "mode", "mode": "toggle"}, key("up", "k", 1.7, 37),
      {"op": "update", "hotkeys": {"command_mode": "ctrl+alt+j"}},
      *tap("k", 1.8, 37, held=CTRL_ALT), *tap("j", 2.0, 36, held=CTRL_ALT)]],
    # Default hotkeys only (the stub's minimal set) and keys with no name.
    [{"command_mode": ""},
     [*tap("-", 1.0, 74, True), *tap("x", 1.2, 45), key("down", None, 1.4, 0),
      key("up", None, 1.5, 0), *tap("enter", 1.6, 28, True), *tap("enter", 1.8, 28, False)]],
]

CASES = {
    "parse_hotkey": [[s] for s in SPELLINGS],
    "format_hotkey": [[[], "kp *"], [["alt", "ctrl"], "kp *"], [["win", "shift"], "f9"],
                      [["cmd"], "x"], [["ctrl"], None], [[], ""]],
    "format_hotkey_display": [[s] for s in SPELLINGS],
    "handle_keyboard_event": TRACES,
}


# M2.6 hotkeys-mouse-capture: side buttons through _claims_mouse_button and _on_mouse_button.
#
# A mouse trace is [hotkey overrides, steps] over the same stub backend, with the mouse hook a
# fake that only remembers whether it runs. Steps are the key trace's ops plus
#     {"op": "claims", "button", "modifiers"}                 what the hook would ask
#     {"op": "mouse", "button", "pressed", "modifiers", "t"}  _on_mouse_button
#     {"op": "hook"}                                          whether the mouse hook runs
#     {"op": "held"}         [record held, command held, sorted press and family holds]
#     {"op": "cleanup"}
# and the result has one entry per step: claims and hook their answer, mouse the callbacks
# fired, held its triple, a key step [passes, fired], hold_active its answer; the others null.
# The C++ replay is tests/hotkeys/test_golden_mouse.cpp.


def _mouse_trace(module, overrides, steps):
    from config import config
    from services import synthetic_keys

    held = set()
    backend = _stub_backend(held)
    manager = backend.HotkeyManager({**config.DEFAULT_HOTKEYS, **overrides})
    fired = []

    def stamped(name):
        return lambda at: fired.append(f"{name}@{at!r}")

    def plain(name):
        return lambda: fired.append(name)

    manager.set_callbacks(
        on_record_toggle=plain("record_toggle"),
        on_record_press=stamped("record_press"),
        on_record_release=stamped("record_release"),
        on_command_press=stamped("command_press"),
        on_command_release=stamped("command_release"),
        on_cancel=plain("cancel"),
        on_minimize_tray=plain("minimize_tray"),
        on_meeting_toggle=plain("meeting_toggle"),
        on_scratchpad_toggle=plain("scratchpad_toggle"),
        on_cycle_language=plain("cycle_language"),
        on_paste_last_original=plain("paste_last_original"),
        on_route_record=plain("route_record"),
        on_walkthrough=plain("walkthrough"),
        on_screenshot=plain("screenshot"),
        on_paste_walkthrough=plain("paste_walkthrough"),
        on_status_update_auto_hide=lambda status: fired.append(f"status:{status}"),
    )
    results = []
    with patch.object(synthetic_keys, "native_windows_modifiers", lambda: None):
        for step in steps:
            op = step["op"]
            if op == "claims":
                results.append(manager._claims_mouse_button(
                    step["button"], frozenset(step["modifiers"])))
            elif op == "mouse":
                fired.clear()
                # Debouncers read time.monotonic, the event its hook time stamp; both are "t".
                with patch.object(time, "monotonic", return_value=step["t"]):
                    manager._on_mouse_button(step["button"], step["pressed"],
                                             frozenset(step["modifiers"]), step["t"])
                results.append(list(fired))
            elif op == "hook":
                results.append(manager._mouse_hook.running)
            elif op == "held":
                results.append([manager._record_key_held, manager._command_key_held,
                                sorted([*manager._press_held,
                                        *(f"{ns}:{item}" for ns, item in manager._dynamic_held)])])
            elif op == "cleanup":
                manager.cleanup()
                results.append(None)
            elif op == "key":
                held.clear()
                held.update(step.get("held", []))
                event = types.SimpleNamespace(
                    event_type=step["type"], name=step["name"], is_keypad=step["keypad"])
                if step.get("scan") is not None:
                    event.scan_code = step["scan"]
                fired.clear()
                with patch.object(time, "monotonic", return_value=step["t"]):
                    passes = manager._handle_keyboard_event(event)
                results.append([passes, list(fired)])
            elif op == "mode":
                manager.set_record_mode(step["mode"])
                results.append(None)
            elif op == "enabled":
                manager.program_enabled = step["value"]
                results.append(None)
            elif op == "suspend":
                manager.set_capture_suspended(step["value"])
                results.append(None)
            elif op == "dynamic":
                family = step["family"]
                manager.set_dynamic_hotkeys(
                    family, dict(step["items"]),
                    lambda item_id, family=family: fired.append(f"{family}:{item_id}"))
                results.append(None)
            elif op == "update":
                manager.update_hotkeys(step["hotkeys"])
                results.append(None)
            elif op == "rehook":
                manager.rehook()
                results.append(None)
            elif op == "hold_active":
                results.append(manager.keyboard_hold_active(step["now"]))
            else:
                raise ValueError(op)
    return results


def claims(button, modifiers=()):
    return {"op": "claims", "button": button, "modifiers": list(modifiers)}


def mouse(button, pressed, t, modifiers=()):
    return {"op": "mouse", "button": button, "pressed": pressed, "modifiers": list(modifiers),
            "t": t}


def side_click(button, t, modifiers=()):
    return [mouse(button, True, t, modifiers), mouse(button, False, t + 0.1)]


HOOK = {"op": "hook"}
HELD = {"op": "held"}

MOUSE_TRACES = [
    # The hook runs only while a side button is bound and capture is live.
    [{}, [HOOK, {"op": "update", "hotkeys": {"scratchpad_toggle": "mouse4"}}, HOOK,
          {"op": "suspend", "value": True}, HOOK, {"op": "suspend", "value": False}, HOOK,
          {"op": "rehook"}, HOOK, {"op": "update", "hotkeys": {"scratchpad_toggle": ""}}, HOOK,
          {"op": "dynamic", "family": "transform", "items": [["polish", "ctrl+mouse5"]]}, HOOK,
          {"op": "dynamic", "family": "transform", "items": []}, HOOK,
          {"op": "dynamic", "family": "profile", "items": [["ticket", "mouse4"]]}, HOOK,
          {"op": "cleanup"}, HOOK]],
    # Claims follow the exact modifier set, the bindings and the enabled state.
    [{"record_toggle": "mouse4", "enable_disable": "ctrl+mouse5"},
     [claims("mouse4"), claims("mouse4", ["ctrl"]), claims("mouse5"), claims("mouse5", ["ctrl"]),
      {"op": "enabled", "value": False}, claims("mouse4"), claims("mouse5", ["ctrl"]),
      {"op": "enabled", "value": True}, {"op": "suspend", "value": True}, claims("mouse4"),
      claims("mouse5", ["ctrl"]), {"op": "suspend", "value": False}, claims("mouse4")]],
    # Every stored action is claimed, served or not, and family items too.
    [{"some_future_action": "alt+mouse4", "minimize_tray": "shift+mouse5"},
     [claims("mouse4", ["alt"]), claims("mouse5", ["shift"]), claims("mouse5"),
      {"op": "dynamic", "family": "action", "items": [["open", "ctrl+shift+mouse4"]]},
      claims("mouse4", ["ctrl", "shift"]), claims("mouse4", ["ctrl"]),
      *side_click("mouse4", 1.0, ["alt"]), *side_click("mouse5", 2.0, ["shift"]),
      *side_click("mouse4", 3.0, ["ctrl", "shift"]), HELD]],
    # Push-and-hold on a side button: repeats of the press are absorbed.
    [{"record_toggle": "mouse4"},
     [{"op": "mode", "mode": "push_hold"}, mouse("mouse4", True, 3.0),
      mouse("mouse4", True, 3.05), HELD, mouse("mouse4", False, 3.4), HELD,
      mouse("mouse4", False, 3.5)]],
    # Toggle mode and its debounce, and a press with modifiers that matches nothing.
    [{"record_toggle": "mouse4"},
     [*side_click("mouse4", 1.0), *side_click("mouse4", 1.2), *side_click("mouse4", 1.6),
      mouse("mouse4", True, 2.0, ["ctrl"]), HELD, mouse("mouse4", False, 2.1)]],
    # A side-button hold survives a rehook and an update, not a capture suspension.
    [{"record_toggle": "mouse4", "command_mode": "ctrl+mouse5"},
     [{"op": "mode", "mode": "push_hold"}, mouse("mouse4", True, 3.0), {"op": "rehook"}, HELD,
      mouse("mouse4", False, 9.0), mouse("mouse5", True, 10.0, ["ctrl"]),
      {"op": "update", "hotkeys": {"cancel": "kp -"}}, HELD, mouse("mouse5", False, 11.0),
      mouse("mouse4", True, 12.0), {"op": "suspend", "value": True}, HELD,
      mouse("mouse4", True, 12.5), {"op": "suspend", "value": False}, HELD,
      mouse("mouse4", False, 13.0)]],
    # Enable/disable on a side button: once per click, claimed while disabled, the gate.
    [{"enable_disable": "mouse4", "record_toggle": "mouse5"},
     [*side_click("mouse4", 0.0), claims("mouse5"), claims("mouse4"), *side_click("mouse5", 0.5),
      *side_click("mouse4", 1.0), HELD, *side_click("mouse5", 1.5)]],
    # A release ends every hold on its button and survives disabling; a key does not end it.
    [{"record_toggle": "mouse4", "command_mode": "mouse4", "scratchpad_toggle": "alt+mouse4"},
     [{"op": "mode", "mode": "push_hold"}, mouse("mouse4", True, 1.0),
      mouse("mouse4", True, 1.1, ["alt"]), HELD, *tap("*", 1.2, 55, True),
      key("up", "mouse4", 1.25), HELD, {"op": "enabled", "value": False},
      mouse("mouse4", False, 1.3), HELD, {"op": "hold_active", "now": 1.31}]],
    # A side-button hold leaves the keyboard hold check alone; keys still work beside it.
    [{"record_toggle": "mouse4", "cancel": "kp -"},
     [{"op": "mode", "mode": "push_hold"}, mouse("mouse4", True, 5.0),
      {"op": "hold_active", "now": 5.1}, *tap("-", 5.2, 74, True),
      {"op": "hold_active", "now": 5.3}, mouse("mouse4", False, 5.4)]],
    # Families on side buttons: per-item debounce, and a family emptied mid-hold.
    [{},
     [{"op": "dynamic", "family": "profile", "items": [["ticket", "mouse5"]]},
      {"op": "dynamic", "family": "transform", "items": [["polish", "mouse5"]]},
      *side_click("mouse5", 1.0), *side_click("mouse5", 1.2), *side_click("mouse5", 1.5),
      mouse("mouse5", True, 2.0), {"op": "dynamic", "family": "profile", "items": []}, HELD,
      mouse("mouse5", False, 2.1), HELD]],
]

WRAPPERS["mouse_button"] = _mouse_trace
CASES["mouse_button"] = MOUSE_TRACES
