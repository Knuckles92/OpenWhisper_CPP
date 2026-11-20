"""Curated inputs for ui_qt/widgets/flow_layout.py FlowLayout._arrange (fv_ui_widgets
flow_arrange and flow_height).

_arrange works on QLayoutItems; the wrapper runs it on a stand-in self holding stand-in items
of fixed size hints, which record the geometry the layout gives them:

    args:   [sizes, width, spacing]   [w, h] per item, null for an empty (hidden) item
    result: {"height": what _arrange returns, "rects": [[x, y, w, h] or null per item]}

The rectangle starts at the origin, as heightForWidth's does; setGeometry offsets every item by
its rectangle's corner. Empty items are skipped and keep null.
"""
from types import SimpleNamespace

from PyQt6.QtCore import QRect, QSize


class _Item:
    def __init__(self, size):
        self._size = size
        self.rect = None

    def isEmpty(self):
        return self._size is None

    def sizeHint(self):
        return QSize(*self._size)

    def setGeometry(self, rect):
        self.rect = [rect.x(), rect.y(), rect.width(), rect.height()]


def _arrange(module, sizes, width, spacing):
    items = [_Item(size) for size in sizes]
    stub = SimpleNamespace(_items=items, _gap=spacing)
    height = module.FlowLayout._arrange(stub, QRect(0, 0, width, 0), move=True)
    return {"height": height, "rects": [item.rect for item in items]}


WRAPPERS = {"_arrange": _arrange}

_CHIPS = [[60, 24], [80, 32], [40, 20], [100, 32], [70, 28]]

CASES = {
    "_arrange": [
        [[], 300, 6], [[None], 300, 6], [[[50, 20]], 300, 6], [[[50, 20]], 30, 6],
        [[[50, 20]], 0, 6],
        # Two items: one line while they fit with the gap, else two lines.
        [[[50, 20], [60, 30]], 116, 6], [[[50, 20], [60, 30]], 115, 6],
        # Five chips of different heights at widths that give 1 to 5 lines.
        [_CHIPS, 400, 6], [_CHIPS, 374, 6], [_CHIPS, 373, 6], [_CHIPS, 300, 6],
        [_CHIPS, 200, 6], [_CHIPS, 150, 6], [_CHIPS, 100, 6], [_CHIPS, 80, 6], [_CHIPS, 50, 6],
        [_CHIPS, 400, 0], [_CHIPS, 300, 8], [_CHIPS, 200, 12],
        # Hidden items take no room and keep no geometry.
        [[[60, 24], None, [80, 32], None, [40, 20]], 150, 6],
        [[None, None, [80, 32]], 100, 6],
        # Odd height differences centre down (floor division).
        [[[30, 21], [30, 10], [30, 15]], 200, 6],
        # An item wider than the line is clamped to it.
        [[[500, 30], [40, 20]], 300, 6], [[[40, 20], [500, 30], [40, 20]], 300, 6],
        [[[0, 0], [0, 0]], 0, 6], [[[10, 10], [10, 10], [10, 10]], 26, 0],
    ],
}
