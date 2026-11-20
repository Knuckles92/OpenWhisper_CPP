"""Write src/agentapi/data/openapi.json: the OpenAPI document of OpenWhisper's History API.

    $env:FIORIVOCE_OW_ROOT = 'D:\\coding\\whisper_local'
    & $env:FIORIVOCE_OW_PYTHON -I -B tools/gen/agent_openapi.py [--check]

FastAPI generates the document from services/agent_api/app.py's routes and models
(app.openapi()); FioriVoce serves it unchanged at /openapi.json except for the title, which names
the product (agentapi::HistoryApi puts identity().product in it again when it loads the file).
The app is built under tools/parity/ow_isolation.py over an empty database in out/parity, so
nothing of the owner's data is read. --check compares with the committed file instead of writing
it and fails on a difference.
"""
from __future__ import annotations

import argparse
import json
import shutil
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools" / "parity"))
import ow_isolation  # noqa: E402

OUTPUT = ROOT / "src" / "agentapi" / "data" / "openapi.json"
OPENWHISPER, FIORIVOCE = "OpenWhisper", "FioriVoce"


def document() -> str:
    checkout = ow_isolation.checkout()
    ow_isolation.check_reference_commit(checkout)
    data = ow_isolation.isolate()
    ow_isolation.after_import_guards(data)
    from services.agent_api.app import create_app
    from services.database import DatabaseManager

    work = ow_isolation.SCRATCH / "agent-openapi"
    shutil.rmtree(work, ignore_errors=True)
    work.mkdir(parents=True)
    database = work / "history.db"
    DatabaseManager(str(database)).close()
    schema = create_app(database, "x" * 32).openapi()
    title = schema["info"]["title"]
    if OPENWHISPER not in title:
        raise SystemExit(f"The title {title!r} no longer names {OPENWHISPER}; update this script.")
    schema["info"]["title"] = title.replace(OPENWHISPER, FIORIVOCE)
    text = json.dumps(schema, indent=2, ensure_ascii=False) + "\n"
    if OPENWHISPER in text:
        raise SystemExit(f"{OPENWHISPER} appears outside the title; decide how to rebrand it.")
    return text


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--check", action="store_true",
                        help="compare with the committed file instead of writing it")
    args = parser.parse_args(argv)
    text = document()
    if args.check:
        current = OUTPUT.read_text(encoding="utf-8") if OUTPUT.is_file() else ""
        if current != text:
            print(f"{OUTPUT.relative_to(ROOT)} is out of date; run tools/gen/agent_openapi.py")
            return 1
        return 0
    OUTPUT.parent.mkdir(parents=True, exist_ok=True)
    with OUTPUT.open("w", encoding="utf-8", newline="\n") as out:
        out.write(text)
    print(f"{OUTPUT.relative_to(ROOT)}: {len(text)} characters")
    return 0


if __name__ == "__main__":
    sys.exit(main())
