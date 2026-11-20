"""Curated inputs for services/_mouse_hook_win.py (M2.6 hotkeys-mouse-capture): the hook
callback's decisions (MouseButtonHook._make_callback) over scripted hook calls.

Nothing here installs a hook. The callback is built over a fake user32, as OpenWhisper's tests
build it (tests/test_personalize_s6_backends.py _HookHarness): CallNextHookEx answers 0, and
GetAsyncKeyState reports the step's "pressed" virtual keys down. Each hook call gets a real
MSLLHOOKSTRUCT, whose address is the lParam.

A trace is [claimed buttons, steps]; each step is one of
    {"op": "call", "code", "message", "xbutton", "flags", "pressed": [vk, ...]}
    {"op": "claimed", "buttons": [...]}   the claims answer changes (a binding goes away)
    {"op": "raise", "value": bool}        claims raises from now on (or stops raising)
    {"op": "restart"}                     a new installation: a fresh callback over the same hook
and the result has one entry per step: a call gives [returned, [claims asked], [events queued]],
an asked claim as [button, sorted modifiers] and a queued event as [button, pressed, sorted
modifiers] (its time stamp is left out); the other steps give null. The C++ replay is
tests/hotkeys/test_golden_mouse.cpp.
"""

import ctypes
import queue
from unittest.mock import Mock

WM_MOUSEMOVE = 0x0200
WM_LBUTTONDOWN = 0x0201
WM_LBUTTONUP = 0x0202
WM_MOUSEWHEEL = 0x020A
WM_XBUTTONDOWN = 0x020B
WM_XBUTTONUP = 0x020C
LLMHF_INJECTED = 0x01
LLMHF_LOWER_IL_INJECTED = 0x02
VK_SHIFT, VK_CONTROL, VK_MENU, VK_LWIN, VK_RWIN = 0x10, 0x11, 0x12, 0x5B, 0x5C


class _User32:
    def __init__(self, pressed):
        self.pressed = pressed

    def CallNextHookEx(self, hook, code, wparam, lparam):
        return 0

    def GetAsyncKeyState(self, key):
        return -32768 if key in self.pressed else 0


class _Kernel32:
    def GetCurrentThreadId(self):
        return 77

    def GetModuleHandleW(self, _name):
        return 1


def _callback_trace(module, claimed, steps):
    pressed = set()
    user32 = _User32(pressed)
    claimed = set(claimed)
    raising = [False]
    asked = []

    def claims(button, modifiers):
        asked.append([button, sorted(modifiers)])
        if raising[0]:
            raise RuntimeError("claims failed")
        return button in claimed

    hook = module.MouseButtonHook(
        claims, Mock(), libraries=(user32, _Kernel32(), lambda function: function))
    events = queue.Queue()
    callback = hook._make_callback(events)
    structs = []
    results = []
    for step in steps:
        op = step["op"]
        if op == "call":
            pressed.clear()
            pressed.update(step.get("pressed", []))
            info = module.MSLLHOOKSTRUCT()
            info.mouseData = step["xbutton"] << 16
            info.flags = step.get("flags", 0)
            structs.append(info)
            asked.clear()
            returned = callback(step.get("code", 0), step["message"], ctypes.addressof(info))
            queued = []
            while not events.empty():
                button, is_press, modifiers, _at = events.get_nowait()
                queued.append([button, is_press, sorted(modifiers)])
            results.append([returned, list(asked), queued])
        elif op == "claimed":
            claimed.clear()
            claimed.update(step["buttons"])
            results.append(None)
        elif op == "raise":
            raising[0] = step["value"]
            results.append(None)
        elif op == "restart":
            events = queue.Queue()
            callback = hook._make_callback(events)
            results.append(None)
        else:
            raise ValueError(op)
    return results


class _ClickUser32(_User32):
    """_User32 with the two calls observe_click makes: the window under a point is the step's
    "window" (0 for none), owned by this process, another one, or nobody (the step's "owner":
    "self", "other" or None, GetWindowThreadProcessId failing)."""

    def __init__(self, process):
        super().__init__(set())
        self.process = process
        self.window = 0
        self.owner = None
        self.asked = []

    def WindowFromPoint(self, point):
        self.asked.append([point.x, point.y])
        return self.window

    def GetWindowThreadProcessId(self, window, process_id):
        if self.owner is None:
            return 0
        process_id._obj.value = self.process if self.owner == "self" else self.process + 1
        return 1


def _click_trace(module, steps):
    """A walkthrough's hook (MouseButtonHook(lambda *_: False, ..., on_click=...)) over scripted
    calls. A call step is {"op": "call", "code", "message", "x", "y", "flags", "xbutton",
    "window", "owner"}; its result is [returned, [points asked of WindowFromPoint], [clicks
    queued as [x, y]]] (the time stamp is left out). {"op": "restart"} makes a fresh callback, as
    a new installation does, and gives null."""
    import os

    user32 = _ClickUser32(os.getpid())
    hook = module.MouseButtonHook(
        lambda *_: False, Mock(), on_click=Mock(),
        libraries=(user32, _Kernel32(), lambda function: function))
    events = queue.Queue()
    callback = hook._make_callback(events)
    structs = []
    results = []
    for step in steps:
        op = step["op"]
        if op == "call":
            user32.window = step.get("window", 0)
            user32.owner = step.get("owner")
            user32.asked = []
            info = module.MSLLHOOKSTRUCT()
            info.pt.x, info.pt.y = step["x"], step["y"]
            info.mouseData = step.get("xbutton", 0) << 16
            info.flags = step.get("flags", 0)
            structs.append(info)
            returned = callback(step.get("code", 0), step["message"], ctypes.addressof(info))
            queued = []
            while not events.empty():
                event = events.get_nowait()
                queued.append([event.x, event.y] if isinstance(event, module._Click)
                              else ["button", *event[:2]])
            results.append([returned, user32.asked, queued])
        elif op == "restart":
            events = queue.Queue()
            callback = hook._make_callback(events)
            results.append(None)
        else:
            raise ValueError(op)
    return results


WRAPPERS = {"callback_trace": _callback_trace, "click_trace": _click_trace}


def call(message, xbutton=1, flags=0, pressed=(), code=0):
    return {"op": "call", "code": code, "message": message, "xbutton": xbutton, "flags": flags,
            "pressed": list(pressed)}


def click(xbutton=1, flags=0, pressed=()):
    return [call(WM_XBUTTONDOWN, xbutton, flags, pressed), call(WM_XBUTTONUP, xbutton, flags)]


BOTH = ["mouse4", "mouse5"]

TRACES = [
    # Everything but the side buttons passes untouched and asks nothing.
    [BOTH, [call(WM_MOUSEMOVE), call(WM_LBUTTONDOWN), call(WM_LBUTTONUP), call(WM_MOUSEWHEEL),
            call(0x0204, 0), call(0x020D, 1)]],
    # A claimed press and its release are swallowed; a second release passes.
    [BOTH, [call(WM_XBUTTONDOWN, 2, pressed=[VK_CONTROL]), {"op": "claimed", "buttons": []},
            call(WM_XBUTTONUP, 2), call(WM_XBUTTONUP, 2)]],
    # Unclaimed, injected and unknown buttons reach the app; only real presses ask.
    [[], [*click(1), {"op": "claimed", "buttons": ["mouse4"]},
          call(WM_XBUTTONDOWN, 1, LLMHF_INJECTED), call(WM_XBUTTONUP, 1, LLMHF_INJECTED),
          call(WM_XBUTTONDOWN, 3), call(WM_XBUTTONDOWN, 0), call(WM_XBUTTONDOWN, 1, 0, (), -1),
          call(WM_XBUTTONDOWN, 1, LLMHF_LOWER_IL_INJECTED), call(WM_XBUTTONUP, 1)]],
    # Modifiers come from GetAsyncKeyState: either Windows key is "win".
    [BOTH, [*click(1, pressed=[VK_SHIFT, VK_MENU]), *click(2, pressed=[VK_RWIN]),
            *click(1, pressed=[VK_LWIN, VK_CONTROL, VK_SHIFT, VK_MENU]), *click(2, pressed=[0x41])]],
    # The release of a swallowed press is swallowed after a restart; one never pressed is not.
    [["mouse4"], [call(WM_XBUTTONDOWN, 1), {"op": "restart"}, call(WM_XBUTTONUP, 1),
                  call(WM_XBUTTONUP, 1), call(WM_XBUTTONDOWN, 2), {"op": "restart"},
                  call(WM_XBUTTONUP, 2)]],
    # A release is matched by its button alone: the other button's press does not count.
    [BOTH, [call(WM_XBUTTONDOWN, 1, pressed=[VK_CONTROL]), call(WM_XBUTTONUP, 2),
            call(WM_XBUTTONDOWN, 2), call(WM_XBUTTONUP, 1, pressed=[VK_CONTROL]),
            call(WM_XBUTTONUP, 2)]],
    # An injected release of a swallowed press passes, and the real one is still swallowed.
    [BOTH, [call(WM_XBUTTONDOWN, 1), call(WM_XBUTTONUP, 1, LLMHF_INJECTED),
            call(WM_XBUTTONUP, 1)]],
    # A claims that raises lets the press through and swallows nothing later.
    [BOTH, [{"op": "raise", "value": True}, *click(1, pressed=[VK_CONTROL]),
            {"op": "raise", "value": False}, *click(1)]],
    # Two presses before a release: one release swallowed.
    [BOTH, [call(WM_XBUTTONDOWN, 1), call(WM_XBUTTONDOWN, 1), call(WM_XBUTTONUP, 1),
            call(WM_XBUTTONUP, 1)]],
]

WM_RBUTTONDOWN = 0x0204
WM_RBUTTONUP = 0x0205
WM_MBUTTONDOWN = 0x0207
WM_MBUTTONUP = 0x0208


def press(message=WM_LBUTTONDOWN, x=400, y=300, owner="other", window=None, flags=0, code=0):
    """A press over `window` (by default x, or 0 with no owner to read)."""
    if window is None:
        window = x if owner is not None else 0
    return {"op": "call", "code": code, "message": message, "x": x, "y": y, "flags": flags,
            "window": window, "owner": owner}


def up(message=WM_LBUTTONUP, x=-30, y=12, flags=0, code=0):
    return {"op": "call", "code": code, "message": message, "x": x, "y": y, "flags": flags,
            "window": 5, "owner": "other"}


RESTART = {"op": "restart"}

CLICK_TRACES = [
    # tests/test_personalize_s6_backends.py: its own window, injected, then another app's.
    [press(owner="self"), up(), press(WM_RBUTTONDOWN, flags=LLMHF_INJECTED),
     up(WM_RBUTTONUP, flags=LLMHF_INJECTED), press(), up()],
    # No window under the point, an owner that cannot be read, then a real one.
    [press(window=0), up(), press(owner=None, window=9), up(), press(owner="other"), up(x=7, y=8)],
    # Left and right are each matched by their own button.
    [press(), up(WM_RBUTTONUP), up(), press(WM_RBUTTONDOWN), up(WM_LBUTTONUP),
     up(WM_RBUTTONUP, x=1, y=2)],
    # A second press over this app forgets the first; a lone release is nothing.
    [press(), press(owner="self"), up(), up(), press(), up(), up()],
    # Injected halves: an injected press records nothing and asks nothing; an injected release
    # leaves the real press waiting.
    [press(flags=LLMHF_INJECTED), up(), press(), up(flags=LLMHF_INJECTED), up(x=3, y=4),
     press(flags=LLMHF_LOWER_IL_INJECTED), up()],
    # Negative codes are not read at all.
    [press(code=-1), up(), press(), up(code=-1), up()],
    # The middle button, moves and wheels are not clicks; side buttons pass beside them.
    [press(WM_MBUTTONDOWN), up(WM_MBUTTONUP), {**press(WM_MOUSEMOVE)}, {**press(WM_MOUSEWHEEL)},
     press(), {**press(WM_XBUTTONDOWN), "xbutton": 1}, {**up(WM_XBUTTONUP), "xbutton": 1}, up()],
    # A new installation forgets what the last one saw pressed.
    [press(), RESTART, up(), press(WM_RBUTTONDOWN), up(WM_RBUTTONUP)],
]

CASES = {"callback_trace": TRACES, "click_trace": [[trace] for trace in CLICK_TRACES]}
