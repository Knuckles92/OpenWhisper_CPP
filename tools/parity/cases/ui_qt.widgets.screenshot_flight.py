"""Curated inputs for ui_qt/widgets/screenshot_flight.py (fv_ui_walkthrough's flightCardRect).

    card_rect(progress, source, target)

runs ScreenshotFlight.card_rect unbound on a stub holding the progress and the two rectangles
([x, y, width, height], global coordinates); the result is the card as [x, y, width, height].
No widget is made, so no QApplication is needed.
"""
import types

FLOAT_TOLERANCE = 1e-9


def _card_rect(module, progress, source, target):
    from PyQt6.QtCore import QRectF

    stub = types.SimpleNamespace(progress=progress, source=QRectF(*source),
                                 target=QRectF(*target))
    rect = module.ScreenshotFlight.card_rect(stub)
    return [rect.x(), rect.y(), rect.width(), rect.height()]


def _flight_s(module):
    return module.FLIGHT_S


WRAPPERS = {"card_rect": _card_rect, "FLIGHT_S": _flight_s}

_SCREEN = [0.0, 0.0, 1920.0, 1080.0]
_REGION = [400.0, 300.0, 640.0, 360.0]
_THUMB = [1606.0, 220.0, 252.0, 141.75]
_TALL = [1606.0, 600.0, 106.875, 190.0]
_LEFT_MONITOR = [-1600.0, 120.0, 800.0, 600.0]

CASES = {
    "card_rect": [[p, source, target]
                  for source, target in ((_SCREEN, _THUMB), (_REGION, _THUMB),
                                         (_REGION, _TALL), (_LEFT_MONITOR, _THUMB))
                  for p in (0.0, 0.1, 0.25, 0.5, 0.68, 0.75, 0.9, 1.0)]
                 + [[0.5, [10.0, 10.0, 0.0, 0.0], [10.0, 10.0, 0.0, 0.0]]],
    "FLIGHT_S": [[]],
}
