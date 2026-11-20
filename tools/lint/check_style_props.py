"""lint.style_props: widget code sets dynamic properties only through
fv::ui::setStyleProperty(widget, prop::<name>, value) and widget-local sheets only through
fv::ui::setLocalStyleSheet, so a theme or scale change can re-resolve them (architecture 8.4).
Fails on a raw setProperty or setStyleSheet call in a src/ui_* directory other than ui_theme
(which implements the helpers), and on a prop::<name> that ui_theme/style_props.h does not
declare.

Usage: check_style_props.py [--root <checkout>]
"""

from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import fvlint  # noqa: E402

RAW_CALL = re.compile(r"(?:->|\.|\b)(setProperty|setStyleSheet)\s*\(")
PROP_USE = re.compile(r"\bprop::(\w+)")
PROP_DECL = re.compile(r"\b(?:inline\s+)?constexpr\b[^;=]*?\b(\w+)\s*(?:\{|=)")


def declared_props(root: Path) -> set[str] | None:
    header = root / "src" / "ui_theme" / "style_props.h"
    if not header.exists():
        return None
    return set(PROP_DECL.findall(strip_comments_and_strings(header.read_text(encoding="utf-8"))))


def strip_comments_and_strings(text: str) -> str:
    """The text with comments and string literals blanked, line breaks kept."""
    out = list(text)
    for _, _, _, start, end in fvlint.cxx_tokens(text):
        for i in range(start, end):
            if out[i] != "\n":
                out[i] = " "
    return "".join(out)


def check(root: Path) -> tuple[list[str], int]:
    src = root / "src"
    props = declared_props(root)
    problems: list[str] = []
    files: list[Path] = []
    for directory in sorted(src.glob("ui_*")):
        if directory.is_dir() and directory.name != "ui_theme":
            files.extend(fvlint.source_files(directory))
    for path in files:
        relative = path.relative_to(root).as_posix()
        code = strip_comments_and_strings(path.read_text(encoding="utf-8", errors="replace"))
        for number, line in enumerate(code.splitlines(), start=1):
            for match in RAW_CALL.finditer(line):
                problems.append(f"{relative}:{number}: raw {match.group(1)}; use "
                                "fv::ui::setStyleProperty or fv::ui::setLocalStyleSheet")
            for match in PROP_USE.finditer(line):
                if props is None:
                    problems.append(f"{relative}:{number}: prop::{match.group(1)} used, but "
                                    "src/ui_theme/style_props.h does not exist")
                elif match.group(1) not in props:
                    problems.append(f"{relative}:{number}: prop::{match.group(1)} is not "
                                    "declared in ui_theme/style_props.h")
    return problems, len(files)


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, default=fvlint.repo_root())
    args = parser.parse_args(argv)
    problems, count = check(args.root)
    return fvlint.report("lint.style_props", problems, f"{count} widget files")


if __name__ == "__main__":
    sys.exit(main())
