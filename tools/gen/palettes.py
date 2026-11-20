"""Write src/ui_theme/data/palettes.json: OpenWhisper's dark and light palette tokens.

    $env:OPENWHISPER_CPP_OW_ROOT = 'D:\\coding\\whisper_local'
    & $env:OPENWHISPER_CPP_OW_PYTHON -I -B tools/gen/palettes.py [--check]

The tokens are read from ui_qt/utils/palette.py by importing it from the read-only checkout
through tools/parity/ow_isolation.py, so fv_ui_theme resolves theme.qss against exactly the
values OpenWhisper does. Token order is the dict order of palette.py. --check compares with the
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

OUTPUT = ROOT / "src" / "ui_theme" / "data" / "palettes.json"


def render(commit: str) -> str:
    from ui_qt.utils import palette

    palettes = {name: dict(value.tokens) for name, value in palette.PALETTES.items()}
    if set(palettes) != {palette.DARK, palette.LIGHT}:
        raise SystemExit(f"palette.py defines {sorted(palettes)}, not dark and light.")
    if set(palettes[palette.DARK]) != set(palettes[palette.LIGHT]):
        raise SystemExit("The dark and light palettes define different tokens.")
    document = {
        "source": "ui_qt/utils/palette.py",
        "source_commit": commit,
        "default": palette.DARK,
        "palettes": palettes,
    }
    return json.dumps(document, indent=1, ensure_ascii=False) + "\n"


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--check", action="store_true",
                        help="compare with the committed file instead of writing it")
    args = parser.parse_args(argv)

    checkout = ow_isolation.checkout()
    commit = ow_isolation.check_reference_commit(checkout)
    ow_isolation.isolate()
    text = render(commit)
    if args.check:
        current = OUTPUT.read_text(encoding="utf-8") if OUTPUT.is_file() else ""
        if current != text:
            print(f"{OUTPUT.relative_to(ROOT)} is out of date; run tools/gen/palettes.py")
            return 1
        print(f"{OUTPUT.relative_to(ROOT)} is up to date")
        return 0
    OUTPUT.parent.mkdir(parents=True, exist_ok=True)
    OUTPUT.write_bytes(text.encode("utf-8"))
    print(f"wrote {OUTPUT.relative_to(ROOT)}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
