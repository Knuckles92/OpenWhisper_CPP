"""Curated inputs for ui_qt/widgets/segmented_bar.py SegmentedBar._reflow (fv_ui_widgets
segmented_columns).

_reflow measures the tray and its buttons and then places them; the wrapper runs it on a
stand-in self whose layout and buttons report the given sizes, and records what it decides:

    args:   [n, cell, gap, width]   n choices whose widest natural width is cell, the grid's gap
                                    (also its margin on each side), the tray's width
    result: {"columns": what _place got, "wrap": what set_wrapping got,
             "minimum": what setMinimumWidth got}

The C++ replay calls segmented_columns(n, cell, gap, width - 2 * gap + gap), the room _reflow
computes from those.
"""
from types import SimpleNamespace


class _Margins:
    def __init__(self, margin):
        self._margin = margin

    def left(self):
        return self._margin

    def right(self):
        return self._margin


class _Layout:
    def __init__(self, gap):
        self._gap = gap

    def contentsMargins(self):
        return _Margins(self._gap)

    def horizontalSpacing(self):
        return self._gap


class _Button:
    def __init__(self, natural, record):
        self._natural = natural
        self._record = record

    def natural_width(self):
        return self._natural

    def set_wrapping(self, wrap):
        self._record["wrap"] = wrap

    def setMinimumWidth(self, width):
        self._record["minimum"] = width


def _reflow(module, n, cell, gap, width):
    record = {}
    # Only the widest natural width matters; the others are narrower.
    buttons = [_Button(cell if index == 0 else max(0, cell - index), record)
               for index in range(n)]

    def place(columns):
        record["columns"] = columns

    stub = SimpleNamespace(buttons=buttons, layout=lambda: _Layout(gap), width=lambda: width,
                           _place=place)
    module.SegmentedBar._reflow(stub)
    return record


WRAPPERS = {"_reflow": _reflow}

CASES = {
    "_reflow": (
        # n 1..7 at the 4 px tray (room = width - 4) around the widths where columns change.
        [[n, 120, 4, width] for n in range(1, 8)
         for width in (0, 60, 123, 124, 127, 128, 251, 252, 253, 375, 376, 377, 499, 500, 501,
                       623, 624, 625, 747, 748, 749, 871, 872, 873, 1000)]
        # The compact 3 px tray, and the look picker's 11 choices.
        + [[n, 90, 3, width] for n in (2, 3, 4) for width in (92, 93, 95, 96, 188, 189, 280)]
        + [[11, 140, 4, 640], [11, 140, 4, 300], [11, 140, 4, 140], [11, 140, 4, 147],
           [11, 140, 4, 148], [11, 52, 4, 640], [2, 0, 4, 10]]
    ),
}
