"""Classify OpenWhisper's test files by port area and by kind.

Reads only. Usage:
    python -I -B tools/parity/classify_tests.py [<openwhisper checkout> [<areas.json> [<out dir>]]]
The checkout defaults to OPENWHISPER_CPP_OW_ROOT, areas.json to docs/port/areas.json and the output
folder to tools/parity/. Writes classification.json and classification.md there; a rerun on the
same checkout gives the same bytes.

Area: every repo module a test imports (AST imports plus dotted-name strings
such as monkeypatch targets) is resolved to a file and looked up in
areas.json. The primary area is the one whose module stem matches the test
file name, else the area with the most referenced modules.

Kind (first rule that matches wins):
  dropped-engine  references a local engine OpenWhisper_CPP will not port
  platform        skips/branches on a platform or imports a platform backend
  qt-widget       imports PyQt6 or ui_qt (conftest's own qt rule)
  integration     fakes, sockets, subprocess, threads, HTTP mocks, sqlite db
  pure-logic      everything else
"""
from __future__ import annotations

import ast
import collections
import json
import os
import re
import sys
from pathlib import Path

DROPPED = re.compile(
    r"faster_whisper|ctranslate2|moonshine|qwen|local_asr\.mlx|parakeet_mlx|\bmlx\b|"
    r"transcribe_engine|isolated_worker|services\.isolated\b|whisper_sources|"
    r"custom_whisper|local_asr\.process|local_asr\.worker|qwen_batch"
)
DROPPED_FILES = {
    "transcriber/local_backend.py", "services/isolated.py", "services/isolated_worker.py",
    "services/whisper_sources.py", "services/local_asr/moonshine.py",
    "services/local_asr/mlx.py", "services/local_asr/qwen_batch.py",
    "services/local_asr/qwen_speech.py", "services/local_asr/transcribe_engine.py",
    "services/local_asr/process.py", "services/local_asr/worker.py",
    "ui_qt/dialogs/custom_whisper_dialog.py",
}
# a skip/branch on the platform; counted, so one Mac-only case in a widget
# test does not make the whole file platform-specific
PLATFORM_BRANCH = re.compile(r"skipif\([^)]*(sys\.platform|platform\.system)|"
                             r"monkeypatch\.setattr\([^)]*sys, \"platform\"")
PLATFORM_MODULE_MARK = re.compile(r"^pytestmark\s*=.*skipif[^\n]*platform", re.M)
PLATFORM_MODULE = re.compile(
    r"_hotkey_carbon|_hotkey_pynput|_hotkey_keyboard|_mouse_hook_win|_mouse_monitor_mac|"
    r"_x11_key_grab|focus_context\._(win|mac|linux)|_win_uia|hyprland|omarchy|"
    r"app_update_macos|macos_dock|snap\.macos|snap\.grim|sck_stream|linux_audio|"
    r"linux_deps|ctypes\.windll|winreg"
)
INTEGRATION = re.compile(
    r"tests\.fakes|from fakes|socket\.|subprocess|threading\.|TestClient|MockTransport|"
    r"DatabaseManager|SqlMeetingRepository|\bdef test_\w+\([^)]*\b(db|repo)\b|"
    r"QThread|asyncio\.run|uvicorn|websocket|http\.server|MagicMock|mock\.patch|\bpatch\("
)


# hand corrections where import weighting picks the app shell for a test
# that is really about settings or text styles
OVERRIDES = {
    "test_setting_defaults.py": "a10-settings-services",
    "test_meeting_speech_follows_dictation.py": "a10-settings-services",
    "test_personalize_s1_styles.py": "a12-text-ai-personalize-services",
    "test_personalize_fix2_g3_stats_dialog.py": "a09-history-stats",
    "test_recording_isolation.py": "a05-recording-audio",
    "test_hotkey_headless.py": "a06-hotkeys-paste-focus",
    "test_qwen_batch_worker.py": "a14-models-components-services",
    "test_whisper_av_compat.py": "a14-models-components-services",
    "test_frozen_updater.py": "a16-updater-setup-packaging",
    "test_windows_installer.py": "a16-updater-setup-packaging",
    "test_build_component.py": "a14-models-components-services",
}


def load_areas(path: Path) -> dict[str, str]:
    areas = json.loads(path.read_text(encoding="utf-8"))
    return {f: key for key, v in areas.items() for f in v["files"]}


def module_files(root: Path, dotted: str) -> list[str]:
    parts = dotted.split(".")
    found = []
    for i in range(len(parts), 0, -1):
        base = root.joinpath(*parts[:i])
        for cand in (base.with_suffix(".py"), base / "__init__.py"):
            if cand.is_file():
                rel = cand.relative_to(root).as_posix()
                # a package __init__ counts only when nothing deeper matched
                if not found or not rel.endswith("__init__.py"):
                    found.append(rel)
        if found:
            break
    return found


def referenced_modules(source: str) -> set[str]:
    names: set[str] = set()
    try:
        tree = ast.parse(source)
    except SyntaxError:
        return names
    for node in ast.walk(tree):
        if isinstance(node, ast.Import):
            names.update(a.name for a in node.names)
        elif isinstance(node, ast.ImportFrom) and node.module and not node.level:
            names.add(node.module)
            names.update(f"{node.module}.{a.name}" for a in node.names)
        elif isinstance(node, ast.Constant) and isinstance(node.value, str):
            if re.fullmatch(r"(services|ui_qt|meeting|transcriber|config|main)(\.\w+)*",
                            node.value):
                names.add(node.value)
    return names


def classify(root: Path, file_area: dict[str, str]):
    rows = []
    tests = sorted((root / "tests").glob("test_*.py"))
    sources, imported = {}, {}
    for test in tests:
        source = test.read_text(encoding="utf-8-sig", errors="replace")
        files = set()
        for name in referenced_modules(source):
            if name.split(".")[0] in ("tests", "fakes", "helpers"):
                continue
            files.update(module_files(root, name))
        # modules loaded by file path (spec_from_file_location) name the file
        for base in set(re.findall(r"[\"'](\w+\.py)[\"']", source)):
            files.update(f for f in file_area if f.rsplit("/", 1)[-1] == base
                         and base != "__init__.py")
        sources[test], imported[test] = source, files
    # config.py and settings.py are imported nearly everywhere; weight each
    # module by how rarely tests import it so they do not decide the area
    df = collections.Counter(f for files in imported.values() for f in files)
    for test in tests:
        source, files = sources[test], imported[test]
        hits = collections.Counter()
        for f in sorted(files):
            if f in file_area:
                hits[file_area[f]] += 1.0 / df[f]
        stem = test.stem[len("test_"):]
        primary, best = None, ""
        for f in sorted(files):
            mod = Path(f).stem if not f.endswith("__init__.py") else Path(f).parent.name
            if f in file_area and (stem == mod or stem.startswith(mod + "_")) \
                    and len(mod) > len(best):
                primary, best = file_area[f], mod
        if primary is None and hits:
            primary = min(hits, key=lambda a: (-hits[a], a))
        primary = OVERRIDES.get(test.name, primary)
        if primary is None:
            # nothing in areas.json: name the top-level package it reaches
            tops = sorted({f.split("/")[0] for f in files})
            primary = "unmapped:" + (",".join(tops) if tops else "none")
        uses_qt = "PyQt6" in source or "ui_qt" in source
        dropped_files = {f for f in files if f in DROPPED_FILES}
        dropped_hits = len(DROPPED.findall(source))
        platform_mods = (PLATFORM_MODULE.search(test.stem) or re.search(r"_(mac|macos|win|windows|linux|wayland|x11|hyprland|omarchy)(_|$)", test.stem)
                         or len(PLATFORM_MODULE.findall(source)) >= 5)
        platform_branches = len(PLATFORM_BRANCH.findall(source))
        tests_n = max(1, len(re.findall(r"^\s*(?:async\s+)?def test_", source, re.M)))
        if dropped_files or DROPPED.search(test.stem) or dropped_hits >= 8:
            kind = "dropped-engine"
        elif platform_mods or PLATFORM_MODULE_MARK.search(source) or (
                platform_branches * 2 >= tests_n and platform_branches >= 2):
            kind = "platform"
        elif uses_qt:
            kind = "qt-widget"
        elif INTEGRATION.search(source):
            kind = "integration"
        else:
            kind = "pure-logic"
        rows.append({
            "file": test.name,
            "lines": source.count("\n") + 1,
            "tests": len(re.findall(r"^\s*(?:async\s+)?def test_", source, re.M)),
            "area": primary,
            "otherAreas": sorted(a for a in hits if a != primary),
            "kind": kind,
            "qt": uses_qt,
        })
    return rows


def main() -> None:
    tools = Path(__file__).resolve().parent
    repo = tools.parents[1]
    args = sys.argv[1:]
    if args:
        root = Path(args[0])
    elif os.environ.get("OPENWHISPER_CPP_OW_ROOT"):
        root = Path(os.environ["OPENWHISPER_CPP_OW_ROOT"])
    else:
        raise SystemExit("Pass the OpenWhisper checkout or set OPENWHISPER_CPP_OW_ROOT.")
    areas_path = Path(args[1]) if len(args) > 1 else repo / "docs" / "port" / "areas.json"
    out = Path(args[2]) if len(args) > 2 else tools
    rows = classify(root, load_areas(areas_path))
    out.mkdir(parents=True, exist_ok=True)
    (out / "classification.json").write_bytes(
        (json.dumps(rows, indent=1) + "\n").encode("utf-8"))

    kinds = ["pure-logic", "integration", "qt-widget", "platform", "dropped-engine"]
    by_area = collections.defaultdict(list)
    for r in rows:
        by_area[r["area"]].append(r)
    lines = ["| Area | Files | Tests | " + " | ".join(kinds) + " |",
             "|---|---:|---:|" + "---:|" * len(kinds)]
    for area in sorted(by_area):
        rs = by_area[area]
        c = collections.Counter(r["kind"] for r in rs)
        lines.append(f"| {area} | {len(rs)} | {sum(r['tests'] for r in rs)} | "
                     + " | ".join(str(c[k]) for k in kinds) + " |")
    c = collections.Counter(r["kind"] for r in rows)
    lines.append(f"| **total** | {len(rows)} | {sum(r['tests'] for r in rows)} | "
                 + " | ".join(str(c[k]) for k in kinds) + " |")
    lines.append("")
    for area in sorted(by_area):
        lines.append(f"<details><summary>{area} ({len(by_area[area])})</summary>\n")
        for kind in kinds:
            names = [r["file"][len("test_"):-3] for r in by_area[area] if r["kind"] == kind]
            if names:
                lines.append(f"- **{kind}** ({len(names)}): " + ", ".join(names))
        lines.append("\n</details>\n")
    (out / "classification.md").write_bytes(("\n".join(lines) + "\n").encode("utf-8"))
    print("\n".join(lines[: len(by_area) + 3]))


if __name__ == "__main__":
    main()
