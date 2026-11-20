"""Curated inputs for ui_qt/overlays/live_recording.py (fv_uimath VoiceRibbon, format_clock,
particle_hue).

VoiceRibbon is fed [levels, dt] frames: every level is heard, then the frame advances. The
result holds, per frame, the meter's [loudness, shown], then the history, height_at and crest
after the last frame. VoiceRibbon.draw runs against a recording painter and reports the shape it
would paint (the top edge, the gradient stops as [position, [r, g, b, a]]), for either theme.
"""
import random

FLOAT_TOLERANCE = 1e-12

SPEECH = 0.06
ROOM = 0.0008


def _ribbon(module, frames, width, xs, baseline):
    ribbon = module.VoiceRibbon()
    seen = []
    for levels, dt in frames:
        for level in levels:
            ribbon.hear(level)
        ribbon.advance(dt)
        seen.append([ribbon.loudness, ribbon.shown])
    return {
        "frames": seen,
        "history": ribbon.history,
        "height_at": [ribbon.height_at(x, width) for x in xs],
        "crest": [ribbon.crest(x, width, baseline) for x in xs],
    }


class _RecordingPainter:
    """Keeps what VoiceRibbon.draw hands the painter."""

    def __init__(self):
        self.pen = None
        self.area = None
        self.fill = None
        self.line = None
        self.line_pen = None

    def setPen(self, pen):
        self.pen = pen

    def setBrush(self, brush):
        pass

    def fillPath(self, path, brush):
        self.area, self.fill = path, brush

    def drawPath(self, path):
        self.line, self.line_pen = path, self.pen


def _stops(gradient):
    return [[position, list(color.getRgb())] for position, color in gradient.stops()]


def _elements(path):
    return [[path.elementAt(i).x, path.elementAt(i).y] for i in range(path.elementCount())]


def _draw(module, frames, width, baseline, hue, grow, theme):
    from ui_qt.utils.palette import palette_for, set_current_palette

    set_current_palette(palette_for(theme))
    ribbon = module.VoiceRibbon()
    for levels, dt in frames:
        for level in levels:
            ribbon.hear(level)
        ribbon.advance(dt)
    painter = _RecordingPainter()
    ribbon.draw(painter, width, baseline, hue, grow)
    if painter.line is None:
        return None
    area = _elements(painter.area)
    pen_brush = painter.line_pen.brush().gradient()
    return {
        "top": _elements(painter.line),
        "area_first": area[0],
        "area_last": area[-2],
        "gradient_start": [painter.fill.start().x(), painter.fill.start().y()],
        "gradient_end": [painter.fill.finalStop().x(), painter.fill.finalStop().y()],
        "fill": _stops(painter.fill),
        "edge": _stops(pen_brush),
        "edge_start": [pen_brush.start().x(), pen_brush.start().y()],
        "edge_end": [pen_brush.finalStop().x(), pen_brush.finalStop().y()],
        "pen_width": painter.line_pen.widthF(),
    }


def _particle_hue(module, animation_time, color_shift_speed):
    from types import SimpleNamespace

    return module.particle_hue(SimpleNamespace(animation_time=animation_time,
                                               color_shift_speed=color_shift_speed))


WRAPPERS = {"VoiceRibbon": _ribbon, "VoiceRibbon.draw": _draw, "particle_hue": _particle_hue}


def _frames(seed, count, dts, speech_every):
    rng = random.Random(seed)
    frames = []
    for i in range(count):
        dt = dts[i % len(dts)]
        if i % speech_every < speech_every // 2:
            levels = [rng.choice([SPEECH, 0.12, 0.02, ROOM]) * rng.uniform(0.5, 1.5)
                      for _ in range(rng.randint(1, 3))]
        else:
            levels = [ROOM * rng.uniform(0.0, 1.0)]
        frames.append([levels, dt])
    return frames


_XS = [0, 7, 14, 14.5, 20, 77.7, 150, 222.3, 285, 286, 300, 400]

_SILENCE_THEN_SPEECH = ([[[ROOM], 1 / 30]] * 60) + ([[[SPEECH], 1 / 30]] * 30)
_SPEECH_THEN_SILENCE = ([[[0.1], 1 / 60]] * 90) + ([[[0.0], 1 / 60]] * 200)
_STALLS = [[[0.05], 0.5], [[0.0], 0.001], [[0.2], 0.0], [[0.0], 3.0], [[0.01], 0.0219], [[0.0], 0.022],
           [[0.3, 0.001, 0.05], 0.011]]

_DRAW_FRAMES = _SILENCE_THEN_SPEECH + ([[[0.01], 1 / 30]] * 12)

CASES = {
    "VoiceRibbon": [
        [[], 300, _XS, 50],
        [_SILENCE_THEN_SPEECH, 300, _XS, 50],
        [_SPEECH_THEN_SILENCE, 300, _XS, 50],
        [_STALLS, 300, _XS, 50],
        [_frames(1, 200, [1 / 30, 1 / 60, 0.017, 0.041], 40), 300, _XS, 50],
        [_frames(2, 160, [1 / 30], 25), 120, [0, 14, 20, 60, 106, 107, 200], 80],
        [_frames(3, 120, [0.1, 0.0331], 30), 28, [0, 14, 20, 28], 30],
        [_frames(4, 90, [1 / 30], 20), 10, [0, 5, 14, 20], 30],
        [_frames(5, 300, [1 / 45], 60), 640, [14, 100, 313.5, 626, 640], 100],
        [[[[1.0], 1 / 30], [[-0.5], 1 / 30], [[5.0], 1 / 30], [[float("nan")], 1 / 30]], 300,
         [14, 150, 286], 50],
    ],
    "VoiceRibbon.draw": [
        [_DRAW_FRAMES, 300, 50, 0.0, 1.0, "dark"],
        [_DRAW_FRAMES, 300, 50, 0.0, 1.0, "light"],
        [_DRAW_FRAMES, 300, 50, 123.4, 0.5, "dark"],
        [_DRAW_FRAMES, 300, 50, 355.5, 0.25, "light"],
        [_DRAW_FRAMES, 300, 50, 700.0, 1.0, "dark"],
        [_DRAW_FRAMES, 300, 50, -45.0, 0.9, "dark"],
        [_DRAW_FRAMES, 640, 51.5, 210.0, 0.37, "light"],
        [_DRAW_FRAMES, 300, 50, 10.0, 0.0, "dark"],
        [_DRAW_FRAMES, 300, 50, 10.0, -1.0, "dark"],
        [[], 300, 50, 80.0, 1.0, "dark"],
    ],
    "format_clock": [
        [0], [0.0], [0.4], [0.999], [1], [1.9], [9], [10], [59], [59.99], [60], [61.5], [125],
        [599.9], [600], [3599], [3600], [3661], [6000.5], [86399], [100000], [-1], [-0.5], [-61],
        [-1e9], [1e6], [4.9999999999],
    ],
    "particle_hue": [
        [0.0, 50], [0.5, 50], [1.0, 50], [7.2, 50], [7.21, 50], [100.0, 50], [3.3333, 50],
        [0.0, 0], [2.0, 0], [5.5, 120.5], [1000.0, 50], [0.01, 50],
    ],
}
