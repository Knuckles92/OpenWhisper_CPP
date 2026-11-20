"""Curated inputs for ui_qt/overlays/recording_looks.py (fv_uimath overlay_look).

hue_color takes the theme name first and returns the colour's [r, g, b, a]; lerp_point takes and
returns [x, y]. "tables" is every constant the C++ side keeps.
"""

FLOAT_TOLERANCE = 1e-12


def _hue_color(module, theme, hue, alpha=255, bright=True):
    from ui_qt.utils.palette import palette_for, set_current_palette

    set_current_palette(palette_for(theme))
    return list(module.hue_color(hue, alpha, bright=bright).getRgb())


def _lerp_point(module, a, b, t):
    from PyQt6.QtCore import QPointF

    point = module.lerp_point(QPointF(*a), QPointF(*b), t)
    return [point.x(), point.y()]


def _tables(module):
    return {
        "looks": list(module.LOOKS),
        "default": module.DEFAULT,
        "aliases": dict(module._ALIASES),
        "center_y": module.CENTER_Y,
        "morph_s": module.MORPH_S,
        "squash": module.SQUASH,
        "shrink": module.SHRINK,
        "fit": module.FIT,
    }


WRAPPERS = {"hue_color": _hue_color, "lerp_point": _lerp_point, "tables": _tables}

CASES = {
    "normalize_look": (
        [[look] for look in ("ribbon", "bars", "wave", "aurora", "pulse", "dots", "baitball",
                             "fish", "glow", "fizz", "legacy")]
        + [[alias] for alias in ("live", "orb", "bubbles")]
        + [[value] for value in ("", "Ribbon", "BARS", " bars", "bars ", "classic", "waveform",
                                 "bait ball", "ribbon\n", "legacy2", "wave​")]
        + [[None], [0], [1], [3.5], [True]]
    ),
    "smooth": [[t] for t in (-10.0, -1e-9, 0.0, 1e-9, 0.1, 0.25, 0.3333, 0.5, 0.6, 0.75, 0.9,
                             0.999999, 1.0, 1.0000001, 1.6, 5.0, float("nan"), float("inf"),
                             float("-inf"))],
    "hue_color": (
        [[theme, float(hue)] for theme in ("dark", "light") for hue in range(0, 360, 3)]
        + [["dark", float(hue), 255, False] for hue in range(0, 360, 5)]
        + [["dark", 200.0, alpha] for alpha in (-5, 0, 0.9, 1, 99.99, 128, 254.9, 255, 256, 1e9)]
        + [["light", 200.0, alpha] for alpha in (-5, 0, 0.9, 99.99, 255, 300)]
        + [[theme, hue] for theme in ("dark", "light")
           for hue in (-0.5, -1.0, -30.5, -359.9, -360.0, -721.0, 359.99, 360.0, 360.5, 719.9, 725.9,
                       1e6, 190.0, 245.0, 60.0 * 5.5, 12.9999999)]
        + [["light", 40.0, 150.5, False], ["dark", 40.0, 150.5, False]]
    ),
    "lerp_point": [
        [[0.0, 0.0], [10.0, 20.0], t] for t in (-1.0, 0.0, 0.25, 0.5, 0.9, 1.0, 2.0)
    ] + [[[3.5, -7.25], [-1.5, 8.0], 0.3], [[1.0, 1.0], [1.0, 1.0], 0.7]],
    "tables": [[]],
}


# --- M2.7: the looks' state and geometry (fv_uimath look_sims) -----------------------------------
#
# Aurora, Pulse and Dots run on a stub overlay whose ribbon says what shown and history it has.
# The drawing functions run through a recording painter and report what they drew, in order, as
#   ["line", x1, y1, x2, y2, pen]
#   ["ellipse", cx, cy, rx, ry, pen, brush]
#   ["rrect", x, y, width, height, rx, ry, pen, brush]
#   ["path", elements, pen, brush]      drawPath, elements as [x, y] pairs
#   ["fillpath", elements, brush]
# where pen is None (no pen) or [width, colour] and brush is None, an [r, g, b, a] colour or a
# gradient {"kind", "start", "stop", "stops": [[position, [r, g, b, a]]], ...}; a pen's colour
# may be a gradient. Looks take their hue from the case, and are as wide as the case says.

import math as _math
from types import SimpleNamespace as _Namespace


class _Recorder:
    def __init__(self):
        self.ops = []
        self._pen = None
        self._brush = None

    @staticmethod
    def _stops(gradient):
        return [[position, list(color.getRgb())] for position, color in gradient.stops()]

    def _gradient(self, gradient):
        from PyQt6.QtGui import QConicalGradient

        if isinstance(gradient, QConicalGradient):
            return {"kind": "conical", "center": [gradient.center().x(), gradient.center().y()],
                    "angle": gradient.angle(), "stops": self._stops(gradient)}
        return {"kind": "linear", "start": [gradient.start().x(), gradient.start().y()],
                "stop": [gradient.finalStop().x(), gradient.finalStop().y()],
                "stops": self._stops(gradient)}

    def setPen(self, pen):
        from PyQt6.QtGui import QPen

        if not isinstance(pen, QPen):
            self._pen = None
            return
        gradient = pen.brush().gradient()
        colour = self._gradient(gradient) if gradient is not None else list(pen.color().getRgb())
        self._pen = [pen.widthF(), colour]

    def setBrush(self, brush):
        from PyQt6.QtGui import QColor, QGradient

        if isinstance(brush, QColor):
            self._brush = list(brush.getRgb())
        elif isinstance(brush, QGradient):
            self._brush = self._gradient(brush)
        else:
            self._brush = None

    @staticmethod
    def _elements(path):
        return [[path.elementAt(i).x, path.elementAt(i).y] for i in range(path.elementCount())]

    def drawLine(self, a, b):
        self.ops.append(["line", a.x(), a.y(), b.x(), b.y(), self._pen])

    def drawEllipse(self, center, rx, ry):
        self.ops.append(["ellipse", center.x(), center.y(), rx, ry, self._pen, self._brush])

    def drawRoundedRect(self, rect, rx, ry):
        self.ops.append(["rrect", rect.x(), rect.y(), rect.width(), rect.height(), rx, ry,
                         self._pen, self._brush])

    def drawPath(self, path):
        self.ops.append(["path", self._elements(path), self._pen, self._brush])

    def fillPath(self, path, brush):
        from PyQt6.QtGui import QColor

        fill = list(brush.getRgb()) if isinstance(brush, QColor) else self._gradient(brush)
        self.ops.append(["fillpath", self._elements(path), fill])


def _visual(module, name, theme, hue, width=300, shown=0.0, history=None, snapshot=None,
            height=80):
    from ui_qt.utils.palette import palette_for, set_current_palette

    set_current_palette(palette_for(theme))
    overlay = _Namespace(
        width=lambda: width, overlay_width=width, _base_height=height,
        _ribbon=_Namespace(shown=shown, history=list(history or [0.0] * 100)),
        style=_Namespace(animation_time=0.0, color_shift_speed=50.0))
    visual = getattr(module, name)(overlay)
    visual.hue = lambda: hue
    if snapshot is not None:
        visual.snapshot = list(snapshot)
    return visual


def _heard_ribbon(levels):
    from ui_qt.overlays import live_recording

    ribbon = live_recording.VoiceRibbon()
    for level in levels:
        ribbon.hear(level)
        ribbon.advance(1 / 30)
    return ribbon


def _draw(module, name, theme, hue, width, shown, history, snapshot, grow):
    visual = _visual(module, name, theme, hue, width, shown, history, snapshot)
    if name == "MirrorWave":
        # The wave reads the ribbon's own interpolation: history is the levels it heard.
        visual.overlay._ribbon = _heard_ribbon(history)
    recorder = _Recorder()
    visual.draw(recorder, grow)
    return recorder.ops


def _draw_work(module, name, theme, hue, width, shown, history, snapshot, morph, clock, state):
    visual = _visual(module, name, theme, hue, width, shown, history, snapshot)
    for key, value in state.items():
        setattr(visual, key, value)
    recorder = _Recorder()
    visual.draw_work(recorder, morph, clock)
    return recorder.ops


def _aurora(module, frames, work, theme, hue, width, grow, morph, clock):
    visual = _visual(module, "Aurora", theme, hue, width)
    ribbon = visual.overlay._ribbon
    seen = []
    for level, dt in frames:
        ribbon.shown = level
        visual.advance(dt)
        seen.append([visual._amplitude, visual._phase])
    recorder = _Recorder()
    visual.draw(recorder, grow)
    out = {"frames": seen, "draw": recorder.ops}
    for dt, rate in work:
        visual.advance_work(dt, rate)
        seen.append([visual._amplitude, visual._phase])
    recorder = _Recorder()
    visual.draw_work(recorder, morph, clock)
    out["work"] = recorder.ops
    return out


def _pulse(module, frames, work, theme, hue, width, grow, morph, clock, history):
    visual = _visual(module, "Pulse", theme, hue, width, history=history)
    ribbon = visual.overlay._ribbon
    seen = []
    for level, dt in frames:
        ribbon.shown = level
        visual.advance(dt)
        seen.append([list(visual._ripples), visual._time])
    recorder = _Recorder()
    visual.draw(recorder, grow)
    out = {"frames": seen, "draw": recorder.ops}
    visual.begin_work()
    for dt, rate in work:
        visual.advance_work(dt, rate)
        seen.append([list(visual._ripples), visual._time])
    recorder = _Recorder()
    visual.draw_work(recorder, morph, clock)
    out["work"] = recorder.ops
    return out


def _snap_at(module, values, f):
    visual = _visual(module, "Dots", "dark", 0.0, snapshot=values)
    return visual.snap_at(f)


def _dots_level(module, values, k):
    visual = _visual(module, "Dots", "dark", 0.0)
    return visual._level(values, k)


def _emitter(module, history, xs):
    ribbon = _heard_ribbon(history)
    overlay = _Namespace(overlay_width=300, _base_height=80, _ribbon=ribbon, width=lambda: 300)
    wave = module.MirrorWave(overlay).emitter()
    crest = module.Ribbon(overlay).emitter()
    return [[wave(x), crest(x)] for x in xs]


WRAPPERS.update({
    "Visual.draw": _draw,
    "Visual.draw_work": _draw_work,
    "Aurora": _aurora,
    "Pulse": _pulse,
    "Visual.snap_at": _snap_at,
    "Dots._level": _dots_level,
    "emitters": _emitter,
})

_HISTORY = [0.0] * 20 + [round(0.5 + 0.5 * _math.sin(i * 0.37), 6) * (1 - i / 120) for i in range(80)]
_SNAP = [round(0.5 + 0.45 * _math.sin(i * 0.21 + 1.0), 6) for i in range(100)]
_THEMES = ("dark", "light")
_LEVELS = ([[0.05, 1 / 30]] * 12 + [[0.7, 1 / 30]] * 3 + [[0.2, 1 / 30]] * 5 + [[0.9, 1 / 30]] * 4
           + [[0.9, 1 / 30]] * 8 + [[0.1, 1 / 30], [0.6, 0.0], [0.95, 1 / 60], [0.3, 0.2]] * 3)
_RIPPLES = ([[0.0, 1 / 30]] * 20 + [[0.8, 1 / 30]] * 3 + [[0.2, 1 / 30]] * 3 + [[0.9, 1 / 30]] * 2
            + [[0.55, 0.05], [0.2, 0.05], [0.9, 0.05]] * 4)
_WORK_FRAMES = [[1 / 30, 1.0]] * 10 + [[1 / 30, 1.7]] * 10
_MORPH_CLOCK = [(0.0, 0.0), (0.25, 0.4), (0.5, 1.1), (1.0, 2.3), (1.0, 6.5)]

CASES.update({
    "Visual.draw": (
        [["Bars", theme, hue, width, 0.4, _HISTORY, None, grow]
         for theme in _THEMES for hue in (0.0, 123.5, 359.0) for width in (300, 120, 33.3)
         for grow in (1.0, 0.5, 0.0)]
        + [["Bars", "dark", 40.0, 300, 0.4, [0.0] * 3, None, 1.0],
           ["Bars", "dark", 40.0, 300, 0.4, [0.7], None, 0.8],
           ["Bars", "dark", 40.0, 10, 0.4, _HISTORY, None, 0.8]]
        + [["MirrorWave", theme, hue, 300, 0.4, _HISTORY, None, grow]
           for theme in _THEMES for hue in (0.0, 200.0) for grow in (1.0, 0.3, 0.0)]
        + [["Dots", theme, hue, width, 0.4, _HISTORY, None, grow]
           for theme in _THEMES for hue in (0.0, 77.0) for width in (300, 100)
           for grow in (1.0, 0.99, 0.7, 0.4, 0.1, 0.0)]
        + [["Dots", "dark", 10.0, 300, 0.4, [0.5], None, 0.5]]
    ),
    "Visual.draw_work": (
        [[name, theme, hue, 300, 0.4, _HISTORY, _SNAP, morph, clock, {}]
         for name in ("Bars", "MirrorWave", "Dots") for theme in _THEMES for hue in (0.0, 211.0)
         for morph, clock in _MORPH_CLOCK]
        + [["Ribbon", theme, hue, width, 0.4, _HISTORY, _SNAP, morph, clock, {}]
           for theme in _THEMES for hue in (0.0, 211.0) for width in (300, 90)
           for morph, clock in _MORPH_CLOCK]
    ),
    "Aurora": [[_LEVELS, _WORK_FRAMES, theme, hue, 300, grow, morph, clock]
               for theme in _THEMES for hue in (0.0, 150.0) for grow in (1.0, 0.4)
               for morph, clock in _MORPH_CLOCK[1:]] + [[[], [], "dark", 10.0, 120, 1.0, 0.0, 0.0]],
    "Pulse": ([[_LEVELS, _WORK_FRAMES, theme, hue, 300, grow, morph, clock, _SNAP]
               for theme in _THEMES for hue in (0.0, 150.0) for grow in (1.0, 0.4)
               for morph, clock in _MORPH_CLOCK[1:]]
              + [[_RIPPLES, [], "dark", 10.0, 300, 0.7, 0.0, 0.0, _SNAP],
                 [_RIPPLES, [[0.1, 1.0]] * 9, "light", 99.0, 300, 1.0, 0.6, 1.5, _SNAP]]),
    "Visual.snap_at": ([[_SNAP, f] for f in (-1.0, 0.0, 0.001, 0.25, 0.5, 0.73, 0.999, 1.0, 2.0)]
                       + [[[0.0, 0.0], 0.5], [[0.2, 0.9], 0.33], [[0.4], 0.9]]),
    "Dots._level": ([[_SNAP, k] for k in range(5)] + [[[0.3], k] for k in range(5)]
                    + [[[0.1, 0.2, 0.3, 0.4, 0.5, 0.6, 0.7, 0.8, 0.9, 1.0, 1.1], k]
                       for k in range(5)]),
    "emitters": [[[0.0] * 10 + [0.06] * 20 + [0.002] * 6, [0, 14, 50, 150, 286, 300]]],
})
