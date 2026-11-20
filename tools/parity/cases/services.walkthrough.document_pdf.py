"""Curated inputs for the Qt half of services/walkthrough/document.py (fv_documents, M6.7): the
walkthrough and Markdown composers and their pagination.

The functions live in services.walkthrough.document, whose own case file
(services.walkthrough.document.py) belongs to another package; golden_vectors.py imports the
module named by this file's stem, so this file registers services.walkthrough.document under that
name and its vectors go to tests/golden/services.walkthrough.document_pdf.json.

Everything runs on an offscreen QApplication with the fonts and DPI the C++ tests pin
(QT_QPA_FONTDIR=%WINDIR%\\Fonts, QT_FONT_DPI=96, scale 1), at a layout DPI of 96. Screenshots are
PNGs of one colour written into a scratch folder under out/parity, so only their sizes matter.

    compose(paper, walkthrough, format)
        _Composer(_text_area(page_layout(paper), 96), 96, format).compose(walkthrough)
    compose_markdown(paper, text, images, look)
        _MarkdownComposer(<the same page>, 96, folder, look).compose(text)

paper is "letter", "a4" or [width, height] in layout units. A walkthrough is {"started_at": iso,
"duration_s": seconds, "sections": [[text, shot, kept_at_s]]}, a shot [width, height] or
"unreadable" (a file that is not a PNG) or None; a format is [title, details, about, captions]
or None; images are [[name, width, height]] written into the folder; a look is [text_color,
heading_color, font]. Each result is what the document ends up as:

    pages        doc.pageCount()
    page_starts  for each page, the first block whose first line lies on it (_page_index, the
                 painter's rule), or None when a block from the page before runs onto it
    groups       [first, image, last] of each screenshot section
    frames       [x, y, width, height] of each screenshot, in layout units
    images       [width, height] of every image in the document, in document order
    markers      the text before the tab of every list item (the Markdown composer's markers)

Floats compare within _SLACK (0.5 layout units).
"""

import importlib
import os
import shutil
import sys
import tempfile
from datetime import datetime
from pathlib import Path

sys.modules["services.walkthrough.document_pdf"] = importlib.import_module(
    "services.walkthrough.document")

REPO = Path(__file__).resolve().parents[3]
SCRATCH = REPO / "out" / "parity" / "walkthrough_document_pdf"
DPI = 96

_APP = []


def _app():
    # As fv_testkit_qt's main: offscreen finds no fonts on Windows without the font folder, and
    # the documents size point fonts at the screen's logical DPI.
    os.environ["QT_QPA_PLATFORM"] = "offscreen"
    os.environ["QT_QPA_FONTDIR"] = os.path.join(os.environ.get("WINDIR", r"C:\Windows"), "Fonts")
    os.environ["QT_FONT_DPI"] = "96"
    os.environ["QT_ENABLE_HIGHDPI_SCALING"] = "1"
    os.environ["QT_SCALE_FACTOR"] = "1"
    os.environ.pop("QT_SCREEN_SCALE_FACTORS", None)
    from PyQt6.QtWidgets import QApplication
    if not _APP:
        # Held for the process: the documents need the application's fonts.
        _APP.append(QApplication.instance() or QApplication([]))
    return _APP[0]


def _root() -> Path:
    SCRATCH.mkdir(parents=True, exist_ok=True)
    return Path(tempfile.mkdtemp(dir=SCRATCH))


def _png(path: Path, width: int, height: int) -> Path:
    from PyQt6.QtGui import QColor, QImage
    image = QImage(width, height, QImage.Format.Format_RGB32)
    image.fill(QColor("#3060c0"))
    assert image.save(str(path))
    return path


def _page(module, paper):
    from PyQt6.QtCore import QSizeF
    from PyQt6.QtGui import QPageSize
    if isinstance(paper, list):
        return QSizeF(*paper)
    size = {"letter": QPageSize.PageSizeId.Letter, "a4": QPageSize.PageSizeId.A4}[paper]
    return module._text_area(module.page_layout(QPageSize(size)), DPI)


def _describe(module, doc, groups, frames):
    height = doc.pageSize().height()
    starts = [None] * doc.pageCount()
    images = []
    markers = []
    block = doc.begin()
    while block.isValid():
        layout = block.layout()
        if layout.lineCount():
            origin = module._origin(doc, block)
            page = module._page_index(origin.y() + layout.lineAt(0).y(), height)
            if 0 <= page < len(starts) and starts[page] is None:
                starts[page] = block.blockNumber()
        if block.textList() is not None:
            markers.append(block.text().split("\t", 1)[0])
        iterator = block.begin()
        while not iterator.atEnd():
            char_format = iterator.fragment().charFormat()
            iterator += 1
            if char_format.isImageFormat():
                image = char_format.toImageFormat()
                images.append([image.width(), image.height()])
        block = block.next()
    return {
        "pages": doc.pageCount(),
        "page_starts": starts,
        "groups": [[group.first, group.image, group.last] for group in groups],
        "frames": [[frame.x(), frame.y(), frame.width(), frame.height()] for frame in frames],
        "images": images,
        "markers": markers,
    }


def _compose(module, paper, walkthrough, document_format):
    _app()
    root = _root()
    try:
        sections = []
        for number, (text, shot, kept_at_s) in enumerate(walkthrough["sections"], 1):
            path = None
            if shot == "unreadable":
                path = root / f"shot-{number}.png"
                path.write_bytes(b"not a png")
            elif shot is not None:
                path = _png(root / f"shot-{number}.png", *shot)
            sections.append(module.Section(text, path, kept_at_s))
        value = module.Walkthrough(datetime.fromisoformat(walkthrough["started_at"]),
                                   walkthrough["duration_s"], tuple(sections))
        chosen = module.DocumentFormat() if document_format is None else module.DocumentFormat(
            *document_format)
        composer = module._Composer(_page(module, paper), DPI, chosen)
        doc, frames = composer.compose(value)
        return _describe(module, doc, composer.groups, frames)
    finally:
        shutil.rmtree(root)


def _compose_markdown(module, paper, text, images, look):
    _app()
    root = _root()
    try:
        for name, width, height in images:
            _png(root / name, width, height)
        chosen = module.Look() if look is None else module.Look(*look)
        composer = module._MarkdownComposer(_page(module, paper), DPI, root, chosen)
        doc, groups = composer.layout(text)
        frames = module._paginate(doc, groups)
        return _describe(module, doc, groups, frames)
    finally:
        shutil.rmtree(root)


WRAPPERS = {
    "compose": _compose,
    "compose_markdown": _compose_markdown,
}

STARTED = "2026-10-20T14:32:05"


def _walkthrough(duration, *sections):
    return {"started_at": STARTED, "duration_s": duration, "sections": [list(s) for s in sections]}


_CONTEXT = "Some context before the first screenshot. " * 20
_LONG = "An explanation that keeps going. " * 150
_SECOND = "Some context before the second screenshot. " * 12

# The fixture of the acceptance: three sections with three screenshots, the second too tall to
# stay under the first.
THREE_SHOTS = _walkthrough(
    136.4,
    ("Okay, the update card is grayed out.", [1600, 900], 41.3),
    ("Next, this label is stale.\nIt should say 2.8.", [900, 1200], 72.6),
    ("And the footer overlaps.\n\nSecond paragraph about it.", [1280, 720], 101.0),
    ("That's it.", None, None),
)

CASES = {
    "compose": [
        ["letter", THREE_SHOTS, None],
        ["a4", THREE_SHOTS, None],
        ["letter", THREE_SHOTS, ["", False, False, False]],
        ["letter", THREE_SHOTS, ["Checkout bugs", True, False, True]],
        ["letter", _walkthrough(136.4, ("The update card is grayed out.", [800, 450], 41.3),
                                ("That's it.", None, None)), None],
        ["letter", _walkthrough(90, ("First words.", [900, 1200], 10.0),
                                ("Second words.", [900, 1200], 50.0),
                                ("Last words.", None, None)), None],
        ["letter", _walkthrough(90, ("First words.", [1344, 900], 10.0),
                                ("", [1600, 900], 20.0)), None],
        ["letter", _walkthrough(90, (_CONTEXT, [900, 1200], 61.0), ("Done.", None, None)), None],
        ["letter", _walkthrough(600, ("Short start.", [900, 500], 5.0),
                                (_LONG, [900, 1200], 300.0), ("Done.", None, None)), None],
        ["letter", _walkthrough(52, ("First paragraph.\n\nSecond one.", None, None)), None],
        ["letter", _walkthrough(3, (" ", None, None)), None],
        ["letter", _walkthrough(20, ("These words stay.", "unreadable", 7.0)), None],
        ["letter", _walkthrough(20, ("These words stay.", "unreadable", 7.0)),
         ["Walkthrough", True, True, False]],
        ["letter", _walkthrough(40, ("First.", [900, 700], 5.0), (_SECOND, [900, 700], 30.0),
                                ("Done.", None, None)), ["Walkthrough", True, True, False]],
        ["a4", _walkthrough(40, ("First.", [900, 700], 5.0), (_SECOND, [900, 700], 30.0),
                            ("Done.", None, None)), None],
        [[672, 921], _walkthrough(9, ("First words.", [1344, 600], 1.0),
                                  ("", [1600, 900], 2.0)), ["Walkthrough", True, True, True]],
        [[672, 921], _walkthrough(9, ("First words.", [1344, 840], 1.0),
                                  ("", [1600, 900], 2.0)), None],
        [[672, 921], _walkthrough(9, ("First words.", [1344, 900], 1.0),
                                  ("", [1600, 900], 2.0)), None],
        [[600, 800], _walkthrough(30, ("", [400, 2000], 1.0), ("", [2000, 400], 2.0),
                                  ("Tall and wide.", [500, 500], None)), None],
    ],
    "compose_markdown": [
        ["letter", "# Title\n\n1. First step\n2. Second step\n\n![A wide one](shot-1.png)\n\nAfter.",
         [["shot-1.png", 1600, 900]], None],
        ["letter", "1. First step\n2. Second step\n\n- A point\n  - A smaller one\n\n"
         "a. lettered?\n\n* [ ] task\n", [], None],
        [[600, 800], "1. A step\n\n   ![wide](shot-1.png)\n", [["shot-1.png", 1600, 900]], None],
        [[600, 800], "# Title\n\nWords.\n\n| Setting | Value |\n| --- | --- |\n| Agent | Codex |\n",
         [], None],
        ["letter", "Before\n\n![gone](shot-9.png)\n\n<b>bold?</b>\n\nAfter", [], None],
        ["letter", "# Title\n\n" + "\n\n".join(f"Paragraph {n} " + "word " * 40 for n in range(9))
         + "\n\n## Lead\n\nThe words about it.\n\n![tall](shot-1.png)",
         [["shot-1.png", 800, 1200]], None],
        ["letter", "# Purple\n\n" + "Words in purple. " * 40, [], ["purple", "", ""]],
        ["a4", "# Steps\n\n1. Sign in.\n2. Invoices hang.\n\n- one\n- two\n\n"
         "| Step | Result |\n|---|---|\n| Sign in | Stalls |\n\n3. Then it recovers.\n", [],
         ["#5b2a86", "purple", "no such font"]],
        ["letter", "## Overview\n\nIntro words.\n\n> A quote\n> over two lines\n\n```\ncode\nblock\n```"
         "\n\n---\n\n### Shot\n\n![one](shot-1.png)\n\nMiddle words.\n\n![two](shot-2.png)\n\n"
         "#### Deep heading\n\n![three](shot-3.png)",
         [["shot-1.png", 1400, 1000], ["shot-2.png", 900, 1300], ["shot-3.png", 640, 360]], None],
        [[600, 800], "| A | B |\n|---|---|\n| ![in a cell](shot-1.png) | words |\n",
         [["shot-1.png", 1600, 900]], None],
    ],
}

FLOAT_TOLERANCE = 0.5

PINS = {
    "layout_dpi": DPI,
    "qt_font_dpi": 96,
    "note": "compose(paper, walkthrough, format) and compose_markdown(paper, text, images, look); "
            "see the case file's docstring for the result fields",
}
