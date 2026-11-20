"""Writes tests/fixtures/meeting_intel/knowledge: the Office and PDF files the knowledge-folder
tests read, made by the libraries OpenWhisper reads them with.

    $env:OPENWHISPER_CPP_OW_ROOT = 'D:\\coding\\whisper_local'
    & $env:OPENWHISPER_CPP_OW_PYTHON -I -B tools/parity/check_isolation.py -- `
        $env:OPENWHISPER_CPP_OW_PYTHON -I -B tools/parity/fixtures/make_knowledge_folder.py

OpenWhisper's environment is entered through tools/parity/ow_isolation.py (its data root lives
under out/parity/); only the files below are written, into the fixture folder.

- memo.docx (python-docx): paragraphs with a tab, a line break and a page break, an empty
  paragraph, and a table with a horizontally and a vertically merged cell.
- deck.pptx (python-pptx): a text box, a title placeholder with a soft line break, a group shape
  whose text is not read, and a second slide.
- sheet.xlsx (openpyxl): two worksheets with shared strings, integers, floats, booleans, a date,
  a time, a duration, a formula without a cached value, a gap row and a column past the 20th.
- budget.pdf: one page of text, written here by hand (no PDF library writes text).
- owner-only.pdf (pypdf): budget.pdf encrypted with an owner password only, so the empty user
  password opens it.
- locked.pdf (pypdf): a blank page encrypted with the user password "secret".
- broken.pdf: bytes that only start like a PDF.

The golden vectors of tools/parity/cases/meeting.context_folder.py record what OpenWhisper
extracts from these files.
"""
from __future__ import annotations

import datetime
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
REPO = HERE.parents[2]
sys.path.insert(0, str(REPO / "tools" / "parity"))

import ow_isolation  # noqa: E402

OUT = REPO / "tests" / "fixtures" / "meeting_intel" / "knowledge"


def make_docx(path: Path) -> None:
    from docx import Document
    from docx.enum.text import WD_BREAK

    document = Document()
    document.add_paragraph("The quarterly budget was approved")
    paragraph = document.add_paragraph("Owner:\tFinance")
    run = paragraph.add_run("second line")
    run.add_break()
    run.add_text("after a soft break")
    paragraph.add_run().add_break(WD_BREAK.PAGE)
    document.add_paragraph("")
    document.add_paragraph("   Spaced out   ")
    table = document.add_table(rows=3, cols=3)
    table.cell(0, 0).text = "Item"
    table.cell(0, 1).text = "Cost"
    table.cell(0, 2).text = "Notes"
    table.cell(1, 0).text = "Venue"
    table.cell(1, 1).text = "1200"
    table.cell(1, 2).text = "spans two rows"
    table.cell(2, 0).text = "Catering"
    table.cell(1, 2).merge(table.cell(2, 2))
    merged = table.cell(2, 0).merge(table.cell(2, 1))
    merged.text = "Catering and drinks"
    document.add_paragraph("Closing note on the budget")
    document.save(path)


def make_pptx(path: Path) -> None:
    from pptx import Presentation
    from pptx.util import Inches

    deck = Presentation()
    slide = deck.slides.add_slide(deck.slide_layouts[5])
    slide.shapes.title.text = "Budget review\vFriday"
    box = slide.shapes.add_textbox(Inches(1), Inches(2), Inches(5), Inches(1))
    box.text_frame.text = "Budget slides for Friday"
    box.text_frame.add_paragraph().text = "Second paragraph"
    group = slide.shapes.add_group_shape()
    inner = group.shapes.add_textbox(Inches(1), Inches(4), Inches(3), Inches(1))
    inner.text_frame.text = "grouped text is not read"
    second = deck.slides.add_slide(deck.slide_layouts[6])
    note = second.shapes.add_textbox(Inches(1), Inches(1), Inches(5), Inches(1))
    note.text_frame.text = "Appendix: venue costs"
    deck.save(path)


def make_xlsx(path: Path) -> None:
    from openpyxl import Workbook

    book = Workbook()
    sheet = book.active
    sheet.title = "Budget"
    sheet["A1"] = "budget"
    sheet["B1"] = "12000"
    sheet["C1"] = 12000
    sheet["D1"] = 3.5
    sheet["E1"] = True
    sheet["A2"] = datetime.datetime(2026, 3, 1, 9, 30)
    sheet["B2"] = datetime.date(2026, 3, 2)
    sheet["C2"] = datetime.time(14, 15, 30)
    sheet["D2"] = datetime.timedelta(hours=26, minutes=5)
    sheet["E2"] = "=C1*2"
    sheet["A3"] = "  padded  "
    sheet["B3"] = "   "
    sheet["A5"] = "after a gap row"
    sheet.cell(row=6, column=20, value="column twenty")
    sheet.cell(row=6, column=21, value="column twenty-one")
    other = book.create_sheet("Notes")
    other["A1"] = "Second sheet budget note"
    book.save(path)


def text_pdf(lines: list[str]) -> bytes:
    """One Letter page showing `lines` in Helvetica, with a correct cross-reference table."""
    content = ["BT", "/F1 14 Tf", "72 720 Td", "18 TL"]
    for line in lines:
        escaped = line.replace("\\", "\\\\").replace("(", "\\(").replace(")", "\\)")
        content.append(f"({escaped}) Tj T*")
    content.append("ET")
    stream = "\n".join(content).encode("latin-1")
    objects = [
        b"<< /Type /Catalog /Pages 2 0 R >>",
        b"<< /Type /Pages /Kids [3 0 R] /Count 1 >>",
        b"<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] /Contents 4 0 R "
        b"/Resources << /Font << /F1 5 0 R >> >> >>",
        b"<< /Length " + str(len(stream)).encode() + b" >>\nstream\n" + stream + b"\nendstream",
        b"<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica /Encoding /WinAnsiEncoding >>",
    ]
    out = bytearray(b"%PDF-1.4\n")
    offsets = []
    for number, body in enumerate(objects, 1):
        offsets.append(len(out))
        out += f"{number} 0 obj\n".encode() + body + b"\nendobj\n"
    xref = len(out)
    out += f"xref\n0 {len(objects) + 1}\n".encode()
    out += b"0000000000 65535 f \n"
    for offset in offsets:
        out += f"{offset:010d} 00000 n \n".encode()
    out += f"trailer\n<< /Size {len(objects) + 1} /Root 1 0 R >>\nstartxref\n{xref}\n%%EOF\n".encode()
    return bytes(out)


def make_pdfs(folder: Path) -> None:
    from pypdf import PdfReader, PdfWriter

    plain = folder / "budget.pdf"
    plain.write_bytes(text_pdf(["Budget review on Friday", "Venue costs were approved"]))

    owner_only = PdfWriter(clone_from=PdfReader(str(plain)))
    owner_only.encrypt(user_password="", owner_password="owner-secret")
    with (folder / "owner-only.pdf").open("wb") as handle:
        owner_only.write(handle)

    locked = PdfWriter()
    locked.add_blank_page(width=72, height=72)
    locked.encrypt("secret")
    with (folder / "locked.pdf").open("wb") as handle:
        locked.write(handle)

    (folder / "broken.pdf").write_bytes(b"%PDF-1.4 not a real document")


def main() -> None:
    ow_isolation.isolate()
    OUT.mkdir(parents=True, exist_ok=True)
    make_docx(OUT / "memo.docx")
    make_pptx(OUT / "deck.pptx")
    make_xlsx(OUT / "sheet.xlsx")
    make_pdfs(OUT)
    for name in sorted(path.name for path in OUT.iterdir()):
        print(name)


if __name__ == "__main__":
    main()
