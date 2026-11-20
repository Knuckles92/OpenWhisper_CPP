"""Curated inputs for ui_qt/widgets/component_row_widget.py (M2.5, models/downloads_model.h's
component_row_state and component_row_progress).

steps(steps) builds a real row for asr-nvidia-cuda on an offscreen QApplication and applies the
steps in turn, recording what the row shows after each:
  ["state", info, installing]    update_state(ComponentInfo(**info), installing); info gives
                                 state, display_name, summary, reason, download_bytes,
                                 install_bytes
  ["progress", phase, done, total]   set_progress
  ["release"]                    the Remove guard's timer fires
No event loop runs, so the guard stays active until a "release" step.
"""

import os


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


def _info(fields):
    from services.components import ComponentInfo
    return ComponentInfo(
        component_id="asr-nvidia-cuda",
        display_name=fields.get("display_name", "NVIDIA Speech GPU"),
        summary=fields.get("summary", "NVIDIA GPU runtime."), state=fields["state"],
        installed_version=None, available_version=None,
        download_bytes=fields.get("download_bytes", 0),
        install_bytes=fields.get("install_bytes", 0), reason=fields.get("reason", ""))


def _snapshot(row):
    progress = row.progress
    return {
        "name": row.name_label.text(),
        "summary": row.summary_label.text(),
        "badge_text": row.badge.text(),
        "badge_tone": row.badge.property("tone"),
        "size_text": row.size_label.text(),
        "progress_visible": not progress.isHidden(),
        "progress_indeterminate": progress.minimum() == 0 and progress.maximum() == 0,
        "progress_value": progress.value(),
        "install_visible": not row.install_button.isHidden(),
        "install_text": row.install_button.text(),
        "install_enabled": row.install_button.isEnabled(),
        "install_tooltip": row.install_button.toolTip(),
        "remove_visible": not row.remove_button.isHidden(),
        "remove_enabled": row.remove_button.isEnabled(),
        "guard_active": row._remove_guard.isActive(),
    }


def _steps(module, steps):
    _app()
    row = module.ComponentRowWidget("asr-nvidia-cuda")
    states = [_snapshot(row)]
    for name, *args in steps:
        if name == "state":
            row.update_state(_info(args[0]), args[1])
        elif name == "progress":
            row.set_progress(*args)
        elif name == "release":
            row._remove_guard.stop()
            row._release_remove()
        states.append(_snapshot(row))
    row._remove_guard.stop()
    row.deleteLater()
    return states


WRAPPERS = {"steps": _steps}

_STATES = ["not_installed", "external", "installed", "update_available", "incompatible", "broken"]

CASES = {
    "steps": [
        *[[[["state", {"state": state, "download_bytes": size, "install_bytes": 1530000000}, False]]]
          for state in _STATES for size in (0, 1460000000)],
        [[["state", {"state": "not_installed", "reason": "Needs a newer driver."}, False]]],
        [[["state", {"state": "broken", "summary": "", "reason": "The previous installation did "
                     "not finish."}, False]]],
        [[["state", {"state": "update_available", "summary": "", "reason": ""}, False]]],
        [[["state", {"state": "not_installed", "download_bytes": 1460000000}, False],
          ["progress", "downloading", 1, 2],
          ["state", {"state": "not_installed", "download_bytes": 1460000000}, True],
          ["progress", "resolving", 0, 0],
          ["progress", "downloading", 584000000, 1460000000],
          ["progress", "verifying", 584000000, 1460000000],
          ["progress", "extracting", 3, 0],
          ["progress", "extracting", 12, 40],
          ["progress", "finalizing", 1, 1],
          ["progress", "mystery", 1, 4],
          ["state", {"state": "installed", "install_bytes": 1530000000}, False],
          ["state", {"state": "installed", "install_bytes": 1530000000}, False],
          ["release"],
          ["state", {"state": "installed", "install_bytes": 1530000000}, False]]],
        [[["state", {"state": "not_installed", "download_bytes": 1460000000}, True],
          ["progress", "downloading", 10, 100],
          ["state", {"state": "not_installed", "download_bytes": 1460000000}, False],
          ["release"],
          ["state", {"state": "not_installed", "download_bytes": 1460000000}, True],
          ["state", {"state": "update_available", "download_bytes": 1460000000}, False]]],
        [[["state", {"state": "installed", "install_bytes": 1530000000}, False],
          ["state", {"state": "update_available", "download_bytes": 5}, True],
          ["progress", "downloading", 0, 0],
          ["state", {"state": "update_available", "download_bytes": 5}, False]]],
    ],
}
