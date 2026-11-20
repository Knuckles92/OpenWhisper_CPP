"""Write src/hotkeys/data/keyboard_names.json: the key names of Python's `keyboard` package.

    & $env:OPENWHISPER_CPP_OW_PYTHON -I -B tools/gen/keyboard_names.py [--check]

Saved Windows shortcuts spell their keys the way `keyboard` names hook events ("kp *" is the
numpad "*", "page up", "!" for Shift+1 going down), so the C++ hook must name every event exactly
as `keyboard` does. `keyboard` builds its names at run time from the active layout:
`_winkeyboard.get_event_names` asks ToUnicode, GetKeyNameText and MapVirtualKey, then falls back
to its table of official virtual keys, and `KeyboardEvent` normalizes the first answer through
`_canonical_names.normalize_name`. This script runs that very code (OpenWhisper's interpreter has
the package) for every (scan code, virtual key, extended) the hook can see and every combination
of the five states `keyboard` names by (Shift, AltGr, Num Lock, Caps Lock, Scroll Lock), and
writes the answers with the tables the C++ needs to compute the same names on another layout.

Nothing here installs a hook or sends a key: importing `keyboard` starts no listener, and the
name tables only call MapVirtualKeyExW, ToUnicode and GetKeyNameTextW on this thread.

The file records the layout it was made on; the committed one is the US layout (00000409).
--check compares with the committed file instead of writing it and fails on a difference.
"""

from __future__ import annotations

import argparse
import ctypes
import importlib.metadata
import json
import sys
from ctypes import wintypes
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
OUTPUT = ROOT / "src" / "hotkeys" / "data" / "keyboard_names.json"

# Bit i of a modifier mask is MODIFIER_BITS[i]; `keyboard`'s hook builds its tuple in this order.
MODIFIER_BITS = ("shift", "alt gr", "num lock", "caps lock", "scroll lock")
MAPVK_VSC_TO_VK_EX = 3
MAPVK_VK_TO_VSC_EX = 4
# The fake left Ctrl Windows sends with AltGr: `keyboard` names it "alt gr" by hand.
ALT_GR = (541, 162)


def modifiers_of(mask: int) -> tuple:
    return tuple(name for bit, name in enumerate(MODIFIER_BITS) if mask & (1 << bit))


def layout_name() -> str:
    buffer = ctypes.create_unicode_buffer(9)
    get_name = ctypes.WinDLL("user32").GetKeyboardLayoutNameW
    get_name.argtypes = [wintypes.LPWSTR]
    get_name.restype = wintypes.BOOL
    if not get_name(buffer):
        raise SystemExit("GetKeyboardLayoutNameW failed")
    return buffer.value


def triples(winkeyboard) -> list[tuple[int, int, int]]:
    """Every (scan code, virtual key, extended) the hook can report, in a fixed order."""
    map_key = ctypes.WinDLL("user32").MapVirtualKeyExW
    map_key.argtypes = [wintypes.UINT, wintypes.UINT, wintypes.HKL]
    map_key.restype = wintypes.UINT
    pairs = []
    # keyboard's own name-table pairs: every scan code to its key, every key to its scan code.
    pairs += [(sc, map_key(sc, MAPVK_VSC_TO_VK_EX, None)) for sc in range(0x100)]
    pairs += [(map_key(vk, MAPVK_VK_TO_VSC_EX, None), vk) for vk in range(0x100)]
    # Keys behind the 0xE0 prefix, which the hook reports with their bare scan code.
    pairs += [(sc, map_key(0xE000 | sc, MAPVK_VSC_TO_VK_EX, None)) for sc in range(0x100)]
    # Numpad keys report another key with Num Lock off.
    pairs += [(sc, vk) for sc, vk, _extended in winkeyboard.keypad_keys]
    # Virtual-key input with no scan code (SendInput, accessibility tools).
    pairs += [(0, vk) for vk in range(0x100)]
    pairs.append(ALT_GR)
    seen = set()
    result = []
    for sc, vk in pairs:
        for extended in (0, 1):
            triple = (sc, vk, extended)
            if triple not in seen:
                seen.add(triple)
                result.append(triple)
    return sorted(result)


def event_name(winkeyboard, normalize_name, sc, vk, extended, mask) -> str:
    modifiers = modifiers_of(mask)
    if (sc, vk) == ALT_GR and modifiers in winkeyboard.distinct_modifiers:
        names = ["alt gr"]
    else:
        names = list(winkeyboard.get_event_names(sc, vk, extended, modifiers))
    return normalize_name(names[0]) if names else ""


def generate() -> str:
    import keyboard
    from keyboard import _canonical_names, _winkeyboard

    _winkeyboard.init()
    pool = [""]
    index = {"": 0}
    keys = []
    for sc, vk, extended in triples(_winkeyboard):
        row = []
        for mask in range(1 << len(MODIFIER_BITS)):
            name = event_name(_winkeyboard, _canonical_names.normalize_name, sc, vk, extended,
                              mask)
            if name not in index:
                index[name] = len(pool)
                pool.append(name)
            row.append(index[name])
        if not any(row):
            continue
        keys.append([sc, vk, extended, row[0] if len(set(row)) == 1 else row])

    # keyboard.is_pressed(<modifier>) asks for any of these scan codes; the matcher's names.
    modifier_scan_codes = {
        name: list(keyboard.key_to_scan_codes(spelling, False))
        for name, spelling in (("ctrl", "ctrl"), ("alt", "alt"), ("shift", "shift"),
                               ("win", "win"))
    }
    official = [[vk, name, keypad]
                for vk, (name, keypad) in sorted(_winkeyboard.official_virtual_keys.items())]
    keypad = []
    for triple in _winkeyboard.keypad_keys:
        if list(triple) not in keypad:
            keypad.append(list(triple))

    def line(value) -> str:
        return json.dumps(value, ensure_ascii=True, separators=(",", ":"))

    out = ["{",
           f' "generator": "tools/gen/keyboard_names.py",',
           f' "keyboard": {line(importlib.metadata.version("keyboard"))},',
           f' "layout": {line(layout_name())},',
           f' "modifier_bits": {line(list(MODIFIER_BITS))},',
           f' "alt_gr": {line(list(ALT_GR))},',
           f' "alt_gr_modifier_masks": '
           f'{line(sorted(sum(1 << MODIFIER_BITS.index(m) for m in mods) for mods in _winkeyboard.distinct_modifiers))},',
           f' "modifier_scan_codes": {line(modifier_scan_codes)},',
           ' "official_virtual_keys": [']
    out += [f"  {line(row)}," for row in official[:-1]] + [f"  {line(official[-1])}"]
    out += [" ],", ' "keypad_keys": [']
    out += [f"  {line(row)}," for row in keypad[:-1]] + [f"  {line(keypad[-1])}"]
    out += [" ],", ' "canonical_names": {']
    canonical = list(_canonical_names.canonical_names.items())
    out += [f"  {line(k)}:{line(v)}," for k, v in canonical[:-1]]
    out += [f"  {line(canonical[-1][0])}:{line(canonical[-1][1])}"]
    out += [" },", ' "names": [']
    out += [f"  {line(name)}," for name in pool[:-1]] + [f"  {line(pool[-1])}"]
    out += [" ],", ' "keys": [']
    out += [f"  {line(row)}," for row in keys[:-1]] + [f"  {line(keys[-1])}"]
    out += [" ]", "}", ""]
    return "\n".join(out)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--check", action="store_true",
                        help="compare with the committed file instead of writing it")
    args = parser.parse_args()
    if sys.platform != "win32":
        raise SystemExit("The keyboard package's Windows names can only be read on Windows.")
    text = generate()
    if args.check:
        current = OUTPUT.read_text(encoding="utf-8") if OUTPUT.exists() else ""
        if current != text:
            print(f"{OUTPUT.relative_to(ROOT)} is out of date", file=sys.stderr)
            return 1
        print(f"{OUTPUT.relative_to(ROOT)} is up to date")
        return 0
    OUTPUT.parent.mkdir(parents=True, exist_ok=True)
    OUTPUT.write_bytes(text.encode("utf-8"))
    print(f"wrote {OUTPUT.relative_to(ROOT)}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
