"""Curated inputs for MainWindow._clamp_geometry and MainWindow._restored_width
(fv_uimath window_geometry). Both are methods, so the wrappers call them on a stub window with
the main window's limits: minimumWidth 650, maximumWidth 1280, minimumHeight 660 (AppConfig's
MAIN_WINDOW_* values). Rectangles are [x, y, width, height], turned into QRect by the wrapper;
_clamp_geometry's result is an [x, y, width, height] list."""

from types import SimpleNamespace

MIN_WIDTH, MAX_WIDTH, MIN_HEIGHT = 650, 1280, 660


def _window():
    return SimpleNamespace(
        minimumWidth=lambda: MIN_WIDTH,
        maximumWidth=lambda: MAX_WIDTH,
        minimumHeight=lambda: MIN_HEIGHT,
        _geometry_format="collapsed_content_v1",
    )


def _clamp(module, x, y, width, height, available):
    from PyQt6.QtCore import QRect

    rect = module.MainWindow._clamp_geometry(_window(), x, y, width, height,
                                             avail=QRect(*available))
    return [rect.x(), rect.y(), rect.width(), rect.height()]


def _restored_width(module, geometry):
    return module.MainWindow._restored_width(_window(), geometry)


SCREEN = [0, 0, 1920, 1080]
LAPTOP = [0, 0, 1280, 800]
SHORT = [0, 0, 1024, 640]
SECONDARY = [1920, 0, 1600, 900]
NEGATIVE = [-1280, -200, 1280, 1024]
TINY = [0, 0, 600, 500]

PINS = {"stub_window": {"minimumWidth": MIN_WIDTH, "maximumWidth": MAX_WIDTH,
                        "minimumHeight": MIN_HEIGHT}}

WRAPPERS = {
    "_clamp_geometry": _clamp,
    "_restored_width": _restored_width,
}

CASES = {
    "_clamp_geometry": [
        [0, 0, 680, 840, LAPTOP],
        [10, 10, 680, 500, SCREEN],
        [10, 10, 500, 680, SCREEN],
        [100, 100, 2000, 2000, SCREEN],
        [-500, -300, 680, 680, SCREEN],
        [5000, 3000, 680, 680, SCREEN],
        [1900, 1050, 680, 680, SCREEN],
        [3400, 850, 680, 680, SECONDARY],
        [100, 100, 680, 680, SECONDARY],
        [-2000, -500, 680, 680, NEGATIVE],
        [-100, 0, 700, 700, NEGATIVE],
        [0, 0, 680, 700, SHORT],
        [500, 300, 680, 640, SHORT],
        [-20, -20, 1036, 900, SHORT],
        [0, 0, 680, 680, TINY],
        [50, 50, 400, 300, TINY],
        [0, 0, 680, 680, [0, 0, 0, 0]],
    ],
    "_restored_width": [
        [{"x": 10, "y": 10, "width": 1036, "height": 680}],
        [{"x": 10, "y": 10, "width": 1036, "height": 680, "format": "collapsed_content_v1"}],
        [{"x": 10, "y": 10, "width": 600, "height": 680, "format": "collapsed_content_v1"}],
        [{"x": 10, "y": 10, "width": 700, "height": 680, "format": "collapsed_content_v1"}],
        [{"x": 10, "y": 10, "width": 700, "height": 680}],
        [{"width": 680, "format": "collapsed_content_v1"}],
        [{"width": 681, "format": "collapsed_content_v1"}],
        [{"width": 1035}],
        [{"width": 1036, "format": "expanded_v0"}],
        [{"width": 1036, "format": None}],
        [{"width": 1500}],
        [{"width": 1700}],
        [{"width": 2000, "format": "collapsed_content_v1"}],
        [{"width": 1280, "format": "collapsed_content_v1"}],
        [{"width": -50, "format": "collapsed_content_v1"}],
        [{"width": 0}],
        [{"width": True, "format": "collapsed_content_v1"}],
        [{"height": 680, "format": "collapsed_content_v1"}],
    ],
}
