"""Write src/models/data/speech_catalog.json: the pinned speech models and NeMo-Speech.cpp runtimes.

    $env:FIORIVOCE_OW_ROOT = 'D:\\coding\\whisper_local'
    & $env:FIORIVOCE_OW_PYTHON -I -B tools/gen/speech_catalog.py [--check]

Reads OpenWhisper's pinned catalogs as files (nothing is imported from the checkout):
services/local_asr/models.json for the Parakeet-family models FioriVoce offers, and the
nvidia_*_runtime.json files that services/local_asr/catalog.py:runtime_catalog merges, keeping
its order. FioriVoce runs the runtime in process, so the Windows entries lose the Python
embeddable zip OpenWhisper's worker needed, and their install_bytes lose what that zip
extracts. Everything else, version strings included, is copied as it is: an install adopted from
OpenWhisper then reads as current, not as "Update available". --check compares with the
committed file instead of writing it and fails on a difference.
"""

from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools" / "parity"))
import ow_isolation  # noqa: E402

OUTPUT = ROOT / "src" / "models" / "data" / "speech_catalog.json"

# The models of the backends FioriVoce keeps (catalog.py MODELS whose backend is "parakeet").
MODEL_KEYS = ("parakeet-v3", "orukeet-v0.1")

# (component id, platform tag, file), in the order runtime_catalog() adds them.
RUNTIMES = (
    ("asr-nvidia-cpu", "win_amd64", "nvidia_cpu_runtime.json"),
    ("asr-nvidia-cuda", "win_amd64", "nvidia_cuda_runtime.json"),
    ("asr-nvidia-cpu", "darwin_arm64", "nvidia_macos_runtime.json"),
    ("asr-nvidia-cpu", "darwin_x86_64", "nvidia_macos_intel_runtime.json"),
    ("asr-nvidia-metal", "darwin_arm64", "nvidia_macos_metal_runtime.json"),
    ("asr-nvidia-cpu", "linux_x86_64", "nvidia_linux_cpu_runtime.json"),
    ("asr-nvidia-cuda", "linux_x86_64", "nvidia_linux_cuda_runtime.json"),
    ("asr-nvidia-vulkan", "linux_x86_64", "nvidia_linux_vulkan_runtime.json"),
)

# The Python embeddable zip's archive name prefix in the Windows entries.
PYTHON_ARCHIVE_PREFIX = "python-3.12.10-embed-"
# Bytes python-3.12.10-embed-amd64.zip extracts to, as an OpenWhisper install of
# asr-nvidia-cuda holds them (its top-level files beside bin/, include/, lib/ and share/; the
# python312._pth there is OpenWhisper's 31-byte rewrite). Measured 2026-11-14.
PYTHON_EMBED_EXTRACTED_BYTES = 22_501_250


def _load(path: Path) -> dict:
    # catalog.py reads the Windows files as utf-8-sig, the rest as utf-8.
    return json.loads(path.read_text(encoding="utf-8-sig"))


def _without_python(entry: dict) -> dict:
    kept = [a for a in entry["archives"] if not a["name"].startswith(PYTHON_ARCHIVE_PREFIX)]
    if len(kept) == len(entry["archives"]):
        return entry
    if len(kept) != len(entry["archives"]) - 1:
        raise SystemExit("expected exactly one Python archive in a Windows runtime entry")
    result = {}
    for key, value in entry.items():
        if key == "install_bytes":
            value = value - PYTHON_EMBED_EXTRACTED_BYTES
        elif key == "archives":
            value = kept
        result[key] = value
    return result


def render(checkout: Path, commit: str) -> str:
    folder = checkout / "services" / "local_asr"
    models = _load(folder / "models.json")
    components: dict = {}
    for component_id, platform, filename in RUNTIMES:
        entry = _load(folder / filename)
        if entry.get("platform") != platform:
            raise SystemExit(f"{filename} is not for {platform}")
        components.setdefault(component_id, {"platforms": {}})["platforms"][platform] = (
            _without_python(entry)
        )
    document = {
        "source": "services/local_asr/models.json and nvidia_*_runtime.json",
        "source_commit": commit,
        "python_embed_extracted_bytes": PYTHON_EMBED_EXTRACTED_BYTES,
        "models": {key: models[key] for key in MODEL_KEYS},
        "components": components,
    }
    return json.dumps(document, indent=2, ensure_ascii=True) + "\n"


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--check", action="store_true",
                        help="compare with the committed file instead of writing it")
    args = parser.parse_args(argv)

    checkout = ow_isolation.checkout()
    commit = ow_isolation.check_reference_commit(checkout)
    text = render(checkout, commit)
    if args.check:
        current = OUTPUT.read_text(encoding="utf-8") if OUTPUT.is_file() else ""
        if current != text:
            print(f"{OUTPUT.relative_to(ROOT)} is out of date; run tools/gen/speech_catalog.py")
            return 1
        print(f"{OUTPUT.relative_to(ROOT)} is up to date")
        return 0
    OUTPUT.parent.mkdir(parents=True, exist_ok=True)
    OUTPUT.write_bytes(text.encode("utf-8"))
    print(f"wrote {OUTPUT.relative_to(ROOT)}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
