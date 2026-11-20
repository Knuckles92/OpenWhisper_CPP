"""Write src/uimath/data/overlay_tables.json: the Cleaning-up art's scribbled lines.

    $env:OPENWHISPER_CPP_OW_ROOT = 'D:\\coding\\whisper_local'
    & $env:OPENWHISPER_CPP_OW_PYTHON -I -B tools/gen/overlay_tables.py [--check]

OpenWhisper lays out the three lines of word strokes with random.Random(7) when
ui_qt/overlays/tidy_lines.py is imported. A Mersenne Twister seeded through std::mt19937 gives a
different stream, so the table is dumped as data: per line, each word as [length, dy, dx,
wobble phase]. The module is imported from the read-only checkout through
tools/parity/ow_isolation.py and its LINES are written as they are; floats keep Python's
repr(), which round-trips. --check compares with the committed file instead of writing it and
fails on a difference.
"""

from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools" / "parity"))
import ow_isolation  # noqa: E402

DATA = ROOT / "src" / "uimath" / "data"
OUTPUT = DATA / "overlay_tables.json"
SAMPLE_OUTPUT = DATA / "sample_voice.json"


def render(commit: str) -> str:
    from ui_qt.overlays import tidy_lines

    document = {
        "source": "ui_qt/overlays/tidy_lines.py _scribble(7)",
        "source_commit": commit,
        "seed": 7,
        "half_span": tidy_lines.HALF_SPAN,
        "word_gap": tidy_lines.WORD_GAP,
        "lines": [[list(word) for word in line] for line in tidy_lines.LINES],
    }
    return json.dumps(document, indent=1, ensure_ascii=False) + "\n"


def render_sample_voice(commit: str) -> str:
    from ui_qt.widgets import recording_look_preview as preview

    document = {
        "source": "ui_qt/widgets/recording_look_preview.py _sample_words(3)",
        "source_commit": commit,
        "seed": 3,
        "loop_s": preview.LOOP_S,
        "stages": [[name, start] for name, start in preview.STAGES],
        "words": [list(word) for word in preview._sample_words(3)],
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
    data = ow_isolation.isolate()
    # tidy_lines imports recording_looks, which imports config: check that its paths resolved
    # into scratch before anything else runs.
    ow_isolation.after_import_guards(data)
    outputs = [(OUTPUT, render(commit)), (SAMPLE_OUTPUT, render_sample_voice(commit))]
    if args.check:
        stale = False
        for path, text in outputs:
            current = path.read_bytes().decode("utf-8") if path.is_file() else ""
            if current != text:
                print(f"{path.relative_to(ROOT)} is out of date; run tools/gen/overlay_tables.py")
                stale = True
            else:
                print(f"{path.relative_to(ROOT)} is up to date")
        return 1 if stale else 0
    for path, text in outputs:
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(text.encode("utf-8"))
        print(f"wrote {path.relative_to(ROOT)}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
