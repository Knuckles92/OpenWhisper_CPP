"""Curated inputs for services/walkthrough/styles.py (fv_walkthrough's styles.h).

Settings are given as JSON objects keyed by the settings key's string. Styles come back as
[id, name, instructions] lists; an id save_style made up (12 random hex digits) reads "<new>".

    _saved_styles(settings), _load_styles(settings)
    _style_named(settings, name)             among load_styles(settings): its id, or None
    _style_for(settings, instructions)       among load_styles(settings): its id, or None
    _save_style(settings, name, instructions)
        {"style": the style, "saved": the setting afterwards} or {"error": the ValueError's
        text, "saved": the setting afterwards (None when absent)}
    _delete_style(settings, style_id)        the setting afterwards
"""
from __future__ import annotations

import copy

KEY = "walkthrough_write_up_styles"


def _listed(styles):
    return [[s.id, s.name, s.instructions] for s in styles]


def _saved_styles(module, settings):
    return _listed(module.saved_styles(settings))


def _load_styles(module, settings):
    return _listed(module.load_styles(settings))


def _style_named(module, settings, name):
    style = module.style_named(name, module.load_styles(settings))
    return None if style is None else style.id


def _style_for(module, settings, instructions):
    style = module.style_for(instructions, module.load_styles(settings))
    return None if style is None else style.id


def _save_style(module, settings, name, instructions):
    settings = copy.deepcopy(settings)
    known = {s.id for s in module.saved_styles(settings)}

    def stored():
        if KEY not in settings:
            return None
        return [[item["id"] if item["id"] in known else "<new>", item["name"],
                 item["instructions"]] for item in settings[KEY]]

    try:
        style = module.save_style(settings, name, instructions)
    except ValueError as exc:
        return {"error": str(exc), "saved": stored()}
    return {"style": [style.id if style.id in known else "<new>", style.name,
                      style.instructions], "saved": stored()}


def _delete_style(module, settings, style_id):
    settings = copy.deepcopy(settings)
    module.delete_style(settings, style_id)
    return settings[KEY]


WRAPPERS = {
    "_saved_styles": _saved_styles,
    "_load_styles": _load_styles,
    "_style_named": _style_named,
    "_style_for": _style_for,
    "_save_style": _save_style,
    "_delete_style": _delete_style,
}


def _style(style_id, name, instructions="Words."):
    return {"id": style_id, "name": name, "instructions": instructions}


BROKEN = {KEY: [
    _style("a", "Fine"),
    _style("a", "Same id"),
    _style("walkthrough", "Posing as the default"),
    _style("b", ""),
    _style("c", "  Spaced  ", "  Trimmed words.  "),
    {"id": "d", "name": "No words"},
    {"id": 4, "name": "Number id", "instructions": "Words."},
    {"id": "e", "name": "Blank words", "instructions": " \n "},
    "not a style",
    None,
    ["e", "List", "Words."],
    _style(" f ", "Spaced id"),
]}

FULL = {KEY: [_style(f"id{n}", f"Style {n}") for n in range(30)]}
OVERFULL = {KEY: [_style(f"id{n}", f"Style {n}") for n in range(35)]}
MINE = {KEY: [_style("mine", "Release notes", "Bullets of what changed.")]}

CASES = {
    "_saved_styles": [
        [{}], [{KEY: None}], [{KEY: []}], [{KEY: "nonsense"}], [{KEY: {"id": "x"}}], [BROKEN],
        [OVERFULL], [{"other": 1}],
    ],
    "_load_styles": [[{}], [{KEY: []}], [BROKEN], [MINE]],
    "_style_named": [
        [{}, "Bug report"], [{}, "  bug REPORT "], [{}, "walkthrough"], [{}, "Nope"],
        [{}, "STEP-BY-STEP GUIDE"], [MINE, "release notes"], [MINE, "Bug report"],
        [{KEY: [_style("s", "Straße")]}, "STRASSE"],
    ],
    "_style_for": [
        [{}, ""], [{}, "   "], [{}, "Something else."],
        [{}, "Write this up as a bug report. Start with a short title and a one-sentence summary "
             "of the problem, then the steps to reproduce it, what happened, and what I expected "
             "instead. Put each screenshot where it shows the problem best. Keep exact names, "
             "numbers and error messages as I said or showed them, and don't guess at causes I "
             "didn't mention.\n"],
        [MINE, "  Bullets of what changed.  "], [MINE, "bullets of what changed."],
    ],
    "_save_style": [
        [{}, "  Release notes ", "  Bullets of what changed. "],
        [MINE, "release NOTES", "Shorter bullets."],
        [{}, "Bug report", "Just the steps."],
        [{}, "", "Words."],
        [{}, "   ", "Words."],
        [{}, "x" * 61, "Words."],
        [{}, "x" * 60, "Words."],
        [{}, "é" * 60, "Words."],
        [{}, "Checklist", "  "],
        [{}, "walkthrough", "Words."],
        [{}, " WALKTHROUGH ", "Words."],
        [FULL, "One more", "Words."],
        [FULL, "style 3", "Replacing one still works."],
        [OVERFULL, "Style 31", "Words."],
        [BROKEN, "Fine", "New words."],
        [{KEY: "nonsense"}, "First", "Words."],
    ],
    "_delete_style": [
        [{}, "bug-report"], [{}, "walkthrough"], [{}, "nope"], [MINE, "mine"], [BROKEN, "a"],
        [{KEY: []}, "step-by-step"],
    ],
}
