"""Curated inputs for ui_qt/widgets/capture_delay.py (fv_ui_snap CaptureDelay): what the chip
paints for a delay and a countdown (its text and the countdown arc, read off a recording
painter), the invalid-setting fallback of walkthrough_capture_delay, and the arrow-key steps
through the six presets. The chip saves through settings_manager, which is replaced by a stub that
saves nothing, and the icon painter by one that paints nothing.
"""

import os

_APP = []

DELAYS = [0, 1, 2, 3, 5, 10]
REMAINING = [0, 0.0, -1.0, 0.01, 0.04, 0.05, 0.06, 0.15, 0.25, 0.35, 1.0, 1.04, 1.05, 1.25, 2.4,
             2.45, 2.55, 3.0, 9.95, 9.96, 10.0]
TOTALS = [0, 0.0, 1, 2.4, 3, 5, 10]


def _app():
    os.environ.setdefault("QT_QPA_PLATFORM", "offscreen")
    os.environ.setdefault("QT_QPA_FONTDIR",
                          os.path.join(os.environ.get("WINDIR", r"C:\Windows"), "Fonts"))
    os.environ.setdefault("QT_FONT_DPI", "96")
    from PyQt6.QtWidgets import QApplication
    if not _APP:
        # Held for the process: a collected QApplication takes every widget down with it.
        _APP.append(QApplication.instance() or QApplication([]))
    return _APP[0]


class _NoSettings:
    def save_setting(self, key, value):
        pass


def _chip(module, seconds):
    _app()
    module.settings_manager = _NoSettings()
    chip = module.CaptureDelay()
    chip.set_seconds(seconds)
    return chip


def _painted(module, seconds, remaining, total):
    """The chip's text and its countdown arc span (None when no arc is drawn)."""
    from PyQt6.QtGui import QPainter

    record = {"text": None, "arc": None}

    class Recorder:
        RenderHint = QPainter.RenderHint

        def __init__(self, *args):
            pass

        def drawText(self, *args):
            record["text"] = args[-1]

        def drawArc(self, rect, start, span):
            record["arc"] = [start, span]

        def __getattr__(self, name):
            return lambda *args, **kwargs: None

    chip = _chip(module, seconds)
    chip.set_countdown(remaining, total)
    real_painter, real_icon = module.QPainter, module.paint_icon
    module.QPainter, module.paint_icon = Recorder, lambda *args: None
    try:
        chip.paintEvent(None)
    finally:
        module.QPainter, module.paint_icon = real_painter, real_icon
        chip.deleteLater()
    return record


def _arrow(module, seconds, key):
    """The delay an arrow key (Left, Right, Up or Down) moves the chip to."""
    from PyQt6.QtCore import QEvent, Qt
    from PyQt6.QtGui import QKeyEvent

    chip = _chip(module, seconds)
    try:
        chip.keyPressEvent(QKeyEvent(QEvent.Type.KeyPress, getattr(Qt.Key, f"Key_{key}"),
                                     Qt.KeyboardModifier.NoModifier))
        return chip.seconds
    finally:
        chip.deleteLater()


def _resolve(module, raw):
    from services.settings import SettingsKey

    return module.resolve_walkthrough_capture_delay({SettingsKey.WALKTHROUGH_CAPTURE_DELAY: raw})


WRAPPERS = {
    "paint": _painted,
    "arrow": _arrow,
    "resolve_walkthrough_capture_delay": _resolve,
}

CASES = {
    "paint": [[seconds, remaining, total] for seconds in (0, 3, 10) for remaining in REMAINING
              for total in TOTALS],
    "arrow": [[seconds, key] for seconds in DELAYS for key in ("Left", "Right", "Up", "Down")],
    "resolve_walkthrough_capture_delay": [[raw] for raw in (
        None, True, False, "3", "", -1, 30, 0, 1, 2, 3, 4, 5, 10, 3.0, 2.5, [3], {"s": 3})],
}
