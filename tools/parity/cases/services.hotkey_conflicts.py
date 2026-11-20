"""Curated inputs for services/hotkey_conflicts.py (M1.5 hotkeys) over generated settings.

The profiles, transforms and saved actions a conflict check compares against are loaded from the
settings by their own modules (cleanup_profiles, text_transforms, actions.model), which FioriVoce
ports later; the C++ check takes those lists from its caller. So the wrapper records, beside the
answer, the bindings each loader made of the case's settings, and the replay hands exactly those
to hotkey_conflict. The C++ replay is tests/hotkeys/test_golden.cpp.
"""


def _bindings(settings):
    from services import text_transforms
    from services.actions.model import load_actions
    from services.cleanup_profiles import load_cleanup_profiles

    def rows(items):
        return [[item.id, item.name, item.hotkey] for item in items]

    return {
        "profiles": rows(load_cleanup_profiles(settings)),
        "transforms": rows(text_transforms.load_transforms(settings)),
        "actions": rows(load_actions(settings)),
    }


def _conflict(module, hotkey, settings, exclude=None, standard_hotkeys=None):
    kwargs = {}
    if exclude is not None:
        kwargs["exclude"] = tuple(exclude)
    if standard_hotkeys is not None:
        kwargs["standard_hotkeys"] = standard_hotkeys
    return {"conflict": module.hotkey_conflict(hotkey, settings, **kwargs),
            "bindings": _bindings(settings)}


WRAPPERS = {"hotkey_conflict": _conflict}

PROFILES = [
    {"id": "notes", "name": "Meeting notes", "instructions": "Bullets.", "hotkey": "ctrl+shift+f9"},
    {"id": "ticket", "name": "Ticket", "instructions": "A ticket.", "hotkey": ""},
    {"id": "pad", "name": "Numpad", "instructions": "Pad.", "hotkey": "ctrl+kp_5"},
]
TRANSFORMS = [
    {"id": "polish", "name": "Polish", "instruction": "Polish it.", "hotkey": "ctrl+alt+p"},
    {"id": "short", "name": "Make concise", "instruction": "Shorter."},
    {"id": "upper", "name": "Shout", "instruction": "Upper.", "hotkey": "alt+ctrl+kp_*"},
]
ACTIONS = [
    {"id": "a1", "name": "Send to Slack", "hotkey": "ctrl+alt+shift+s", "prompt": "x",
     "kind": "agent"},
]

BASE = {"transcript_cleanup_profiles": PROFILES, "text_transforms": TRANSFORMS}
SAVED = {**BASE, "hotkeys": {"command_mode": "ctrl+alt+c", "record_toggle": "f8",
                             "custom_thing": "ctrl+f7"}}
EMPTY = {"transcript_cleanup_profiles": [], "text_transforms": []}
ACTIONS_SET = {**EMPTY, "actions": ACTIONS}

CASES = {
    "hotkey_conflict": [
        ["", BASE], ["ctrl+shift+f11", BASE], ["kp *", BASE], ["KP *", BASE], ["kp -", BASE],
        ["alt+ctrl+kp *", BASE], ["ctrl+alt+m", BASE], ["ctrl+shift+f12", BASE],
        ["ctrl+shift+f9", BASE], ["shift+ctrl+f9", BASE], ["ctrl+alt+p", BASE],
        ["ctrl+kp 5", BASE], ["ctrl+kp_5", BASE], ["ctrl+alt+kp *", BASE],
        ["ctrl+alt+kp_*", BASE], ["control+alt+p", BASE], ["f13", BASE],
        ["alt+ctrl+c", SAVED], ["ctrl+alt+c", SAVED, None, {}],
        ["ctrl+alt+s", SAVED, None, {"scratchpad_toggle": "ctrl+alt+s"}],
        ["kp *", SAVED], ["f8", SAVED], ["ctrl+f7", SAVED],
        ["kp *", BASE, ["standard", "record_toggle"]],
        ["kp *", BASE, ["profile", "record_toggle"]],
        ["ctrl+shift+f9", BASE, ["profile", "notes"]],
        ["ctrl+alt+p", BASE, ["transform", "polish"]],
        ["ctrl+alt+shift+s", ACTIONS_SET], ["ctrl+alt+shift+s", ACTIONS_SET, ["action", "a1"]],
        ["kp *", {}], ["ctrl+alt+r", {}], ["ctrl+alt+m", {"hotkeys": {}}],
        ["ctrl+alt+m", {"hotkeys": {"minimize_tray": ""}}],
        ["ctrl+alt+m", {"hotkeys": None}],
    ],
}
