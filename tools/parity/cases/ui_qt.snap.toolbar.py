"""Curated inputs for ui_qt/snap/toolbar.py (fv_ui_snap toolbar): _layout and _wrap for both
variants. Labels are measured with Segoe UI at a pixel size: the toolbar's label font is the chrome
font of 12 px, which UI text sizes 90, 100, 115 and 130 % make 11, 12, 14 and 16 px. Results are
{"buttons": [[kind, value, x, y, width, height, tip, label], ...], "separators": [[x, top], ...]}.
"""

import os

PIXEL_SIZES = [11, 12, 14, 16]
WIDTHS = [300, 320, 360, 400, 480, 560, 640, 720, 800, 960, 1024, 1280, 1366, 1600, 1920]
SHORTCUTS = [{}, {"keep_marks": "Ctrl+Shift+K", "clear": "Ctrl+Shift+C"}, {"clear": ""}]

_APP = []


def _app():
    # As render_reference.py: offscreen finds no fonts on Windows without the font folder, and
    # the C++ widget tests pin 96 dpi.
    os.environ.setdefault("QT_QPA_PLATFORM", "offscreen")
    os.environ.setdefault("QT_QPA_FONTDIR",
                          os.path.join(os.environ.get("WINDIR", r"C:\Windows"), "Fonts"))
    os.environ.setdefault("QT_FONT_DPI", "96")
    from PyQt6.QtWidgets import QApplication
    if not _APP:
        # Held for the process: a collected QApplication takes every widget down with it.
        _APP.append(QApplication.instance() or QApplication([]))
    return _APP[0]


def _metrics(pixel_size):
    _app()
    from PyQt6.QtGui import QFont, QFontMetrics

    font = QFont("Segoe UI")
    font.setPixelSize(pixel_size)
    return QFontMetrics(font)


def _result(buttons, separators):
    return {
        "buttons": [[b.kind, b.value, b.rect.x(), b.rect.y(), b.rect.width(), b.rect.height(),
                     b.tip, b.label] for b in buttons],
        "separators": [list(separator) for separator in separators],
    }


def _layout(module, live, pixel_size, shortcuts):
    return _result(*module._layout(live, _metrics(pixel_size), shortcuts))


def _wrap(module, live, pixel_size, width):
    buttons, _ = module._layout(live, _metrics(pixel_size), {})
    return _result(*module._wrap(buttons, width))


WRAPPERS = {
    "_layout": _layout,
    "_wrap": _wrap,
}

CASES = {
    "_layout": [[live, size, shortcuts] for live in (False, True) for size in PIXEL_SIZES
                for shortcuts in SHORTCUTS],
    "_wrap": [[live, size, width] for live in (False, True) for size in PIXEL_SIZES
              for width in WIDTHS],
}
