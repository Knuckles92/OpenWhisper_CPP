"""Curated inputs for the Qt-free parts of ui_qt/dialogs/settings_downloads.py (M2.5,
models/downloads_model.h and consent_text.h).

DownloadsPage's methods run on a stub page: plain objects stand in for its labels, buttons and
tabs and record what the method sets, so no page is built and no cache is read. Each wrapper
says what it returns. BatchDownloadDialog is built for real on an offscreen QApplication.

  sorted_rows(mode, rows)            rows [name, backend, cached, sort size]: names in _sort_key
                                     order (all Parakeet rows: the backend index is OpenWhisper's)
  _update_selection_bar(sizes, any_missing, busy, blocked)
                                     the bar's texts, enables, tooltip and Download all visibility
  queue(steps)                       DownloadsPage's download and batch methods in turn; after each
                                     step the message line, whether progress_changed fired, the
                                     queue fields and rail_value (storage summary 1 of 2,
                                     714456704 bytes)
  rail_value(downloading, fraction, checking, downloaded, total, bytes)
  _show_cache_totals(checking, sizes, rows, folder)   [models tab title or None, stats line]
  refresh_components(infos)          the components tab title, None when untouched
  _usage_for(model, dictation, meeting)
  _current_cache_info(info, revision) info [repo, size, path, [revisions]] or None
  BatchDownloadDialog(names)         window title, title, list, total and Download button texts
  delete_confirmation(model)         QMessageBox.question's title and text
  remove_confirmation(name, bytes)   the same for a component
"""

import os
from types import SimpleNamespace

ROWS = ["parakeet-v3", "orukeet-v0.1"]
SUMMARY = {"downloaded": 1, "total": 2, "bytes": 714456704, "by_backend": {"Parakeet": 714456704}}
INF = float("inf")


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


class _Widget:
    def __init__(self, text=""):
        self._text, self._enabled, self._tooltip, self._visible = text, True, "", True

    def setText(self, text):
        self._text = text

    def text(self):
        return self._text

    def setEnabled(self, enabled):
        self._enabled = bool(enabled)

    def setToolTip(self, tooltip):
        self._tooltip = tooltip

    def setVisible(self, visible):
        self._visible = bool(visible)


class _Tabs:
    def __init__(self):
        self.titles = {}

    def set_title(self, index, title):
        self.titles[index] = title


class _Signal:
    def __init__(self):
        self.count = 0

    def emit(self, *args):
        self.count += 1


class _Row:
    def set_progress(self, *args):
        pass

    def update_state(self, *args):
        pass


def _sorted_rows(module, mode, rows):
    from services.local_asr.catalog import BACKENDS, WHISPER_BACKEND
    labels = {WHISPER_BACKEND: "Whisper", **BACKENDS}
    order = ["all"] + [key for key, _ in sorted(labels.items(), key=lambda item: item[1].casefold())]
    stub = SimpleNamespace(
        sort_combo=SimpleNamespace(currentData=lambda: mode),
        backend_filter_combo=SimpleNamespace(
            findData=lambda data: order.index(data) if data in order else -1),
    )
    facts = [SimpleNamespace(model_name=name, backend=backend, is_cached=cached,
                             sort_size_bytes=size) for name, backend, cached, size in rows]
    return [row.model_name
            for row in sorted(facts, key=lambda row: module.DownloadsPage._sort_key(stub, row))]


def _selection_bar(module, sizes, any_missing, busy, blocked):
    names = [f"m{index}" for index in range(len(sizes))]
    stub = SimpleNamespace(
        _selected_names=lambda: names,
        rows={name: SimpleNamespace(sort_size_bytes=size) for name, size in zip(names, sizes)},
        _download_slot_busy=lambda: busy,
        _missing_names=lambda: ["missing"] if any_missing else [],
        _downloads_blocked=blocked,
        selection_summary=_Widget(),
        download_selected_button=_Widget(),
        clear_selection_button=_Widget(),
        select_all_button=_Widget(),
        download_all_button=_Widget(),
    )
    module.DownloadsPage._update_selection_bar(stub)
    return {
        "summary": stub.selection_summary.text(),
        "download_text": stub.download_selected_button.text(),
        "download_enabled": stub.download_selected_button._enabled,
        "download_tooltip": stub.download_selected_button._tooltip,
        "clear_enabled": stub.clear_selection_button._enabled,
        "select_all_enabled": stub.select_all_button._enabled,
        "download_all_enabled": stub.download_all_button._enabled,
        "download_all_visible": stub.download_all_button._visible,
    }


def _queue_stub(module):
    return SimpleNamespace(
        _downloading_model=None, _download_fraction=None, _batch_queue=[], _batch_done=0,
        _batch_failed=0, message_label=_Widget(), rows={name: _Row() for name in ROWS},
        refresh=lambda *args, **kwargs: None, progress_changed=_Signal(),
        _display_name=module.DownloadsPage._display_name, _cache_inventory_loading=False,
        storage_summary=lambda: dict(SUMMARY),
    )


def _queue(module, steps):
    page = module.DownloadsPage
    stub = _queue_stub(module)
    methods = {
        "set_downloading": page.set_downloading,
        "progress": page.set_download_progress,
        "finish_download": page.finish_download,
        "begin_batch": page.begin_batch,
        "finish_batch": page.finish_batch,
    }
    states = []
    for name, *args in steps:
        before = stub.progress_changed.count
        methods[name](stub, *args)
        states.append({
            "message": stub.message_label.text(),
            "rail_changed": stub.progress_changed.count > before,
            "downloading": stub._downloading_model,
            "fraction": stub._download_fraction,
            "queue": list(stub._batch_queue),
            "done": stub._batch_done,
            "failed": stub._batch_failed,
            "rail": page.rail_value(stub),
        })
    return states


def _rail_value(module, downloading, fraction, checking, downloaded, total, size):
    stub = SimpleNamespace(
        _downloading_model=downloading, _download_fraction=fraction,
        _cache_inventory_loading=checking, _display_name=module.DownloadsPage._display_name,
        storage_summary=lambda: {"downloaded": downloaded, "total": total, "bytes": size},
    )
    return module.DownloadsPage.rail_value(stub)


def _show_cache_totals(module, checking, sizes, rows, folder):
    saved = module.get_hf_cache_dir
    module.get_hf_cache_dir = lambda: folder
    try:
        stub = SimpleNamespace(_cache_inventory_loading=checking, stats_label=_Widget(),
                               tabs=_Tabs(), rows={f"row{index}": None for index in range(rows)})
        seen = {f"repo{index}": SimpleNamespace(size_bytes=size) for index, size in enumerate(sizes)}
        module.DownloadsPage._show_cache_totals(stub, seen)
        return [stub.tabs.titles.get(0), stub.stats_label.text()]
    finally:
        module.get_hf_cache_dir = saved


def _info(fields):
    from services.components import ComponentInfo
    return ComponentInfo(
        component_id=fields.get("component_id", "asr-nvidia-cuda"),
        display_name=fields.get("display_name", "NVIDIA Speech GPU"),
        summary=fields.get("summary", ""), state=fields["state"],
        installed_version=None, available_version=None,
        download_bytes=fields.get("download_bytes", 0),
        install_bytes=fields.get("install_bytes", 0), reason=fields.get("reason", ""))


def _refresh_components(module, infos):
    built = {f"c{index}": _info(fields) for index, fields in enumerate(infos)}
    saved = module.component_coordinator
    module.component_coordinator = SimpleNamespace(describe=lambda cid: built[cid],
                                                   is_installing=lambda cid: False)
    try:
        stub = SimpleNamespace(_component_rows={cid: _Row() for cid in built}, tabs=_Tabs())
        module.DownloadsPage.refresh_components(stub)
        return stub.tabs.titles.get(1)
    finally:
        module.component_coordinator = saved


def _usage_for(module, model, dictation, meeting):
    return module.DownloadsPage._usage_for(model, dictation, meeting)


def _current_cache_info(module, info, revision):
    cached = None if info is None else module.CachedModelInfo(info[0], info[1], info[2],
                                                               tuple(info[3]))
    result = module._current_cache_info(cached, revision)
    if result is None:
        return None
    return [result.repo_id, result.size_bytes, result.path, list(result.revision_hashes)]


def _batch_dialog(module, names):
    _app()
    from PyQt6.QtWidgets import QLabel, QPushButton
    saved = module.get_hf_cache_dir
    module.get_hf_cache_dir = lambda: "C:/models"
    try:
        dialog = module.BatchDownloadDialog(names)
        buttons = [button.text() for button in dialog.findChildren(QPushButton)]
        result = {
            "window_title": dialog.windowTitle(),
            "title": dialog.findChild(QLabel, "downloadsTitle").text(),
            "list": dialog.model_list_label.text(),
            "total": dialog.total_label.text(),
            "buttons": buttons,
        }
        dialog.deleteLater()
        return result
    finally:
        module.get_hf_cache_dir = saved


def _question_recorder(module):
    asked = []
    real = module.QMessageBox

    def question(parent, title, text, *args):
        asked.append([title, text])
        return real.StandardButton.No

    return asked, SimpleNamespace(question=question, StandardButton=real.StandardButton), real


def _delete_confirmation(module, model):
    asked, fake, real = _question_recorder(module)
    module.QMessageBox = fake
    try:
        module.DownloadsPage._on_delete_clicked(SimpleNamespace(on_delete_requested=None), model)
        return asked[0]
    finally:
        module.QMessageBox = real


def _remove_confirmation(module, name, size):
    asked, fake, real = _question_recorder(module)
    saved = module.component_coordinator
    module.QMessageBox = fake
    module.component_coordinator = SimpleNamespace(
        describe=lambda cid: SimpleNamespace(display_name=name, install_bytes=size))
    try:
        stub = SimpleNamespace(component_remove_requested=_Signal())
        module.DownloadsPage._confirm_component_removal(stub, "asr-nvidia-cuda")
        return asked[0]
    finally:
        module.QMessageBox = real
        module.component_coordinator = saved


WRAPPERS = {
    "sorted_rows": _sorted_rows,
    "_update_selection_bar": _selection_bar,
    "queue": _queue,
    "rail_value": _rail_value,
    "_show_cache_totals": _show_cache_totals,
    "refresh_components": _refresh_components,
    "_usage_for": _usage_for,
    "_current_cache_info": _current_cache_info,
    "BatchDownloadDialog": _batch_dialog,
    "delete_confirmation": _delete_confirmation,
    "remove_confirmation": _remove_confirmation,
}

_SORT_ROWS = [
    ["parakeet-v3", "parakeet", False, 714000000],
    ["orukeet-v0.1", "parakeet", True, 714456704],
    ["Zeta", "parakeet", False, INF],
    ["alpha", "parakeet", True, 100],
    ["beta", "parakeet", False, 100],
    ["Gamma", "parakeet", True, 714000000],
]

CASES = {
    "sorted_rows": [[mode, _SORT_ROWS]
                    for mode in ("recommended", "downloaded", "size", "name", "backend", "other")],
    "_update_selection_bar": [
        [sizes, any_missing, busy, blocked]
        for sizes in ([], [714000000], [714000000, 714000000], [714000000, INF], [1530000000])
        for any_missing in (False, True)
        for busy in (False, True)
        for blocked in (False, True)
    ],
    "queue": [
        [[["set_downloading", "parakeet-v3"], ["progress", "parakeet-v3", 0, 0],
          ["progress", "parakeet-v3", 0, 714456704], ["progress", "parakeet-v3", 3000000, 714456704],
          ["progress", "parakeet-v3", 3600000, 714456704],
          ["progress", "parakeet-v3", 357228352, 714456704],
          ["progress", "parakeet-v3", 900000000, 714456704],
          ["finish_download", "parakeet-v3", True]]],
        [[["set_downloading", "orukeet-v0.1"], ["finish_download", "orukeet-v0.1", False]]],
        [[["begin_batch", ["parakeet-v3", "unknown-model", "orukeet-v0.1"]],
          ["set_downloading", "parakeet-v3"], ["progress", "parakeet-v3", 5, 10],
          ["finish_download", "parakeet-v3", True], ["set_downloading", "orukeet-v0.1"],
          ["finish_download", "orukeet-v0.1", True], ["finish_batch", 2, 2]]],
        [[["begin_batch", ["parakeet-v3", "orukeet-v0.1"]], ["set_downloading", "parakeet-v3"],
          ["finish_download", "parakeet-v3", False], ["set_downloading", "orukeet-v0.1"],
          ["finish_download", "orukeet-v0.1", True], ["finish_batch", 1, 2]]],
        [[["begin_batch", ["parakeet-v3", "orukeet-v0.1"]], ["set_downloading", "parakeet-v3"],
          ["finish_download", "parakeet-v3", True], ["finish_batch", 1, 2]]],
        [[["begin_batch", ["orukeet-v0.1"]], ["set_downloading", "orukeet-v0.1"],
          ["finish_download", "orukeet-v0.1", True], ["finish_batch", 1, 1]]],
        [[["begin_batch", []], ["finish_batch", 0, 0]]],
        [[["set_downloading", "unknown-model"], ["progress", "unknown-model", 1, 3],
          ["finish_download", "other", True]]],
    ],
    "rail_value": [
        ["parakeet-v3", None, False, 1, 2, 714456704],
        ["parakeet-v3", 0.0, False, 1, 2, 714456704],
        ["parakeet-v3", 0.125, False, 1, 2, 714456704],
        ["parakeet-v3", 0.005, False, 1, 2, 714456704],
        ["parakeet-v3", 0.015, False, 1, 2, 714456704],
        ["parakeet-v3", 0.025, False, 1, 2, 714456704],
        ["orukeet-v0.1", 1.0, True, 1, 2, 714456704],
        ["unknown-model", None, False, 0, 2, 0],
        [None, None, True, 1, 2, 714456704],
        [None, None, False, 0, 2, 0],
        [None, None, False, 2, 2, 1428913408],
        ["", None, False, 1, 2, 999],
    ],
    "_show_cache_totals": [
        [True, [], 2, "C:/models"],
        [False, [], 2, "C:/models"],
        [False, [714456704], 2, "C:/models"],
        [False, [714456704, 713975456], 2, "D:/speech-models"],
        [True, [714456704], 2, "D:/speech-models"],
    ],
    "refresh_components": [
        [[]],
        [[{"state": "installed"}, {"state": "not_installed"}]],
        [[{"state": "external"}, {"state": "update_available"}]],
        [[{"state": "update_available"}, {"state": "update_available"}]],
        [[{"state": "broken"}, {"state": "incompatible"}]],
        [[{"state": "installed"}]],
    ],
    "_usage_for": [
        [model, dictation, meeting]
        for model in ("parakeet-v3", "orukeet-v0.1")
        for dictation in ("parakeet-v3", "orukeet-v0.1", "auto", "")
        for meeting in ("parakeet-v3", "orukeet-v0.1", "")
    ],
    "_current_cache_info": [
        [None, None],
        [None, "abc"],
        [["nvidia/parakeet-tdt-0.6b-v3", 714, "/p", ["abc"]], None],
        [["nvidia/parakeet-tdt-0.6b-v3", 714, "/p", ["abc"]], ""],
        [["nvidia/parakeet-tdt-0.6b-v3", 714, "/p", ["abc"]], "abc"],
        [["nvidia/parakeet-tdt-0.6b-v3", 714, "/p", ["old", "abc"]], "abc"],
        [["nvidia/parakeet-tdt-0.6b-v3", 714, "/p", ["old"]], "abc"],
        [["nvidia/parakeet-tdt-0.6b-v3", 714, "/p", []], "abc"],
    ],
    "BatchDownloadDialog": [
        [["parakeet-v3"]],
        [["parakeet-v3", "orukeet-v0.1"]],
        [["parakeet-v3", "unknown-model"]],
        [["unknown-model"]],
        [[]],
    ],
    "delete_confirmation": [["parakeet-v3"], ["orukeet-v0.1"]],
    "remove_confirmation": [["NVIDIA Speech GPU", 1530000000], ["NVIDIA Speech CPU", 26420434],
                            ["NVIDIA Speech CPU", 0]],
}
