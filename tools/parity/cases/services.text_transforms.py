"""Curated inputs for services/text_transforms.py (M4.4 text-prompts): loading, the shortcut
check, validation and the mutators.

A Transform travels as the dict of its fields ({id, name, instruction, hotkey}). Settings travel
as plain dicts keyed by the bare setting names, without saved actions. A mutator runs on a deep
copy of its settings and returns {"settings": the settings after} or {"error": the ValueError's
message}; validate returns {"ok": true} or {"error": ...}. Shortcuts use the Windows vocabulary
and win32 defaults.
"""

import copy

_KEY = "text_transforms"


def _row(transform_id, name, instruction="Do it.", hotkey=""):
    return {"id": transform_id, "name": name, "instruction": instruction, "hotkey": hotkey}


_FULL = {_KEY: [_row(str(i), f"T{i}") for i in range(50)]}

_LIBRARIES = [
    {},
    {_KEY: None},
    {_KEY: []},
    {_KEY: "oops"},
    {_KEY: [
        "nonsense",
        {"id": "x", "name": "", "instruction": "Do it."},
        {"id": "y", "name": "No instruction", "instruction": "  "},
        {"id": 3, "name": "Bad id", "instruction": "Do it."},
        _row(" tidy ", " Tidy ", " Tidy it up. ", "Alt+Ctrl+T"),
        _row("tidy", "Duplicate id"),
        _row("loose", "Loose", hotkey=42),
        _row("blank", "Blank", hotkey="   "),
        _row("pad", "Pad", hotkey="kp_5"),
        {"id": "nokey", "name": "No key", "instruction": "Do it."},
    ]},
    {_KEY: [_row(str(i), f"T{i}") for i in range(55)]},
    _FULL,
]

_WITH_BINDINGS = {
    "hotkeys": {"record_toggle": "ctrl+alt+r", "command_mode": "ctrl+alt+k"},
    "transcript_cleanup_profiles": [{"id": "ticket", "name": "Ticket", "instructions": "Format.",
                                     "hotkey": "ctrl+alt+t", "use_learned_rules": True}],
    _KEY: [_row("tidy", "Tidy", "Tidy.", "ctrl+alt+y")],
}


def _transform(fields):
    from services.text_transforms import Transform

    return Transform(**fields)


def _mutator(name):
    def run(module, settings, *args):
        changed = copy.deepcopy(settings)
        try:
            getattr(module, name)(changed, *args)
        except ValueError as exc:
            return {"error": str(exc)}
        return {"settings": changed}

    return run


def _validate(module, settings, fields):
    try:
        module.validate_transform(_transform(fields), copy.deepcopy(settings))
    except ValueError as exc:
        return {"error": str(exc)}
    return {"ok": True}


_NEW = {"id": "mine", "name": " Mine ", "instruction": " Shorter. ", "hotkey": "ctrl+alt+j"}


def _with(**changes):
    return {**_NEW, **changes}


_TRANSFORM_CASES = [
    _NEW,
    _with(name="  "),
    _with(name="x" * 80),
    _with(name="x" * 81),
    _with(instruction="   "),
    _with(name="polish"),
    _with(name="TIDY"),
    _with(id=""),
    _with(id="  "),
    _with(id=" polish ", name="Polish more", instruction="Polish harder.", hotkey=""),
    _with(hotkey="Alt+Ctrl+R"),
    _with(hotkey="ctrl+alt+k"),
    _with(hotkey="ctrl+alt+t"),
    _with(hotkey="alt+ctrl+y"),
    _with(id="tidy", name="Tidy", instruction="Tidier.", hotkey="ctrl+alt+y"),
    _with(hotkey="kp_9"),
    _with(hotkey="   "),
    _with(id="7", name="T7", instruction="Edited."),
    _with(id="fifty", name="Fifty"),
]

CASES = {
    "STARTER_TRANSFORMS": [[]],
    "load_transforms": [[settings] for settings in _LIBRARIES + [_WITH_BINDINGS]],
    "find_transform": [[settings, transform_id] for settings in (_LIBRARIES[0], _LIBRARIES[4])
                       for transform_id in ("polish", "tidy", "loose", "pad", "missing", "")],
    "transform_hotkey_conflict": [
        [hotkey, settings, exclude]
        for hotkey in ("", "ctrl+alt+r", "alt+ctrl+k", "ctrl+alt+t", "ctrl+alt+y", "f13")
        for settings in ({}, _WITH_BINDINGS)
        for exclude in ("", "tidy")
    ],
    "validate_transform": [[settings, fields] for settings in ({}, _WITH_BINDINGS, _FULL)
                           for fields in _TRANSFORM_CASES],
    "save_transform": [[settings, fields] for settings in (
        {}, {"unrelated": "preserve"}, _WITH_BINDINGS, {_KEY: []}, _FULL,
    ) for fields in _TRANSFORM_CASES],
    "delete_transform": [[settings, transform_id] for settings in (
        {}, {_KEY: [_row("a", "A"), _row("b", "B")]}, _LIBRARIES[3], _LIBRARIES[4],
    ) for transform_id in ("polish", "a", "tidy", "missing")],
}

WRAPPERS = {
    "STARTER_TRANSFORMS": lambda module: [t.to_dict() for t in module.STARTER_TRANSFORMS],
    "load_transforms": lambda module, settings: [
        t.to_dict() for t in module.load_transforms(settings)],
    "find_transform": lambda module, settings, transform_id: (
        None if (found := module.find_transform(settings, transform_id)) is None
        else found.to_dict()),
    "transform_hotkey_conflict": lambda module, hotkey, settings, exclude: (
        module.transform_hotkey_conflict(hotkey, settings, exclude_id=exclude)),
    "validate_transform": _validate,
    "save_transform": lambda module, settings, fields: _mutator("save_transform")(
        module, settings, _transform(fields)),
    "delete_transform": _mutator("delete_transform"),
}

PINS = {"hotkeys": "Windows vocabulary, win32 DEFAULT_HOTKEYS"}
