"""Write src/text/data/common_words.json: the words dictionary learning never learns.

    $env:OPENWHISPER_CPP_OW_ROOT = 'D:\\coding\\whisper_local'
    & $env:OPENWHISPER_CPP_OW_PYTHON -I -B tools/gen/dictionary_words.py [--check]

The list is services/dictionary.py's COMMON_WORDS, read by importing the module from the
read-only checkout through tools/parity/ow_isolation.py, so fv_text refuses exactly the words
OpenWhisper does. COMMON_WORDS is a frozenset, so the words are written sorted. --check compares
with the committed file instead of writing it and fails on a difference.
"""

from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools" / "parity"))
import ow_isolation  # noqa: E402

OUTPUT = ROOT / "src" / "text" / "data" / "common_words.json"


def render(commit: str) -> str:
    from services import dictionary

    words = sorted(dictionary.COMMON_WORDS)
    if any(word != word.casefold() for word in words):
        raise SystemExit("COMMON_WORDS holds a word that is not casefolded.")
    document = {
        "source": "services/dictionary.py",
        "source_commit": commit,
        "words": words,
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
    # services.dictionary imports services.settings and focus_context: check that OpenWhisper's
    # paths resolved into scratch, and swap its services for null ones, before importing it.
    ow_isolation.after_import_guards(data)
    text = render(commit)
    if args.check:
        current = OUTPUT.read_text(encoding="utf-8") if OUTPUT.is_file() else ""
        if current != text:
            print(f"{OUTPUT.relative_to(ROOT)} is out of date; run tools/gen/dictionary_words.py")
            return 1
        print(f"{OUTPUT.relative_to(ROOT)} is up to date")
        return 0
    OUTPUT.parent.mkdir(parents=True, exist_ok=True)
    OUTPUT.write_bytes(text.encode("utf-8"))
    print(f"wrote {OUTPUT.relative_to(ROOT)}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
