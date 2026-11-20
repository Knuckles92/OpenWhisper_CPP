"""Curated inputs for ui_qt/widgets/button_row.py ButtonRow._row_sizes (fv_ui_widgets
button_row_sizes).

_row_sizes is a method that reads self._spacing and self.width(); the wrapper calls it on a
stand-in self:

    args:   [widths, room, spacing]   the buttons' label widths, the row's width, the gap
    result: the number of buttons in each row, top to bottom
"""
from types import SimpleNamespace


def _row_sizes(module, widths, room, spacing):
    stub = SimpleNamespace(_spacing=spacing, width=lambda: room)
    return module.ButtonRow._row_sizes(stub, list(widths))


WRAPPERS = {"_row_sizes": _row_sizes}

CASES = {
    "_row_sizes": [
        # No buttons, one button.
        [[], 300, 8], [[], 0, 8], [[80], 300, 8], [[80], 80, 8], [[80], 79, 8], [[80], 0, 8],
        # Two buttons: one row while both fit with the gap, else stacked.
        [[100, 120], 228, 8], [[100, 120], 227, 8], [[100, 120], 500, 8], [[100, 120], 50, 8],
        # Three buttons: 1 row, 1 + 2 (the short row first), 1 + 1 + 1.
        [[90, 80, 70], 256, 8], [[90, 80, 70], 255, 8], [[90, 80, 70], 158, 8],
        [[90, 80, 70], 157, 8], [[90, 80, 70], 120, 8], [[90, 80, 70], 89, 8],
        [[90, 80, 70], 300, 0], [[90, 80, 70], 240, 0], [[90, 80, 70], 239, 0],
        # 1 + 2 fails on the second row although the first fits; 2 rows of a wide first button.
        [[60, 150, 150], 300, 8], [[60, 150, 150], 307, 8], [[60, 150, 150], 308, 8],
        [[200, 50, 50], 200, 8], [[200, 50, 50], 108, 8],
        # Four and more: 2 + 2, 1 + 1 + 2, 2 + 3, 3 + 3, 2 + 2 + 3.
        [[80, 80, 80, 80], 344, 8], [[80, 80, 80, 80], 343, 8], [[80, 80, 80, 80], 168, 8],
        [[80, 80, 80, 80], 167, 8], [[80, 80, 80, 80], 80, 8],
        [[60, 60, 60, 60, 60], 340, 8], [[60, 60, 60, 60, 60], 339, 8],
        [[60, 60, 60, 60, 60], 196, 8], [[60, 60, 60, 60, 60], 195, 8],
        [[60, 60, 60, 60, 60], 128, 8], [[60, 60, 60, 60, 60], 127, 8],
        [[50, 50, 50, 50, 50, 50], 196, 8], [[50, 50, 50, 50, 50, 50], 165, 8],
        [[50, 50, 50, 50, 50, 50, 50], 166, 8], [[50, 50, 50, 50, 50, 50, 50], 108, 8],
        # Uneven widths, a 0 room and a negative spacing.
        [[30, 200, 30, 30], 268, 8], [[30, 200, 30, 30], 238, 8], [[30, 200, 30, 30], 0, 8],
        [[100, 100], 190, -10], [[0, 0, 0], 0, 0],
    ],
}
