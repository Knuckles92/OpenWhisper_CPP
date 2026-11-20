"""Write src/llm/data/text_model_catalog.json: the reviewed OpenCode routes and model capabilities.

    $env:OPENWHISPER_CPP_OW_ROOT = 'D:\\coding\\whisper_local'
    & $env:OPENWHISPER_CPP_OW_PYTHON -I -B tools/gen/text_model_catalog.py [--check]

Imports services/text_model_catalog.py from the checkout through tools/parity/ow_isolation.py
and copies its _ROUTES (kind -> protocol -> model ids, in the module's order: the first
protocol listing a model is its route) and _CAPABILITIES (kind -> model -> TextModelSpec
fields), with the other constants model_spec reads. The tables are dated data: regenerate them,
never edit the output by hand. --check compares with the committed file instead of writing it
and fails on a difference.
"""

from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools" / "parity"))
import ow_isolation  # noqa: E402

OUTPUT = ROOT / "src" / "llm" / "data" / "text_model_catalog.json"


def render(commit: str) -> str:
    from services import text_model_catalog as catalog

    document = {
        "source": "services/text_model_catalog.py",
        "source_commit": commit,
        "routes": {
            kind: {protocol: list(models) for protocol, models in routes.items()}
            for kind, routes in catalog._ROUTES.items()
        },
        "capabilities": catalog._CAPABILITIES,
        "openai_reasoning_prefixes": list(catalog._OPENAI_REASONING_PREFIXES),
        "openai_responses_text_prefixes": list(catalog._OPENAI_RESPONSES_TEXT_PREFIXES),
        "openai_non_text_markers": list(catalog._OPENAI_NON_TEXT_MARKERS),
        "groq_non_text_markers": list(catalog._GROQ_NON_TEXT_MARKERS),
        "large_context_window": catalog._LARGE_CONTEXT_WINDOW,
        "large_max_output_tokens": catalog._LARGE_MAX_OUTPUT_TOKENS,
        "saved_protocols": list(catalog._SAVED_PROTOCOLS),
        "saved_reasoning_formats": list(catalog._SAVED_REASONING_FORMATS),
    }
    return json.dumps(document, indent=1, ensure_ascii=True) + "\n"


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
            print(f"{OUTPUT.relative_to(ROOT)} is out of date; run tools/gen/text_model_catalog.py")
            return 1
        print(f"{OUTPUT.relative_to(ROOT)} is up to date")
        return 0
    OUTPUT.parent.mkdir(parents=True, exist_ok=True)
    OUTPUT.write_bytes(text.encode("utf-8"))
    print(f"wrote {OUTPUT.relative_to(ROOT)}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
