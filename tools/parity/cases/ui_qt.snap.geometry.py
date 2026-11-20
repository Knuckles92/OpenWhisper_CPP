"""Curated inputs for ui_qt/snap/geometry.py (fv_ui_snap geometry). Rectangles are
[x, y, width, height], points [x, y] and sizes [width, height]; the wrappers build QRect, QPoint,
QPointF and QSize from them and return the same lists (None for no grab)."""


def _rect(values):
    from PyQt6.QtCore import QRect

    return QRect(*values)


def _rect_list(rect):
    return [rect.x(), rect.y(), rect.width(), rect.height()]


def _span(module, a, b):
    from PyQt6.QtCore import QPoint

    return _rect_list(module.span(QPoint(*a), QPoint(*b)))


def _snap_point(module, point, targets):
    from PyQt6.QtCore import QPointF

    snapped = module.snap_point(QPointF(*point), [_rect(t) for t in targets])
    return [snapped.x(), snapped.y()]


def _grab_at(module, selection, point):
    from PyQt6.QtCore import QPoint

    grab = module.grab_at(_rect(selection), QPoint(*point))
    return None if grab is None else list(grab)


def _resized(module, selection, grab, point):
    from PyQt6.QtCore import QPoint

    return _rect_list(module.resized(_rect(selection), tuple(grab), QPoint(*point)))


def _device_rect(module, selection, dpr, image_rect):
    return _rect_list(module.device_rect(_rect(selection), dpr, _rect(image_rect)))


def _place_toolbar(module, size, selection, bounds, gap=10, margin=8):
    from PyQt6.QtCore import QSize

    corner = module.place_toolbar(QSize(*size), _rect(selection), _rect(bounds), gap=gap,
                                  margin=margin)
    return [corner.x(), corner.y()]


def _badge_rect(module, size, selection, bounds, gap=6):
    from PyQt6.QtCore import QSize

    return _rect_list(module.badge_rect(QSize(*size), _rect(selection), _rect(bounds), gap=gap))


WRAPPERS = {
    "span": _span,
    "snap_point": _snap_point,
    "grab_at": _grab_at,
    "resized": _resized,
    "device_rect": _device_rect,
    "place_toolbar": _place_toolbar,
    "badge_rect": _badge_rect,
}

SELECTION = [100, 100, 200, 100]
THIN = [100, 100, 200, 6]
SCREEN = [0, 0, 1920, 1080]
WINDOWS = [[0, 0, 1920, 1080], [100, 100, 400, 300], [480, 380, 300, 200]]
# Around the selection's corners and edges: outside reach, the grab bands and the corner reach.
GRAB_XS = [89, 90, 92, 100, 104, 105, 116, 117, 200, 283, 284, 296, 300, 304, 310, 311]
GRAB_YS = [89, 90, 95, 100, 104, 105, 116, 117, 150, 183, 184, 196, 200, 204, 210, 211]
DEVICE_SELECTIONS = [[10, 20, 100, 50], [1, 1, 3, 3], [0, 0, 1, 1], [7, 3, 101, 33],
                     [390, 290, 50, 50], [-5, -5, 20, 20], [0, 0, 400, 300], [33, 17, 0, 0]]
TOOLBAR = [400, 46]
BOUNDS = [0, 0, 1000, 800]
WIDE = [0, 0, 2000, 800]

CASES = {
    "span": [
        [[110, 60], [10, 10]], [[10, 60], [110, 10]], [[10, 10], [110, 60]],
        [[110, 10], [10, 60]], [[5, 5], [5, 5]], [[-20, 30], [40, -10]],
    ],
    "snap_point": [
        [point, WINDOWS] for point in (
            [5.5, 3.2], [95.0, 103.0], [108.0, 250.0], [300.0, 395.5], [511.0, 410.0],
            [1000.0, 500.0], [1915.0, 1075.0], [-12.0, 5.0], [-9.5, 540.0], [490.0, 391.0],
            [490.0, 381.0], [505.0, 395.0], [782.5, 578.0], [790.0, 590.0], [110.0, 110.0],
            [300.0, 250.0], [100.0, 90.0], [89.9, 100.0], [489.99, 380.01], [1930.0, 1090.0],
        )
    ] + [
        [[5.0, 5.0], []], [[50.0, 50.0], [[0, 0, 100, 100]]], [[50.0, 5.0], [[0, 0, 100, 100]]],
        [[10.0, 10.0], [[0, 0, 20, 20]]], [[3.0, 3.0], [[0, 0, 0, 0]]],
        [[200.0, 104.0], [[0, 0, 400, 100], [0, 110, 400, 100]]],
    ],
    "grab_at": [[SELECTION, [x, y]] for x in GRAB_XS for y in GRAB_YS] + [
        [SELECTION, [92, 92]], [SELECTION, [310, 95]], [SELECTION, [303, 150]],
        [SELECTION, [80, 80]], [SELECTION, [200, 220]], [SELECTION, [106, 150]],
        [THIN, [200, 101]], [THIN, [200, 105]], [THIN, [200, 103]], [THIN, [200, 102]],
        [THIN, [200, 92]], [THIN, [200, 112]], [THIN, [100, 103]], [THIN, [300, 104]],
        [[100, 100, 6, 100], [101, 150]], [[100, 100, 6, 100], [105, 150]],
        [[100, 100, 6, 100], [103, 150]], [[100, 100, 6, 6], [103, 103]],
    ],
    "resized": [
        [SELECTION, [1, 0], [400, 0]], [SELECTION, [-1, -1], [50, 60]],
        [SELECTION, [-1, 0], [350, 0]], [SELECTION, [0, 1], [0, 50]],
        [SELECTION, [1, 1], [40, 30]], [SELECTION, [0, -1], [0, 260]],
        [SELECTION, [1, -1], [300, 200]], [SELECTION, [-1, 1], [301, 99]],
        [SELECTION, [0, 0], [0, 0]], [THIN, [0, 1], [0, 90]],
    ],
    "device_rect": [
        [selection, dpr, [0, 0, round(400 * dpr), round(300 * dpr)]]
        for dpr in (1.0, 1.25, 1.5, 1.75, 2.0)
        for selection in DEVICE_SELECTIONS
    ] + [
        [[10, 20, 100, 50], 1.5, [0, 0, 1200, 900]], [[390, 290, 50, 50], 2.0, [0, 0, 800, 600]],
        [[20, 10, 100, 60], 1.25, [0, 0, 500, 375]], [[3, 3, 7, 7], 1.25, [0, 0, 20, 20]],
    ],
    "place_toolbar": [
        [TOOLBAR, [300, 100, 400, 200], BOUNDS, 10],
        [TOOLBAR, [300, 500, 400, 300], BOUNDS, 10],
        [TOOLBAR, BOUNDS, BOUNDS, 10, 8],
        [TOOLBAR, [0, 100, 60, 60], BOUNDS, 10, 8],
        [TOOLBAR, [940, 100, 60, 60], BOUNDS],
        [TOOLBAR, [300, 0, 400, 760], BOUNDS],
        [TOOLBAR, [300, 30, 400, 720], BOUNDS],
        [TOOLBAR, [300, 60, 400, 700], BOUNDS],
        [TOOLBAR, [300, 736, 400, 64], BOUNDS],
        [[401, 46], [500, 100, 60, 60], WIDE],
        [[401, 46], [500, 100, 61, 60], WIDE],
        [[400, 47], [500, 100, 61, 60], WIDE],
        [[1200, 46], [100, 100, 200, 100], BOUNDS],
        [[400, 900], [100, 100, 200, 100], BOUNDS],
        [TOOLBAR, [1100, 200, 400, 200], [1000, 0, 1600, 900]],
        [TOOLBAR, [-1200, -100, 300, 200], [-1280, -200, 1280, 1024]],
        [TOOLBAR, [300, 100, 400, 200], BOUNDS, 0, 0],
        [TOOLBAR, [300, 100, 400, 200], BOUNDS, 30, 20],
    ],
    "badge_rect": [
        [[80, 22], [100, 200, 300, 100], BOUNDS],
        [[80, 22], [100, 2, 300, 100], BOUNDS],
        [[80, 22], [100, 31, 300, 100], BOUNDS],
        [[80, 22], [100, 32, 300, 100], BOUNDS],
        [[80, 22], [0, 200, 300, 100], BOUNDS],
        [[80, 22], [960, 200, 40, 100], BOUNDS],
        [[80, 22], [-40, 200, 300, 100], BOUNDS],
        [[80, 22], [1100, 40, 300, 100], [1000, 0, 1600, 900]],
        [[80, 22], [100, 200, 300, 100], BOUNDS, 0],
        [[1200, 22], [100, 200, 300, 100], BOUNDS],
    ],
}
