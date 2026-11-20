"""Curated inputs for services/walkthrough/word.py (fv_documents, M6.14): the Word package
python-docx writes for a walkthrough and for a Markdown write-up.

Everything runs on an offscreen QApplication with the fonts and DPI the C++ tests pin
(QT_QPA_FONTDIR=%WINDIR%\\Fonts, QT_FONT_DPI=96, scale 1). Screenshots are PNGs of one colour
written into a scratch folder under out/parity; the package is read back from the .docx written
there.

    write_docx(paper, walkthrough, format)
        word.write_docx(walkthrough, folder, page_size=paper, document_format=format)
    write_markdown_docx(paper, text, images, look, title)
        word.write_markdown_docx(text, folder / "write-up.docx", folder=folder, title=title,
                                 page_size=paper, look=look)
    template_parts()
        the parts of python-docx's default template that every package carries unchanged

paper is "letter", "a4" or None (the locale's); a walkthrough is {"started_at": iso,
"duration_s": seconds, "sections": [[text, shot, kept_at_s]]}, a shot [width, height] or None; a
format is [title, details, about, captions] or None; images are [[name, width, height]] written
into the folder; a look is [text_color, heading_color, font] or None. A package reads as:

    entries      the zip's entry names, in order
    zip64        whether any entry carries a Zip64 extra field
    parts        the text of each XML part named in _PARTS
    styles       a w:styles holding styles.xml's w:docDefaults and the styles a package uses
                 (Normal, the default character, table and numbering styles), as written
    styles_used  the style ids document.xml refers to
    media        each image part's pixel size

The rest of styles.xml (latent styles and 160 styles nothing refers to), stylesWithEffects.xml,
numbering.xml, customXml and the template's thumbnail are python-docx's template, carried
unchanged and unused; template_parts holds the parts FioriVoce carries too.
"""

import os
import shutil
import sys
import tempfile
import zipfile
from datetime import datetime
from pathlib import Path

REPO = Path(__file__).resolve().parents[3]
SCRATCH = REPO / "out" / "parity" / "walkthrough_word"

_APP = []

_PARTS = (
    "[Content_Types].xml",
    "_rels/.rels",
    "docProps/core.xml",
    "word/document.xml",
    "word/_rels/document.xml.rels",
)
_TEMPLATE_PARTS = (
    "docProps/app.xml",
    "word/settings.xml",
    "word/webSettings.xml",
    "word/fontTable.xml",
    "word/theme/theme1.xml",
)
_KEPT_STYLES = ("Normal", "DefaultParagraphFont", "TableNormal", "NoList")
_W = "http://schemas.openxmlformats.org/wordprocessingml/2006/main"


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


def _paper(paper):
    if paper is None:
        return None
    from PyQt6.QtGui import QPageSize
    return QPageSize({"letter": QPageSize.PageSizeId.Letter, "a4": QPageSize.PageSizeId.A4}[paper])


def _zip64(info: zipfile.ZipInfo) -> bool:
    extra, at = info.extra, 0
    while at + 4 <= len(extra):
        if int.from_bytes(extra[at:at + 2], "little") == 1:
            return True
        at += 4 + int.from_bytes(extra[at + 2:at + 4], "little")
    return False


def _styles(xml: bytes) -> str:
    from lxml import etree
    root = etree.fromstring(xml)
    kept = etree.Element(root.tag, nsmap=root.nsmap)
    for child in root:
        if child.tag == f"{{{_W}}}docDefaults":
            kept.append(child)
    for style in root.findall(f"{{{_W}}}style"):
        if style.get(f"{{{_W}}}styleId") in _KEPT_STYLES:
            kept.append(style)
    return etree.tostring(kept, encoding="unicode")


def _styles_used(xml: bytes) -> list[str]:
    from lxml import etree
    root = etree.fromstring(xml)
    used = set()
    for tag in ("pStyle", "rStyle", "tblStyle"):
        for element in root.iter(f"{{{_W}}}{tag}"):
            used.add(element.get(f"{{{_W}}}val"))
    return sorted(used)


def _package(path: Path) -> dict:
    from PyQt6.QtGui import QImage
    with zipfile.ZipFile(path) as package:
        infos = package.infolist()
        media = {}
        for info in infos:
            if info.filename.startswith("word/media/"):
                image = QImage.fromData(package.read(info))
                media[info.filename] = [image.width(), image.height()]
        return {
            "entries": [info.filename for info in infos],
            "zip64": any(_zip64(info) for info in infos),
            "parts": {name: package.read(name).decode("utf-8") for name in _PARTS},
            "styles": _styles(package.read("word/styles.xml")),
            "styles_used": _styles_used(package.read("word/document.xml")),
            "media": media,
        }


def _write_docx(module, paper, walkthrough, document_format):
    _app()
    from services.walkthrough.document import DocumentFormat, Section, Walkthrough
    root = _root()
    try:
        sections = []
        for number, (text, shot, kept_at_s) in enumerate(walkthrough["sections"], 1):
            path = None if shot is None else _png(root / f"shot-{number}.png", *shot)
            sections.append(Section(text, path, kept_at_s))
        value = Walkthrough(datetime.fromisoformat(walkthrough["started_at"]),
                            walkthrough["duration_s"], tuple(sections))
        chosen = DocumentFormat() if document_format is None else DocumentFormat(*document_format)
        path = module.write_docx(value, root, page_size=_paper(paper), document_format=chosen)
        return _package(path)
    finally:
        shutil.rmtree(root)


def _write_markdown_docx(module, paper, text, images, look, title):
    _app()
    from services.walkthrough.document import Look
    root = _root()
    try:
        for name, width, height in images:
            _png(root / name, width, height)
        chosen = Look() if look is None else Look(*look)
        path = module.write_markdown_docx(text, root / "write-up.docx", folder=root, title=title,
                                          page_size=_paper(paper), look=chosen)
        return _package(path)
    finally:
        shutil.rmtree(root)


def _template_parts(module):
    from docx import Document
    root = _root()
    try:
        path = root / "template.docx"
        Document().save(str(path))
        with zipfile.ZipFile(path) as package:
            return {name: package.read(name).decode("utf-8") for name in _TEMPLATE_PARTS}
    finally:
        shutil.rmtree(root)


WRAPPERS = {
    "write_docx": _write_docx,
    "write_markdown_docx": _write_markdown_docx,
    "template_parts": _template_parts,
}

STARTED = "2026-10-20T14:32:05"


def _walkthrough(duration, *sections):
    return {"started_at": STARTED, "duration_s": duration, "sections": [list(s) for s in sections]}


# test_a_walkthrough_reads_as_its_pdf_does_with_each_section_kept_on_a_page's walkthrough.
THREE_SECTIONS = _walkthrough(
    80,
    ("Open the menu.", [400, 240], 4.0),
    ("Pick the backend.\n\nThen save.", [400, 240], 72.0),
    ("That's all.", None, None),
)

# test_a_write_up_keeps_its_structure_links_and_look's write-up.
WRITE_UP = """# Changing the backend

Open **Settings** and follow [the guide](https://example.com/guide).

![The menu](shot-1.png)

## Steps

- Pick one
- Save it

> Restart after.

```
backend = "remote"
```

| Name | Value |
|---|---|
| speed | fast |

![Gone](missing.png)
"""

# What the first write-up leaves out: emphasis, inline code, a line break inside a paragraph,
# numbered and nested lists, a rule, a deep heading, a repeated link, pictures in a list and in
# a table, and a second picture with the same bytes as the first.
MORE_MARKDOWN = """# Deep dive

Some *slanted* and ~~struck~~ words with `inline code`.
A second line in the same paragraph, and [the guide](https://example.com/guide) again \
at [the guide](https://example.com/guide).

1. First step
2. Second step
   - A smaller point

   ![In a list](shot-1.png)

---

#### A deep heading

| Shot | Words |
|---|---|
| ![In a cell](shot-2.png) | **bold** cell |

![Again](shot-1.png)
"""

CASES = {
    "write_docx": [
        ["letter", THREE_SECTIONS, None],
        ["a4", THREE_SECTIONS, ["Bug report", False, False, False]],
        ["letter", _walkthrough(136.4, ("Okay, the update card is grayed out.", [1600, 900], 41.3),
                                ("Next, this label is stale.\nIt should say 2.8.", [900, 1200],
                                 72.6),
                                ("", [1280, 720], 101.0), ("That's it.", None, None)),
         ["Checkout bugs", True, False, True]],
    ],
    "write_markdown_docx": [
        ["letter", WRITE_UP, [["shot-1.png", 400, 240]], ["", "#5b2a86", ""],
         "Changing the backend"],
        ["a4", MORE_MARKDOWN, [["shot-1.png", 1600, 900], ["shot-2.png", 640, 360]],
         ["#333333", "purple", "Courier New"], "Deep dive"],
    ],
    "template_parts": [[]],
}

PINS = {
    "qt_font_dpi": 96,
    "note": "write_docx(paper, walkthrough, format), write_markdown_docx(paper, text, images, "
            "look, title) and template_parts(); see the case file's docstring for the result "
            "fields",
}
