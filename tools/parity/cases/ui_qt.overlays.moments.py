"""Curated inputs for ui_qt/overlays/moments.py (fv_uimath easing and moments).

glyph_path results are {"strokes": [[[x, y], ...], ...], "length": QPainterPath.length()}, one
list of vertices per subpath. "tables" is every constant and table the C++ side keeps.
"""
import math

FLOAT_TOLERANCE = 1e-12


def _glyph_path(module, glyph, center, amount):
    from PyQt6.QtCore import QPointF
    from PyQt6.QtGui import QPainterPath

    path = module.glyph_path(glyph, QPointF(*center), amount)
    strokes = []
    for i in range(path.elementCount()):
        element = path.elementAt(i)
        if element.type == QPainterPath.ElementType.MoveToElement:
            strokes.append([])
        strokes[-1].append([element.x, element.y])
    return {"strokes": strokes, "length": path.length()}


def _tables(module):
    from dataclasses import asdict

    def moment(value):
        data = asdict(value)
        data["hues"] = [list(r) for r in data["hues"]]
        return data

    return {
        "constants": {
            "RADIUS": module.RADIUS, "POP_S": module.POP_S, "RING_S": module.RING_S,
            "GLYPH_DELAY_S": module.GLYPH_DELAY_S, "GLYPH_S": module.GLYPH_S,
            "LABEL_DELAY_S": module.LABEL_DELAY_S, "LABEL_S": module.LABEL_S,
            "GLINT_DELAY_S": module.GLINT_DELAY_S, "GLINT_S": module.GLINT_S,
            "FADE_S": module.FADE_S,
        },
        "sparkles": [list(s) for s in module.SPARKLES],
        "afterglow": [list(s) for s in module._AFTERGLOW],
        "glyphs": {name: [[list(p) for p in stroke] for stroke in strokes]
                   for name, strokes in module.GLYPHS.items()},
        "glyph_widths": dict(module._GLYPH_WIDTH),
        "moments": {name: moment(value) for name, value in module.MOMENTS.items()},
        "canceled": moment(module.CANCELED),
    }


WRAPPERS = {"glyph_path": _glyph_path, "tables": _tables}

_NOW = [-1.0, -0.001, 0.0, 0.01, 0.06, 0.1, 0.1 + 1e-9, 0.16, 0.24, 0.3, 0.5, 0.6, 0.85, 1.2, 2.0, 5.0]
_T = [-2.0, -0.5, -1e-9, 0.0, 1e-9, 0.1, 0.16, 0.25, 0.3333, 0.5, 0.6, 0.75, 0.9, 0.999, 1.0,
      1.0 + 1e-9, 1.2, 2.0, 10.0]

CASES = {
    "phase": (
        [[now, 0.0, 0.24] for now in _NOW]
        + [[now, 0.25, 0.8] for now in _NOW]
        + [[now, 0.1, 0.26] for now in _NOW]
        + [[0.3, 0.0, 1e-9], [0.0, 0.0, 1e-9], [-1.0, 0.0, 1e-9], [5.0, 5.0, 0.3],
           [float("nan"), 0.0, 1.0], [0.5, 0.0, 1e300], [1e300, 0.0, 1.0]]
    ),
    "ease_out_cubic": [[t] for t in _T] + [[float("nan")], [float("inf")], [-1e5]],
    "ease_out_back": (
        [[t] for t in _T]
        + [[t, 2.2] for t in _T]
        + [[t, 3.0] for t in _T]
        + [[t, 1.0] for t in _T]
        + [[t, 0.0] for t in _T]
        + [[0.5, 1.70158], [0.5, 4.5], [0.25, -1.0]]
    ),
    "exit_opacity": [[now, total] for total in (1.5, 0.3, 2.0, 0.1)
                     for now in (-0.1, 0.0, 0.5, 1.0, 1.19, 1.2, 1.2 + 1e-9, 1.35, 1.5, 1.8, 2.0,
                                 2.5)],
    "shake_offset": (
        [[now, 0.0, 0.4, 4.0, 3.5] for now in (-0.1, 0.0, 0.05, 0.1, 0.2, 0.3, 0.399, 0.4, 0.5)]
        + [[now, 0.2, 0.36, 2.6, 3.0] for now in (0.0, 0.2, 0.25, 0.3, 0.38, 0.5, 0.559, 0.56, 1.0)]
        + [[0.1, 0.0, 0.4, 0.0, 3.0], [0.1, 0.0, 0.4, -3.0, 2.0], [0.07, 0.0, 0.4, 1.0, 0.0],
           [0.1, 0.0, 0.4, 5.0, 0.25]]
    ),
    "glyph_path": [
        [glyph, center, amount]
        for glyph in ("check", "cross", "bang", "clip")
        for center in ([0.0, 0.0], [50.5, 29.0], [-13.25, 7.5])
        for amount in (-0.2, 0.0, 1e-9, 0.1, 0.25, 0.3333, 0.5, 0.6, 0.75, 0.9, 0.99, 1.0, 1.5)
    ],
    "tables": [[]],
}
