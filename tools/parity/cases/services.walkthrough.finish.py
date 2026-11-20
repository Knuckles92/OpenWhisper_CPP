"""Curated inputs for document_settings in services/walkthrough/finish.py (fv_walkthrough's
finish.h): a settings snapshot to [title, details, about, captions, paper], paper "letter",
"a4" or None (the locale's).
"""
from __future__ import annotations


def _document_settings(module, settings):
    from PyQt6.QtGui import QPageSize

    document_format, page_size = module.document_settings(settings)
    papers = {QPageSize.PageSizeId.Letter: "letter", QPageSize.PageSizeId.A4: "a4"}
    return [document_format.title, document_format.details, document_format.about,
            document_format.captions, None if page_size is None else papers[page_size.id()]]


WRAPPERS = {"document_settings": _document_settings}

CASES = {
    "document_settings": [
        [{}],
        [{"walkthrough_title": "  Notes  ", "walkthrough_show_details": False,
          "walkthrough_show_about": False, "walkthrough_show_captions": False,
          "walkthrough_page_size": "a4"}],
        [{"walkthrough_page_size": "letter"}],
        [{"walkthrough_page_size": "legal"}],
        [{"walkthrough_page_size": "LETTER"}],
        [{"walkthrough_page_size": None}],
        [{"walkthrough_title": ""}],
        [{"walkthrough_title": "   "}],
        [{"walkthrough_title": 7}],
        [{"walkthrough_title": None}],
        [{"walkthrough_show_details": "false", "walkthrough_show_about": 0,
          "walkthrough_show_captions": 1}],
        [{"walkthrough_show_details": None, "walkthrough_show_about": "no",
          "walkthrough_show_captions": "off"}],
        [{"walkthrough_title": "Café — notes", "walkthrough_page_size": "auto"}],
    ],
}
