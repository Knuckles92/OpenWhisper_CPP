"""Curated inputs for services/setting_types.py (M0.6 settings): every declared key's read and
stored over the JUNK battery of tests/test_typed_settings.py:41-54, the edges where a literal
C++ translation differs from Python (int() of text, floats, bools and huge numbers; strip and
lower of non-ASCII; aliases), and each key's default, choices and aliases.

Each case calls a wrapper with [key, raw] and records {"read": ..., "stored": ...} of the
declaration over {key: raw}; "unset" records them over {}. The C++ replay is
tests/settings/test_typed.cpp.
"""

import math

#: tests/test_typed_settings.py JUNK, in its order.
JUNK = [
    None, True, False, 0, 1, -3, 7, 100, 3.7, "", " ", "yes", "no", "0", "12",
    "auto", "  EN ", "garbage", [], ["a"], [1, 2], {}, {"a": 1}, {"claude-code": "m"},
]
#: Edges every key is tried with.
EDGES = [
    "  12 ", "1_000", "3.7", "-5", "+7", "\t9\n", "1__0", "0x10", "٣", "²",
    "99999999999999999999", "-99999999999999999999",
    math.inf, -math.inf, math.nan, 1.0, -0.5, -3.9, 1e30, 2 ** 63,
    "live", "orb", "bubbles", "api_whisper", "api_gpt4o", "api_gpt4o_mini", "direct",
    "local_whisper", "parakeet_mlx", "parakeet", "remote", "api",
]
#: tests/test_typed_settings.py _battery's extras for IntSetting.
INT_EXTRAS = [0, 1, 9, 10, 48, 49, 65535, 65536, -1, "47", 8767.0]
#: Text where strip(), lower() and the 2000-code-point cut matter.
TEXT_EXTRAS = [
    "  hello  ", " FrÉ 　", " DEİ ", "ẞ", "x" * 2050,
    " " + "é" * 2001 + " ", "  memo set  ", "Two halves.",
]


def _key(value):
    return (type(value).__name__, repr(value))


def _battery(setting):
    values = [*JUNK, *EDGES]
    kind = type(setting).__name__
    if kind in ("IntSetting", "IntChoiceSetting"):
        values += INT_EXTRAS
    if kind in ("StrSetting", "ChoiceSetting"):
        values += TEXT_EXTRAS
    values.append(setting.default)
    values += list(getattr(setting, "choices", ()))
    values += list(getattr(setting, "aliases", {}))
    unique, seen = [], set()
    for value in values:
        if _key(value) not in seen:
            seen.add(_key(value))
            unique.append(value)
    return unique


def _settings():
    from services.settings import ALL_SETTINGS

    return ALL_SETTINGS


def _by_key(key):
    from services.settings import SETTINGS_BY_KEY

    return SETTINGS_BY_KEY[key]


def _read_stored(module, key, raw):
    setting = _by_key(key)
    settings = {key: raw}
    return {"read": setting.read(settings), "stored": setting.stored(settings)}


def _unset(module, key):
    setting = _by_key(key)
    return {"read": setting.read({}), "stored": setting.stored({}),
            "default": setting.default, "kind": type(setting).__name__,
            "in_table": setting.in_table}


class _LazyCases(dict):
    """CASES is read after ow_isolation.isolate(); the battery needs the declarations."""

    def _fill(self):
        if not dict.__len__(self):
            dict.__setitem__(self, "unset", [[str(s)] for s in _settings()])
            dict.__setitem__(self, "read_stored",
                             [[str(s), value] for s in _settings() for value in _battery(s)])

    def __iter__(self):
        self._fill()
        return dict.__iter__(self)

    def __len__(self):
        self._fill()
        return dict.__len__(self)

    def get(self, key, default=None):
        self._fill()
        return dict.get(self, key, default)

    def __getitem__(self, key):
        self._fill()
        return dict.__getitem__(self, key)


CASES = _LazyCases()

WRAPPERS = {
    "unset": _unset,
    "read_stored": _read_stored,
}

PINS = {
    "battery": "tests/test_typed_settings.py JUNK plus the edges and extras in "
               "tools/parity/cases/services.setting_types.py",
}
