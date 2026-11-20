"""Curated inputs for services/cleanup_profiles.py (M4.4 text-prompts): loading, the
shortcut checks, validation, the mutators, cleanup_may_run and the profile prompt.

A CleanupProfile travels as the dict of its fields ({id, name, instructions, hotkey,
use_learned_rules}). Settings travel as plain dicts keyed by the bare setting names, without
saved actions (fv_text cannot load them). A mutator runs on a deep copy of its settings and
returns {"settings": the settings after} or {"error": the ValueError's message}; validate
returns {"ok": true} or {"error": ...}. Shortcuts use the Windows vocabulary and win32 defaults.
"""

import copy

_KEY = "transcript_cleanup_profiles"


def _row(profile_id, name, instructions="Do it.", hotkey="", **extra):
    return {"id": profile_id, "name": name, "instructions": instructions, "hotkey": hotkey,
            **extra}


_LIBRARIES = [
    {},
    {_KEY: None},
    {_KEY: []},
    {_KEY: "oops"},
    {_KEY: {"id": "x"}},
    {_KEY: [
        None, "bad", {}, {"id": 4, "name": "Bad", "instructions": "Bad"},
        {"id": "ok", "name": " Good ", "instructions": " Do this ", "hotkey": 9},
        {"id": "ok", "name": "Duplicate", "instructions": "Skip"},
    ]},
    {_KEY: [
        _row("notes", "Meeting notes", "Bullets.", "Shift+Ctrl+F9"),
        _row("ticket", "Ticket", "A ticket.", "", use_learned_rules=False),
        _row("memo", "Memo", "Tidy.", "kp_7", use_learned_rules=0),
        _row("blank", "  ", "Nothing."),
        _row(" spaced ", " Spaced ", " Spaced out. ", "alt+control+t", use_learned_rules=None),
    ]},
    {_KEY: [_row("only", "Only", hotkey=None)], "quick_record_profile": "only"},
]

_HOTKEYS = {"hotkeys": {"record_toggle": "ctrl+alt+r", "command_mode": "ctrl+alt+k"}}
_WITH_TRANSFORMS = {
    **_HOTKEYS,
    _KEY: [_row("ticket", "Ticket", "Format.", "ctrl+alt+t")],
    "text_transforms": [{"id": "tidy", "name": "Tidy", "instruction": "Tidy.",
                         "hotkey": "ctrl+alt+y"}],
}


def _profile(fields):
    from services.cleanup_profiles import CleanupProfile

    return CleanupProfile(**fields)


def _plain(profile):
    return None if profile is None else profile.to_dict()


def _mutator(name):
    def run(module, settings, *args):
        changed = copy.deepcopy(settings)
        try:
            getattr(module, name)(changed, *args)
        except ValueError as exc:
            return {"error": str(exc)}
        return {"settings": changed}

    return run


def _save(module, settings, fields):
    return _mutator("save_cleanup_profile")(module, settings, _profile(fields))


def _validate(module, settings, fields):
    try:
        module.validate_profile(_profile(fields), copy.deepcopy(settings))
    except ValueError as exc:
        return {"error": str(exc)}
    return {"ok": True}


_NEW = {"id": "new", "name": "Release notes", "instructions": "Bullets.", "hotkey": "",
        "use_learned_rules": True}


def _with(**changes):
    return {**_NEW, **changes}


_PROFILE_CASES = [
    _NEW,
    _with(name=""),
    _with(name="   "),
    _with(name="x" * 80),
    _with(name="x" * 81),
    _with(name=" " + "é" * 80 + " "),
    _with(instructions=" "),
    _with(name="EMAIL"),
    _with(name="email ", id="email"),
    _with(name="  Support Ticket"),
    _with(hotkey="ctrl+alt+r"),
    _with(hotkey="alt+ctrl+K"),
    _with(hotkey="ctrl+alt+t"),
    _with(hotkey="ctrl+alt+y"),
    _with(hotkey="kp_7"),
    _with(hotkey="ctrl+shift+f9"),
    _with(id="ticket", name="Ticket 2", hotkey="ctrl+alt+t"),
    _with(id="support-ticket", name="Support ticket", instructions=" Edited. "),
    _with(use_learned_rules=False, hotkey="ctrl+alt+j"),
]

CASES = {
    "STARTER_PROFILES": [[]],
    "load_cleanup_profiles": [[settings] for settings in _LIBRARIES + [_WITH_TRANSFORMS]],
    "find_cleanup_profile": [[settings, profile_id] for settings in _LIBRARIES[:1] + _LIBRARIES[5:]
                             for profile_id in ("email", "ok", "notes", "spaced", "missing", "")],
    "cleanup_may_run": [[settings] for settings in [
        {}, {"transcript_cleanup_enabled": True}, {"transcript_cleanup_enabled": "yes"},
        {"quick_record_profile": "email"}, {"quick_record_profile": ""},
        {"quick_record_profile": 3},
        {_KEY: [_row("memo", "Memo", "Tidy.", "ctrl+alt+m")]},
        {_KEY: [_row("memo", "Memo", "Tidy.")]},
        {"hotkeys": {"command_mode": "ctrl+alt+k"}}, {"hotkeys": {"command_mode": ""}},
        {"hotkeys": {"command_mode": 1}}, {"hotkeys": {"command_mode": []}},
        {"hotkeys": "oops"}, {"hotkeys": None},
        {"text_transforms": [{"id": "a", "name": "A", "instruction": "B", "hotkey": "f9"}]},
        {"text_transforms": [{"id": "a", "name": "A", "instruction": "B", "hotkey": " "}]},
        {"text_transforms": []},
        _WITH_TRANSFORMS,
    ]],
    "normalize_hotkey": [[hotkey] for hotkey in (
        "", "ctrl+alt+t", "Alt+Ctrl+T", "alt+control+t", "kp_7", "ctrl+kp_add", "shift+kp_",
        "windows+f9", "win+shift+a", "  CTRL + Shift + F9 ", "ctrl+", "+", "ctrl+mouse4",
    )],
    "profile_hotkey_conflict": [
        [hotkey, settings, exclude, standard]
        for hotkey in ("", "ctrl+alt+r", "alt+ctrl+k", "ctrl+alt+t", "ctrl+alt+y", "f13",
                       "ctrl+alt+c")
        for settings in ({}, _WITH_TRANSFORMS)
        for exclude in ("", "ticket")
        for standard in (None, {}, {"command_mode": "ctrl+alt+c"})
    ],
    "validate_profile": [[settings, fields] for settings in ({}, _WITH_TRANSFORMS)
                         for fields in _PROFILE_CASES],
    "save_cleanup_profile": [[settings, fields] for settings in (
        {}, {"unrelated": "preserve"}, _WITH_TRANSFORMS, {_KEY: []},
    ) for fields in _PROFILE_CASES],
    "delete_cleanup_profile": [[settings, profile_id] for settings in (
        {}, {"unrelated": "preserve", "quick_record_profile": "email"}, _LIBRARIES[5],
        _LIBRARIES[6], _LIBRARIES[7], {_KEY: "oops", "quick_record_profile": "x"},
    ) for profile_id in ("email", "support-ticket", "ok", "only", "spaced", "x", "missing")],
    "compose_profile_prompt": [[fields, rules] for fields in (
        _NEW, _with(use_learned_rules=False), _with(instructions="Line one.\nLine two."),
        {"id": "email", "name": "Email", "instructions": "Turn it into an email.",
         "hotkey": "", "use_learned_rules": True},
    ) for rules in ([], ["Spell Acme correctly."], ["One.", "Two.", "Three."])],
}

WRAPPERS = {
    "STARTER_PROFILES": lambda module: [p.to_dict() for p in module.STARTER_PROFILES],
    "load_cleanup_profiles": lambda module, settings: [
        p.to_dict() for p in module.load_cleanup_profiles(settings)],
    "find_cleanup_profile": lambda module, settings, profile_id: _plain(
        module.find_cleanup_profile(settings, profile_id)),
    "profile_hotkey_conflict": lambda module, hotkey, settings, exclude, standard: (
        module.profile_hotkey_conflict(hotkey, settings, exclude_id=exclude,
                                       standard_hotkeys=standard)),
    "validate_profile": _validate,
    "save_cleanup_profile": _save,
    "delete_cleanup_profile": _mutator("delete_cleanup_profile"),
    "compose_profile_prompt": lambda module, fields, rules: module.compose_profile_prompt(
        _profile(fields), rules),
}

PINS = {"hotkeys": "Windows vocabulary, win32 DEFAULT_HOTKEYS"}
