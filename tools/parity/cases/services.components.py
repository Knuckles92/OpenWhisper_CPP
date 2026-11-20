"""Curated inputs for services/components.py (M1.11).

check_compatibility runs with the host's platform tag patched (no python_abi or macos_min case:
OpenWhisper_CPP drops the first, and the second needs a Mac). component_is_published and
available_component_ids(tag) read the built-in catalog, whose NVIDIA Speech rows OpenWhisper_CPP
copies; available_component_ids gets the installed ids and the NVML GPU patched, and OpenWhisper_CPP
compares the NVIDIA Speech ids of its answer. read_manifest reads the given text as a component's
manifest.json; describe builds the given component tree in a scratch components folder against
the given catalog.
"""

import json
import shutil

import ow_isolation
from services import components, gpu_info

_SCRATCH = ow_isolation.SCRATCH / "components-cases"


def _check_compatibility(module, manifest, host_tag):
    original = module.current_platform_tag
    module.current_platform_tag = lambda *args, **kwargs: host_tag
    try:
        return module.check_compatibility(manifest)
    finally:
        module.current_platform_tag = original


def _available_component_ids(module, tag, installed, gpu):
    card = gpu_info.NvidiaGpu(gpu[0], gpu[1], tuple(gpu[2]) if gpu[2] else None) if gpu else None
    # The host is a computer of that platform: nvidia_gpu_runtime reads the host's tag.
    saved = (module.is_installed, gpu_info.nvidia_gpu, module.current_platform_tag)
    module.is_installed = lambda key: key in installed
    gpu_info.nvidia_gpu = lambda: card
    module.current_platform_tag = lambda *args, **kwargs: tag
    try:
        return list(module.available_component_ids(tag))
    finally:
        module.is_installed, gpu_info.nvidia_gpu, module.current_platform_tag = saved


def _in_scratch(module, body):
    root = _SCRATCH / "components"
    shutil.rmtree(_SCRATCH, ignore_errors=True)
    root.mkdir(parents=True)
    original = module.components_root
    module.components_root = lambda: str(root)
    try:
        return body(root)
    finally:
        module.components_root = original
        shutil.rmtree(_SCRATCH, ignore_errors=True)


def _read_manifest(module, text):
    def body(root):
        target = root / "asr-nvidia-cpu"
        target.mkdir()
        if text is not None:
            (target / "manifest.json").write_text(text, encoding="utf-8")
        return module.read_manifest("asr-nvidia-cpu")
    return _in_scratch(module, body)


def _describe(module, scenario):
    """scenario: component, platform, catalog (or None), and the tree: dir (bool), files
    ({relative path: size}), manifest (JSON text written as is, or None), sentinel (text or
    None)."""
    def body(root):
        target = root / scenario["component"]
        if scenario.get("dir"):
            target.mkdir()
        for name, size in scenario.get("files", {}).items():
            path = target / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(b"x" * size)
        if scenario.get("manifest") is not None:
            target.mkdir(exist_ok=True)
            (target / "manifest.json").write_bytes(scenario["manifest"].encode("utf-8"))
        if scenario.get("sentinel") is not None:
            target.mkdir(exist_ok=True)
            (target / ".installed").write_bytes(scenario["sentinel"].encode("utf-8"))
        coordinator = module.ComponentCoordinator()
        catalog = scenario.get("catalog")
        coordinator.fetch_catalog = lambda: catalog
        saved = (module.current_platform_tag, module.gpu_runtime_available)
        module.current_platform_tag = lambda *args, **kwargs: scenario["platform"]
        module.gpu_runtime_available = lambda: False
        try:
            info = coordinator.describe(scenario["component"])
        finally:
            module.current_platform_tag, module.gpu_runtime_available = saved
        return {
            "component_id": info.component_id,
            "state": info.state,
            "installed_version": info.installed_version,
            "available_version": info.available_version,
            "download_bytes": info.download_bytes,
            "install_bytes": info.install_bytes,
            "reason": info.reason,
            "is_usable": info.is_usable,
        }
    return _in_scratch(module, body)


_NEMO = ["asr-nvidia-cpu", "asr-nvidia-cuda", "asr-nvidia-vulkan", "asr-nvidia-metal"]
_TAGS = ["win_amd64", "linux_x86_64", "linux_aarch64", "darwin_arm64", "darwin_x86_64"]
_GPUS = [None, ["NVIDIA GeForce RTX 2060", 6144, [7, 5]],
         ["NVIDIA GeForce GTX 1050 Ti", 4096, [6, 1]]]


def _entry(version="2.0", install_bytes=5000, sizes=(100, 200), **extra):
    entry = {"version": version, "component_api": 1, "platform": "win_amd64",
             "install_bytes": install_bytes,
             "archives": [{"name": f"a{i}.zip", "url": f"https://example.invalid/a{i}.zip",
                           "sha256": "ab" * 32, "size_bytes": size, "extract": "zip"}
                          for i, size in enumerate(sizes)]}
    entry.update(extra)
    return entry


def _catalog(component, entry, tag="win_amd64"):
    return {"schema": 2, "components": {component: {"platforms": {tag: entry}}}}


def _manifest(**fields):
    return json.dumps(fields)


_C = "asr-nvidia-cuda"
_DESCRIBE = [
    {"component": _C, "platform": "win_amd64", "catalog": None},
    {"component": _C, "platform": "win_amd64", "catalog": _catalog(_C, _entry())},
    {"component": _C, "platform": "win_amd64", "catalog": _catalog(_C, _entry()), "dir": True},
    {"component": _C, "platform": "win_amd64", "catalog": _catalog(_C, _entry()),
     "files": {"bin/fake.dll": 1024}},
    {"component": _C, "platform": "win_amd64", "catalog": _catalog(_C, _entry()),
     "files": {"bin/fake.dll": 1024}, "sentinel": "2.0"},
    {"component": _C, "platform": "win_amd64", "catalog": _catalog(_C, _entry()),
     "files": {"bin/fake.dll": 1024}, "sentinel": "2.0", "manifest": "{oops"},
    {"component": _C, "platform": "win_amd64", "catalog": _catalog(_C, _entry()),
     "files": {"bin/fake.dll": 1024}, "sentinel": "2.0",
     "manifest": _manifest(version="2.0", component_api=1, platform="win_amd64")},
    {"component": _C, "platform": "win_amd64", "catalog": _catalog(_C, _entry()),
     "files": {"bin/fake.dll": 1024}, "sentinel": "1.0",
     "manifest": _manifest(version="1.0", component_api=1, platform="win_amd64")},
    {"component": _C, "platform": "win_amd64", "catalog": _catalog(_C, _entry(install_bytes=10)),
     "files": {"bin/fake.dll": 50_000}, "sentinel": "1.0",
     "manifest": _manifest(version="1.0", component_api=1, platform="win_amd64")},
    {"component": _C, "platform": "win_amd64", "catalog": _catalog(_C, _entry(install_bytes=10)),
     "files": {"bin/fake.dll": 5_000_000}, "sentinel": "1.0",
     "manifest": _manifest(version="1.0")},
    {"component": _C, "platform": "win_amd64", "catalog": _catalog(_C, _entry()),
     "files": {"bin/fake.dll": 1024}, "sentinel": "2.0",
     "manifest": _manifest(version="2.0", component_api=2)},
    {"component": _C, "platform": "win_amd64", "catalog": _catalog(_C, _entry()),
     "files": {"bin/fake.dll": 1024}, "sentinel": "2.0",
     "manifest": _manifest(version="2.0", platform="linux_x86_64")},
    {"component": _C, "platform": "linux_x86_64", "catalog": _catalog(_C, _entry()),
     "files": {"bin/fake.dll": 1024}, "sentinel": "2.0", "manifest": _manifest(version="2.0")},
    {"component": _C, "platform": "win_amd64", "catalog": None,
     "files": {"bin/fake.dll": 1024}, "sentinel": "x", "manifest": _manifest(version=3)},
    {"component": _C, "platform": "win_amd64", "catalog": _catalog(_C, _entry(version=3)),
     "files": {"bin/fake.dll": 1024}, "sentinel": "x", "manifest": _manifest(version=3.0)},
    {"component": _C, "platform": "win_amd64", "catalog": _catalog(_C, _entry(version="")),
     "files": {"bin/fake.dll": 1024}, "sentinel": "x", "manifest": _manifest(version="1")},
    {"component": _C, "platform": "win_amd64", "catalog": _catalog(_C, _entry()),
     "files": {"bin/fake.dll": 1024}, "sentinel": "x", "manifest": _manifest()},
    {"component": _C, "platform": "win_amd64", "catalog": _catalog(_C, _entry(), "linux_x86_64"),
     "files": {"bin/fake.dll": 1024}, "sentinel": "1", "manifest": _manifest(version="1")},
    {"component": _C, "platform": "win_amd64", "catalog": _catalog(_C, {}),
     "files": {"bin/fake.dll": 1024}, "sentinel": "1", "manifest": _manifest(version="1")},
]

CASES = {
    "check_compatibility": [
        [{}, "win_amd64"],
        [{"component_api": 1}, "win_amd64"],
        [{"component_api": 1.0}, "win_amd64"],
        [{"component_api": 2}, "win_amd64"],
        [{"component_api": 0}, "linux_x86_64"],
        [{"component_api": "1"}, "win_amd64"],
        [{"component_api": None, "platform": None}, "win_amd64"],
        [{"platform": "win_amd64"}, "win_amd64"],
        [{"platform": "linux_x86_64"}, "win_amd64"],
        [{"platform": "linux_x86_64"}, "linux_aarch64"],
        [{"platform": "linux_x86_64"}, None],
        [{"platform": ""}, "win_amd64"],
        [{"platform": ""}, None],
        [{"component_api": 2, "platform": "linux_x86_64"}, "win_amd64"],
        [{"component_api": 1, "platform": "darwin_arm64"}, "darwin_arm64"],
        [{"version": "0.1.0-py312", "component_api": 1, "platform": "win_amd64"}, "win_amd64"],
    ],
    "component_is_published": [[component, tag] for component in _NEMO + ["gpu-x"]
                               for tag in _TAGS],
    "available_component_ids": [
        [tag, installed, gpu]
        for tag in _TAGS
        for installed in ([], ["asr-nvidia-vulkan"])
        for gpu in _GPUS
    ],
    "read_manifest": [
        [None],
        [""],
        ["[1,2,3]"],
        ["\"text\""],
        ["{}"],
        ["{not json"],
        ['{"platform": ["win_amd64"], "component_api": "x"}'],
        ['{"component_api": true, "version": false}'],
        ['{"component_api": 1, "version": true}'],
        ['{"component_api": 1.0}'],
        ['{"component_api": 1, "version": 2}'],
        ['{"component_api": 1, "version": 2.5}'],
        ['{"version": [1]}'],
        ['{"version": {"a": 1}}'],
        ['{"version": null, "platform": null, "component_api": null, "python_abi": null}'],
        ['{"python_abi": 312}'],
        ['{"python_abi": "cp312", "platform": "win_amd64"}'],
        ['{"platform": 5}'],
        ['{"version": "0.1.0-py312", "component_api": 1, "platform": "win_amd64", '
         '"install_bytes": 454179072, "archives": []}'],
        ['﻿{"version": "1"}'],
    ],
    "describe": [[scenario] for scenario in _DESCRIBE],
}

WRAPPERS = {
    "check_compatibility": _check_compatibility,
    "available_component_ids": _available_component_ids,
    "read_manifest": _read_manifest,
    "describe": _describe,
}

PINS = {
    "host_note": "check_compatibility(manifest, host_tag); available_component_ids(tag, "
                 "installed ids, NVML GPU or None); read_manifest(text or None for no file); "
                 "describe(scenario) builds the scenario's tree and catalog, with GPU "
                 "Acceleration reported absent",
}
