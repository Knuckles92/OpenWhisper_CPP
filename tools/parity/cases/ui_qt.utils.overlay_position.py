"""Curated inputs for ui_qt/utils/overlay_position.py and desktop.on_screen_position
(fv_uimath overlay_position). Rectangles are [x, y, width, height] and points [x, y], turned into
QRect and QPoint by the wrappers; results are [x, y] lists or ints."""


def _rect(values):
    from PyQt6.QtCore import QRect

    return QRect(*values)


def _point(values):
    from PyQt6.QtCore import QPoint

    return QPoint(*values)


def _clamp(module, x, y, width, height, available, margin=8):
    return list(module.clamp_rect_to_available(x, y, width, height, _rect(available), margin))


def _preferred(module, anchor, width, height, available, offset=10, margin=8):
    return list(module.preferred_overlay_position(_point(anchor), width, height,
                                                  _rect(available), offset, margin))


def _max_height(module, anchor, available, configured_max, offset=10, margin=8):
    return module.max_height_for_anchor(_point(anchor), _rect(available), configured_max,
                                        offset, margin)


def _on_screen(module, frame, available):
    from ui_qt.utils.desktop import on_screen_position

    point = on_screen_position(_rect(frame), _rect(available))
    return [point.x(), point.y()]


SCREEN = [0, 0, 1920, 1080]
SECONDARY = [1920, 0, 1600, 900]
NEGATIVE = [-1280, -200, 1280, 1024]

WRAPPERS = {
    "clamp_rect_to_available": _clamp,
    "preferred_overlay_position": _preferred,
    "max_height_for_anchor": _max_height,
    "on_screen_position": _on_screen,
}

CASES = {
    "clamp_rect_to_available": [
        [100, 200, 300, 80, SCREEN], [1900, 1050, 300, 200, SCREEN],
        [-50, -20, 300, 80, SCREEN], [3400, 850, 300, 200, SECONDARY],
        [0, 0, 3000, 2000, SCREEN], [500, 500, 1904, 1064, SCREEN],
        [500, 500, 1905, 1065, SCREEN], [10, 10, 0, 80, SCREEN], [10, 10, 80, -1, SCREEN],
        [10, 10, 80, 80, [0, 0, 0, 0]], [10, 10, 80, 80, [0, 0, 0, 500]],
        [10, 10, 80, 80, [0, 0, 500, 0]], [-2000, -500, 200, 100, NEGATIVE],
        [100, 100, 300, 80, SCREEN, 0], [100, 100, 300, 80, SCREEN, 50],
        [1900, 1050, 300, 200, SCREEN, -5],
    ],
    "preferred_overlay_position": [
        [[100, 100], 300, 80, SCREEN], [[1900, 100], 300, 80, SCREEN],
        [[100, 1040], 300, 200, SCREEN], [[1900, 1040], 300, 200, SCREEN],
        [[200, 1000], 300, 80, SCREEN], [[200, 1000], 300, 400, SCREEN],
        [[1602, 982], 300, 80, SCREEN], [[1603, 983], 300, 80, SCREEN],
        [[3000, 400], 500, 300, SECONDARY], [[-100, -100], 300, 200, NEGATIVE],
        [[960, 540], 2000, 1200, SCREEN], [[100, 100], 300, 80, SCREEN, 0, 0],
        [[100, 100], 300, 80, SCREEN, 25, 16], [[5, 5], 100, 50, [0, 0, 0, 0]],
    ],
    "max_height_for_anchor": [
        [[100, 100], SCREEN, 400], [[100, 200], [0, 0, 1920, 300], 400],
        [[10, 10], [0, 0, 100, 20], 400], [[100, 100], [0, 0, 0, 0], 400],
        [[100, 100], [0, 0, 0, 0], 0], [[100, 100], [0, 0, 500, 0], -3],
        [[100, 100], [0, 0, 500, -10], 50], [[100, 540], SCREEN, 2000],
        [[100, 540], SCREEN, 300, 0, 0], [[100, 1079], SCREEN, 5000],
        [[3000, 450], SECONDARY, 600], [[0, -200], NEGATIVE, 900],
    ],
    "on_screen_position": [
        [[100, 100, 800, 600], SCREEN], [[0, 0, 1920, 1080], SCREEN],
        [[-10, 0, 800, 600], SCREEN], [[1500, 700, 800, 600], SCREEN],
        [[100, 100, 2400, 1300], SCREEN], [[100, 100, 801, 600], [0, 0, 800, 600]],
        [[100, 100, 0, 0], SCREEN], [[100, 100, 0, 50], SCREEN], [[100, 100, 50, 0], SCREEN],
        [[100, 100, 800, 600], [0, 0, 0, 0]], [[2000, 100, 800, 600], SECONDARY],
        [[-1200, -100, 1300, 900], NEGATIVE], [[-1300, 0, 300, 300], NEGATIVE],
        [[100, 100, 801, 601], [0, 0, 800, 600]], [[100, 100, 3, 3], [0, 0, 5, 5]],
        [[1, 1, 3, 3], [0, 0, 5, 5]], [[100, 100, -50, -50], SCREEN],
    ],
}
