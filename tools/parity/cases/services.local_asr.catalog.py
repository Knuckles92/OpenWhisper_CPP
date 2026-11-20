"""Curated inputs for services/local_asr/catalog.py (M1.11).

The resolution functions read the host through services.components (platform tag, installed
components), services.gpu_info (NVML's GPU) and CTranslate2's CUDA device count. Each case pins
them: a platform tag, the installed component ids, and a GPU as [name, total MiB, [major, minor]
or None] or None. FioriVoce asks NVML instead of CTranslate2, so the CUDA device count is 1
exactly when a GPU is given: the grid is the case where both agree.
"""

import itertools
import sys
import types

from services import components, gpu_info

PLATFORMS = ["win_amd64", "linux_x86_64", "linux_aarch64", "darwin_arm64", "darwin_x86_64"]
INSTALLED = [
    [],
    ["asr-nvidia-cpu"],
    ["asr-nvidia-cuda"],
    ["asr-nvidia-cpu", "asr-nvidia-cuda"],
    ["asr-nvidia-cpu", "asr-nvidia-vulkan"],
    ["asr-nvidia-vulkan"],
    ["asr-nvidia-cpu", "asr-nvidia-metal"],
]
GPUS = [
    None,
    ["NVIDIA GeForce RTX 2060", 6144, [7, 5]],
    ["NVIDIA GeForce GTX 1050 Ti", 4096, [6, 1]],
    ["NVIDIA GeForce GTX 1050 Ti", 4096, None],
]


class _Host:
    """Patches the host the catalog reads, for the length of a call."""

    def __init__(self, platform, installed, gpu):
        self.platform, self.installed, self.gpu = platform, set(installed or ()), gpu

    def __enter__(self):
        card = None
        if self.gpu is not None:
            name, mib, capability = self.gpu
            card = gpu_info.NvidiaGpu(name, mib, tuple(capability) if capability else None)
        self._saved = (components.current_platform_tag, components.is_installed,
                       gpu_info.nvidia_gpu, sys.modules.get("ctranslate2"))
        platform = self.platform
        components.current_platform_tag = lambda *args, **kwargs: platform
        components.is_installed = lambda key: key in self.installed
        gpu_info.nvidia_gpu = lambda: card
        count = 1 if card is not None else 0
        sys.modules["ctranslate2"] = types.SimpleNamespace(get_cuda_device_count=lambda: count)
        return self

    def __exit__(self, *exc):
        (components.current_platform_tag, components.is_installed, gpu_info.nvidia_gpu,
         ctranslate2) = self._saved
        if ctranslate2 is None:
            sys.modules.pop("ctranslate2", None)
        else:
            sys.modules["ctranslate2"] = ctranslate2
        return False


def _resolve_runtime(module, backend, requested, platform, installed, gpu):
    with _Host(platform, installed, gpu):
        return list(module.resolve_runtime(backend, requested))


def _runtime_id(module, backend, device, platform, gpu):
    with _Host(platform, [], gpu):
        return module.runtime_id(backend, device)


def _nvidia_gpu_runtime(module, platform, gpu):
    with _Host(platform, [], gpu):
        return module.nvidia_gpu_runtime()


def _missing_runtime(module, model, settings, platform, installed, gpu):
    with _Host(platform, installed, gpu):
        return module.missing_runtime(model, settings)


CASES = {
    "resolve_runtime": [
        [backend, requested, platform, installed, gpu]
        for backend in ("parakeet", "nemotron")
        for requested, platform, installed, gpu in itertools.product(
            ("auto", "cpu", "cuda"), PLATFORMS, INSTALLED, GPUS)
    ],
    "runtime_id": [
        [backend, device, platform, gpu]
        for backend, device, platform, gpu in itertools.product(
            ("parakeet", "nemotron", "qwen_asr", "moonshine", "parakeet_mlx", "parakeet_unified"),
            ("cpu", "cuda", "metal"), PLATFORMS, GPUS)
    ],
    "nvidia_gpu_runtime": [[platform, gpu] for platform, gpu in itertools.product(PLATFORMS, GPUS)],
    "missing_runtime": [
        [model, settings, platform, installed, gpu]
        for model, settings, platform, installed, gpu in itertools.product(
            ("parakeet-v3", "orukeet-v0.1", "turbo"),
            ({}, {"local_asr_devices": {"parakeet": "cpu"}},
             {"local_asr_devices": {"parakeet": "cuda"}}),
            PLATFORMS, INSTALLED[:4], GPUS[:3])
    ],
    "selected_model": [
        ["parakeet", {}],
        ["parakeet", {"local_asr_models": {"parakeet": "orukeet-v0.1"}}],
        ["parakeet", {"local_asr_models": {"parakeet": "parakeet-v3"}}],
        ["parakeet", {"local_asr_models": {"parakeet": "qwen-1.7b"}}],
        ["parakeet", {"local_asr_models": {"parakeet": "turbo"}}],
        ["parakeet", {"local_asr_models": {"parakeet": 3}}],
        ["parakeet", {"local_asr_models": {"parakeet": None}}],
        ["parakeet", {"local_asr_models": {"nemotron": "orukeet-v0.1"}}],
        ["parakeet", {"local_asr_models": []}],
        ["parakeet", {"local_asr_models": "orukeet-v0.1"}],
        ["parakeet", {"local_asr_models": None}],
        ["parakeet", {"whisper_model": "turbo"}],
    ],
    "selected_device": [
        [backend, settings]
        for backend in ("parakeet", "nemotron", "qwen_asr", "moonshine", "parakeet_mlx")
        for settings in (
            {},
            {"local_asr_devices": {backend: "auto"}},
            {"local_asr_devices": {backend: "cpu"}},
            {"local_asr_devices": {backend: "cuda"}},
            {"local_asr_devices": {backend: "metal"}},
            {"local_asr_devices": {backend: "CUDA"}},
            {"local_asr_devices": {backend: 1}},
            {"local_asr_devices": {"other": "cuda"}},
            {"local_asr_devices": []},
            {"local_asr_devices": "cuda"},
        )
    ],
}

WRAPPERS = {
    "resolve_runtime": _resolve_runtime,
    "runtime_id": _runtime_id,
    "nvidia_gpu_runtime": _nvidia_gpu_runtime,
    "missing_runtime": _missing_runtime,
}

PINS = {
    "host_note": "resolve_runtime(backend, requested, platform, installed, gpu), "
                 "runtime_id(backend, device, platform, gpu), nvidia_gpu_runtime(platform, gpu), "
                 "missing_runtime(model, settings, platform, installed, gpu): the platform tag, "
                 "installed component ids and NVML GPU ([name, MiB, [major, minor] or None] or "
                 "None) are patched in; CTranslate2's CUDA count is 1 exactly when a GPU is given",
}


# M2.5: gpu_runtime_offer(model, settings, platform, installed, gpu, vulkan_loader), with the
# Vulkan loader probe patched to `vulkan_loader`. Nemotron is left out: FioriVoce has no such
# model, so the port answers None where OpenWhisper offers it a runtime.
def _gpu_runtime_offer(module, model, settings, platform, installed, gpu, vulkan_loader):
    saved = module._vulkan_loader_present
    module._vulkan_loader_present = lambda: vulkan_loader
    try:
        with _Host(platform, installed, gpu):
            return module.gpu_runtime_offer(model, settings)
    finally:
        module._vulkan_loader_present = saved


CASES["gpu_runtime_offer"] = [
    [model, settings, platform, installed, gpu, vulkan_loader]
    for model, settings, platform, installed, gpu, vulkan_loader in itertools.product(
        ("parakeet-v3", "orukeet-v0.1", "turbo"),
        ({}, {"local_asr_devices": {"parakeet": "cpu"}},
         {"local_asr_devices": {"parakeet": "cuda"}}),
        PLATFORMS, INSTALLED, GPUS, (True, False))
]
WRAPPERS["gpu_runtime_offer"] = _gpu_runtime_offer
PINS["gpu_runtime_offer_note"] = (
    "gpu_runtime_offer(model, settings, platform, installed, gpu, vulkan loader present): the "
    "host is patched as for missing_runtime, and _vulkan_loader_present answers the last argument")
