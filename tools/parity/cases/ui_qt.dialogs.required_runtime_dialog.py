"""Curated inputs for ui_qt/dialogs/required_runtime_dialog.py (M2.5, models/consent_text.h).

Both dialogs are built on an offscreen QApplication with the runtime sizes fixed (the module's
_sizes patched to {component: [download bytes, bytes on disk]}) and, for the GPU choice, the
NVML GPU fixed ([name, MiB, [major, minor] or None] or None). Each returns [window title, title,
body]."""

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


def _texts(dialog):
    from PyQt6.QtWidgets import QLabel
    result = [dialog.windowTitle(), dialog.findChild(QLabel, "headerLabel").text(),
              dialog.body.text()]
    dialog.deleteLater()
    return result


def _required(module, model, component, download):
    _app()
    saved = module._sizes
    module._sizes = lambda component_id: (download, 0)
    try:
        return _texts(module.RequiredRuntimeDialog(model, component))
    finally:
        module._sizes = saved


def _gpu(module, model, gpu_component, cpu_component, gpu, sizes):
    _app()
    from services import gpu_info
    card = None
    if gpu is not None:
        card = gpu_info.NvidiaGpu(gpu[0], gpu[1], tuple(gpu[2]) if gpu[2] else None)
    saved = (module._sizes, gpu_info.nvidia_gpu)
    module._sizes = lambda component_id: tuple(sizes[component_id])
    gpu_info.nvidia_gpu = lambda: card
    try:
        return _texts(module.GpuRuntimeDialog(model, gpu_component, cpu_component))
    finally:
        module._sizes, gpu_info.nvidia_gpu = saved


WRAPPERS = {"RequiredRuntimeDialog": _required, "GpuRuntimeDialog": _gpu}

_SIZES = {"asr-nvidia-cuda": [1460000000, 3120000000], "asr-nvidia-cpu": [4730421, 26420434],
          "asr-nvidia-vulkan": [117000000, 290000000]}

CASES = {
    "RequiredRuntimeDialog": [
        [model, component, download]
        for model in ("parakeet-v3", "orukeet-v0.1")
        for component, download in (("asr-nvidia-cpu", 4730421), ("asr-nvidia-cuda", 1460000000),
                                    ("asr-nvidia-cpu", 0))
    ],
    "GpuRuntimeDialog": [
        [model, gpu_component, "asr-nvidia-cpu", gpu, _SIZES]
        for model in ("parakeet-v3", "orukeet-v0.1")
        for gpu_component in ("asr-nvidia-cuda", "asr-nvidia-vulkan")
        for gpu in (["NVIDIA GeForce RTX 2060", 6144, [7, 5]],
                    ["NVIDIA GeForce GTX 1050 Ti", 4096, [6, 1]], ["Quadro P2000", 5120, None],
                    None)
    ],
}
