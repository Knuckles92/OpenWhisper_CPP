"""Curated inputs for ui_qt/utils/palette.py (fv_ui_theme Palette).

The palette argument is a theme name ("dark", "light") for the built-in palettes, or a
{token: value} object for a Palette("test", tokens). color() results are QColor.name(HexArgb),
"#aarrggbb"; an invalid colour gives what Qt gives for it.
"""


def _palette(module, which):
    if isinstance(which, dict):
        return module.Palette("test", which)
    return module.palette_for(which)


def _resolve(module, which, text):
    return _palette(module, which).resolve(text)


def _css(module, which, token):
    return _palette(module, which).css(token)


def _color(module, which, token, alpha=None):
    from PyQt6.QtGui import QColor

    color = _palette(module, which).color(token, alpha)
    return [color.isValid(), color.name(QColor.NameFormat.HexArgb)]


def _palette_for(module, theme):
    palette = module.palette_for(theme)
    return [palette.name, palette.is_dark]


WRAPPERS = {
    "resolve": _resolve,
    "css": _css,
    "color": _color,
    "palette_for": _palette_for,
}

_EVERY_TOKEN = (
    "QWidget { background: @bg; color: @text; border: 1px solid @border; }\n"
    "QLabel[tone=\"danger\"] { color: @danger-text; background: rgba(@danger-rgb, 0.12); }\n"
    "QPushButton#primaryButton { background: @accent; color: @on-accent; }\n"
    "QToolTip { background: @surface-hover; border: 1px solid @border-strong; }\n"
    "image: url(:/ui_theme/assets/tabler/chevron-down-gray@icon-suffix.svg);\n"
    "QFrame#emptyState { border: 1px dashed @slate-border-strong; background: @slate-disabled; }"
)

def _token_names():
    """Every token of the committed palette data, so each one is resolved and coloured once."""
    import json
    from pathlib import Path

    data = Path(__file__).resolve().parents[3] / "src" / "ui_theme" / "data" / "palettes.json"
    return list(json.loads(data.read_text(encoding="utf-8"))["palettes"]["dark"])


_ALL_TOKENS = _token_names()
_ALL_TOKENS_SHEET = "\n".join(f"/* {name} */ x: @{name};" for name in _ALL_TOKENS)

_TEST_TOKENS = {
    "hex": "#0a84ff", "short": "#fff", "argb": "#800a84ff", "named": "red", "bad": "#zzzzzz",
    "rgba": "rgba(28, 28, 30, 0.5)", "rgba-spaced": "rgba( 1 ,2,  3 , 0.004 )",
    "rgba-int": "rgba(10, 20, 30, 1)", "rgb": "10, 132, 255", "rgb-pad": "  7,8 ,9  ",
    "rgb-big": "300, 0, 0", "empty": "", "transparent": "transparent",
}

CASES = {
    "resolve": [
        ["dark", _EVERY_TOKEN], ["light", _EVERY_TOKEN], ["dark", "no tokens at all"],
        ["dark", ""], ["dark", "color: @no-such-token;"], ["light", "@text@text"],
        ["dark", "mail@example.com"], ["dark", "@Text"], ["dark", "@text-"],
        ["dark", "@slate-text-3-rgb, @slate-text-2"], ["dark", "@ text"],
        ["light", "@icon-suffix.svg"], ["dark", "a @accent-rgb-x b"],
        ["dark", "@text-heading-"], ["dark", "@9text"], ["dark", "@@bg"],
        ["dark", _ALL_TOKENS_SHEET], ["light", _ALL_TOKENS_SHEET],
    ],
    "css": [["dark", "bg"], ["light", "icon-suffix"], ["dark", "icon-suffix"],
            ["dark", "nope"], ["light", "inset"]],
    "color": [
        [_TEST_TOKENS, "hex"], [_TEST_TOKENS, "short"], [_TEST_TOKENS, "argb"],
        [_TEST_TOKENS, "named"], [_TEST_TOKENS, "bad"], [_TEST_TOKENS, "rgba"],
        [_TEST_TOKENS, "rgba-spaced"], [_TEST_TOKENS, "rgba-int"], [_TEST_TOKENS, "rgb"],
        [_TEST_TOKENS, "rgb-pad"], [_TEST_TOKENS, "rgb", 40], [_TEST_TOKENS, "hex", 0],
        [_TEST_TOKENS, "rgba", 255], [_TEST_TOKENS, "empty"], [_TEST_TOKENS, "transparent"],
        [_TEST_TOKENS, "missing"],
        ["dark", "inset"], ["dark", "inset-soft"], ["dark", "overlay-bg"],
        ["dark", "overlay-border"], ["light", "inset"], ["light", "inset-soft"],
        ["light", "overlay-bg"], ["light", "overlay-border"], ["dark", "accent-rgb"],
        ["light", "slate-text-3-rgb"], ["dark", "accent", 70], ["light", "on-accent", 46],
    ] + [[theme, name] for theme in ("dark", "light") for name in _ALL_TOKENS],
    "palette_for": [["dark"], ["light"], ["sepia"], [""], ["Light"], ["system"]],
}
