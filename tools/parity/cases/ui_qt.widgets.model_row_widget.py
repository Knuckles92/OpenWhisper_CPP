"""Curated inputs for ui_qt/widgets/model_row_widget.py (M2.5, models/downloads_model.h's
model_row_state, model_row_progress, model_row_summary and matches_filter).

update_state(model, cached size or None, is_active, is_loaded, downloading, downloads_blocked,
download_slot_busy, queued, selection_enabled) builds a real row on an offscreen QApplication,
renders the state, and returns what the row shows: texts, dynamic properties, explicit
visibility (not isHidden) and enables. set_progress(model, done, total) renders a downloading row
first. _model_summary and matches_filter run on a stub row."""

import itertools
import os
from types import SimpleNamespace


def _app():
    # As render_reference.py: offscreen finds no fonts on Windows without the font folder.
    os.environ.setdefault("QT_QPA_PLATFORM", "offscreen")
    os.environ.setdefault("QT_QPA_FONTDIR",
                          os.path.join(os.environ.get("WINDIR", r"C:\Windows"), "Fonts"))
    from PyQt6.QtWidgets import QApplication
    if not _APP:
        # Held for the process: a collected QApplication takes every widget down with it.
        _APP.append(QApplication.instance() or QApplication([]))
    return _APP[0]


_APP = []


def _row(module, name):
    _app()
    return module.ModelRowWidget(name)


def _update_state(module, name, cached_size, is_active, is_loaded, downloading, blocked, busy,
                  queued, selection_enabled):
    row = _row(module, name)
    info = None
    if cached_size is not None:
        info = module.CachedModelInfo(module.resolve_model_repo(name), cached_size, "/models", ())
    row.update_state(info, is_active=is_active, is_loaded=is_loaded, downloading=downloading,
                     downloads_blocked=blocked, download_slot_busy=busy, queued=queued,
                     selection_enabled=selection_enabled)
    state = {
        "active": bool(row.property("active")),
        "size_text": row.size_label.text(),
        "size_muted": bool(row.size_label.property("muted")),
        "sort_size_bytes": row.sort_size_bytes,
        "badge_text": row.badge.text(),
        "badge_tone": row.badge.property("tone"),
        "badge_visible": not row.badge.isHidden(),
        "progress_visible": not row.progress.isHidden(),
        "checkbox_visible": not row.select_checkbox.isHidden(),
        "checkbox_enabled": row.select_checkbox.isEnabled(),
        "download_visible": not row.download_button.isHidden(),
        "download_enabled": row.download_button.isEnabled(),
        "download_tooltip": row.download_button.toolTip(),
        "set_active_visible": not row.set_active_button.isHidden(),
        "set_active_enabled": row.set_active_button.isEnabled(),
        "delete_visible": not row.delete_button.isHidden(),
        "delete_enabled": row.delete_button.isEnabled(),
        "delete_tooltip": row.delete_button.toolTip(),
    }
    row.deleteLater()
    return state


def _set_progress(module, name, done, total):
    row = _row(module, name)
    row.update_state(None, is_active=False, is_loaded=False, downloading=True)
    row.set_progress(done, total)
    indeterminate = row.progress.minimum() == 0 and row.progress.maximum() == 0
    state = {
        "indeterminate": indeterminate,
        "percent": None if indeterminate else row.progress.value(),
        "size_text": row.size_label.text(),
        "progress_visible": not row.progress.isHidden(),
    }
    row.deleteLater()
    return state


def _model_summary(module, name):
    return module.ModelRowWidget._model_summary(SimpleNamespace(model_name=name))


def _matches_filter(module, name, needle):
    stub = SimpleNamespace(model_name=name, repo_id=module.resolve_model_repo(name))
    return module.ModelRowWidget.matches_filter(stub, needle)


WRAPPERS = {
    "update_state": _update_state,
    "set_progress": _set_progress,
    "_model_summary": _model_summary,
    "matches_filter": _matches_filter,
}

CASES = {
    "update_state": [
        [name, size, *flags]
        for name, size in (("parakeet-v3", None), ("parakeet-v3", 714000000),
                           ("orukeet-v0.1", None), ("unknown-model", None))
        for flags in itertools.product((False, True), repeat=7)
    ],
    "set_progress": [
        ["parakeet-v3", done, total]
        for done, total in ((0, 0), (5, 0), (0, -1), (0, 714456704), (1, 3), (2, 3),
                            (357228352, 714456704), (714456704, 714456704), (999, 1000))
    ],
    "_model_summary": [["parakeet-v3"], ["orukeet-v0.1"], ["base"], ["base.en"],
                       ["distil-large-v3"], ["distil-small.en"], ["unknown-model"]],
    "matches_filter": [
        [name, needle]
        for name in ("parakeet-v3", "orukeet-v0.1")
        for needle in ("", "para", "pkt", "nvidia", "oruk", "tdt", "v3", "zzz", "0.6b", "PARA",
                       "orukeet", "parakeet-v3x", "pv3")
    ],
}
