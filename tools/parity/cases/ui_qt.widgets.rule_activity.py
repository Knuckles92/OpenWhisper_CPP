"""Curated inputs for ui_qt/widgets/rule_activity.py (M4.3, fv_ui_personalize rule_activity.h).

    _format_clock(seconds) -> "m:ss"
    _mix(r1, g1, b1, r2, g2, b2, t, alpha) -> [red, green, blue, alpha] of the blend (alpha as
        QColor.alpha(), 0-255); the wrapper builds the two QColors
    bar_heights(ops) -> LevelTrace after a scripted stream, on an offscreen QApplication

The stream drives LevelTrace with a clock the script sets (QElapsedTimer swapped for a stand-in
whose start() and restart() zero it, as the real one does, and whose time is `at`):

    ["listen"]            start_listening()
    ["push", level]       push_level(level)
    ["at", ms]            the clock reads ms, then advance() once
    ["advance"]           advance() once more at the same time (the bars ease further)
    ["hold", state]       hold("transcribing" or "polishing")
    ["clear"]             clear()

and the result is {"bars": bar_heights(), "shown": the eased heights as drawn, oldest first,
"peak": the loudest level of the step in flight, "state": the trace's state}.
"""

import os

# level_to_height goes through pow and log10, which C++ and Python round alike only to about
# 1e-12 (ui_qt.utils.loudness); the bars add nothing to that.
FLOAT_TOLERANCE = 1e-9

_APP = []


def _app():
    # As render_reference.py: offscreen finds no fonts on Windows without the font folder.
    os.environ.setdefault("QT_QPA_PLATFORM", "offscreen")
    os.environ.setdefault("QT_QPA_FONTDIR",
                          os.path.join(os.environ.get("WINDIR", r"C:\Windows"), "Fonts"))
    from PyQt6.QtWidgets import QApplication
    if not _APP:
        # Held for the process: a collected QApplication takes every widget down with it.
        _APP.append(QApplication.instance() or QApplication([]))
    return _APP[0]


class _Clock:
    """A QElapsedTimer whose time the script sets."""

    def __init__(self):
        self.ms = 0
        self.valid = False

    def start(self):
        self.ms = 0
        self.valid = True

    restart = start

    def elapsed(self):
        return self.ms

    def isValid(self):
        return self.valid


def _mix(module, r1, g1, b1, r2, g2, b2, t, alpha):
    start = module.QColor(r1, g1, b1)
    end = module.QColor(r2, g2, b2)
    color = module._mix(start, end, t, alpha)
    return [color.red(), color.green(), color.blue(), color.alpha()]


def _bar_heights(module, ops):
    _app()
    trace = module.LevelTrace()
    trace._clock = _Clock()
    for op in ops:
        name = op[0]
        if name == "listen":
            trace.start_listening()
        elif name == "push":
            trace.push_level(op[1])
        elif name == "at":
            trace._clock.ms = op[1]
            trace.advance()
        elif name == "advance":
            trace.advance()
        elif name == "hold":
            trace.hold(op[1])
        elif name == "clear":
            trace.clear()
        else:
            raise ValueError(name)
    result = {"bars": trace.bar_heights(), "shown": [bar[1] for bar in trace._bars],
              "peak": trace._peak, "state": trace._state}
    trace.deleteLater()
    return result


def _ramp(count, low, high):
    """count steps, one 60 ms step each, the level rising from low to high."""
    ops = [["listen"]]
    for index in range(count):
        level = low + (high - low) * index / max(1, count - 1)
        ops += [["push", level], ["at", 60 * (index + 1)]]
    return ops


def _stream():
    """The reference screenshot's stream: 30 steps of a speech-like level."""
    levels = [0.0, 0.004, 0.02, 0.08, 0.15, 0.06, 0.01, 0.0, 0.03, 0.12, 0.2, 0.09, 0.02, 0.005,
              0.0, 0.05, 0.1, 0.18, 0.07, 0.015, 0.0, 0.02, 0.06, 0.14, 0.11, 0.04, 0.008, 0.0,
              0.03, 0.09]
    ops = [["listen"]]
    for index, level in enumerate(levels):
        ops += [["push", level], ["at", 60 * (index + 1)]]
    return ops


CASES = {
    "_format_clock": [[0], [1], [9], [10], [59], [60], [61], [71], [600], [3599], [3600], [7325]],
    "_mix": [
        [10, 20, 30, 200, 100, 50, 0.0, 1.0],
        [10, 20, 30, 200, 100, 50, 1.0, 1.0],
        [10, 20, 30, 200, 100, 50, 0.5, 0.5],
        [10, 20, 30, 200, 100, 50, -3.0, 2.0],
        [10, 20, 30, 200, 100, 50, 7.0, -1.0],
        [0, 0, 0, 1, 1, 1, 0.5, 1.0],
        [0, 0, 0, 3, 5, 7, 0.5, 0.25],
        [0, 0, 0, 1, 3, 5, 0.5, 0.75],
        [255, 0, 128, 0, 255, 127, 0.25, 0.333],
        [91, 141, 239, 52, 211, 235, 0.31, 0.77],
        [91, 141, 239, 52, 211, 235, 0.99, 0.01],
        [91, 141, 239, 52, 211, 235, 0.123456, 0.9],
    ],
    "bar_heights": [
        [[["listen"], ["push", 0.02], ["push", 0.1], ["push", 0.05], ["at", 60]]],
        [[["listen"], ["push", 0.1], ["at", 60], ["at", 120]]],
        [[["listen"], ["push", 0.1], ["at", 59]]],
        [[["listen"], ["push", 0.1], ["at", 60], ["advance"], ["advance"], ["advance"]]],
        [[["listen"], ["push", 0.1], ["at", 180], ["push", 0.3], ["at", 240]]],
        [[["listen"], ["push", 0.1], ["at", 60], ["hold", "transcribing"], ["push", 0.3],
          ["at", 600]]],
        [[["listen"], ["push", 0.1], ["at", 60], ["advance"], ["hold", "polishing"],
          ["push", 0.3], ["at", 1300]]],
        [[["push", 0.5], ["at", 120]]],
        [[["listen"], ["push", 0.2], ["at", 60], ["clear"], ["push", 0.4], ["at", 240]]],
        [[["listen"], ["push", 0.2], ["at", 60], ["listen"], ["at", 120]]],
        [_ramp(40, 0.0, 0.4)],
        [_ramp(600, 0.001, 0.3)],
        [_stream()],
        [_stream() + [["hold", "polishing"]]],
        [_stream() + [["hold", "transcribing"], ["advance"], ["advance"]]],
    ],
}

WRAPPERS = {"_mix": _mix, "bar_heights": _bar_heights}
