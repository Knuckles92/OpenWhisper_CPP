"""Curated inputs for ui_qt/whats_new.py (M2.2 appcore/whats_new.h).

OpenWhisper_CPP has its own note (lead amendment 16), so the vectors record is_seen against a stored
value written relative to the current note: "<current>" stands for WHATS_NEW.updated, which the
C++ replay replaces with its own note's date. Every other stored value is passed as it is.
"""
from __future__ import annotations

CURRENT = "<current>"


def _is_seen(module, stored):
    return module.is_seen(module.WHATS_NEW.updated if stored == CURRENT else stored)


WRAPPERS = {"is_seen": _is_seen}

CASES = {
    "is_seen": [[CURRENT], [""], ["2000-01-01"], [True], [False], [None], [0], [[]], [{}],
                ["2026-11-09 "], ["<current>x"]],
}


PINS = {
    "note": "<current> stands for the note's own updated date; MAX_TEXT_LENGTH is 280",
}
