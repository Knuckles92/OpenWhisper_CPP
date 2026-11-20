"""Write src/focus/data/app_catalog.json: OpenWhisper's app catalogue (known apps and sites).

    $env:OPENWHISPER_CPP_OW_ROOT = 'D:\\coding\\whisper_local'
    & $env:OPENWHISPER_CPP_OW_PYTHON -I -B tools/gen/app_catalog.py [--check]

The tables are read from services/focus_context/catalog.py by importing it from the read-only
checkout through tools/parity/ow_isolation.py, so fv_focus names, classifies and matches apps
with exactly OpenWhisper's data. Apps and sites keep their declaration order; "by_id" and
"site_by_key" are the module's own lookup dicts (a later entry wins a repeated key, as in the
dict comprehensions), and the title patterns are the compiled patterns' source text. --check
compares with the committed file instead of writing it and fails on a difference.
"""

from __future__ import annotations

import argparse
import json
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools" / "parity"))
import ow_isolation  # noqa: E402

OUTPUT = ROOT / "src" / "focus" / "data" / "app_catalog.json"


def _pattern(compiled: re.Pattern) -> dict:
    flags = compiled.flags & ~re.UNICODE
    return {"pattern": compiled.pattern, "flags": flags}


def render(commit: str) -> str:
    from services.focus_context import catalog

    apps = list(catalog._APPS)
    sites = list(catalog._SITES)
    app_index = {id(app): index for index, app in enumerate(apps)}
    site_index = {id(site): index for index, site in enumerate(sites)}
    document = {
        "source": "services/focus_context/catalog.py",
        "source_commit": commit,
        "categories": list(catalog.CATEGORIES),
        "surfaces": [catalog.TEXT, catalog.CODE, catalog.TERMINAL],
        "max_overrides": catalog.MAX_OVERRIDES,
        "max_match_chars": catalog.MAX_MATCH_CHARS,
        "title_limit": catalog._TITLE_LIMIT,
        "suffixes": list(catalog._SUFFIXES),
        "patterns": {
            "invisible": _pattern(catalog._INVISIBLE),
            "separators": _pattern(catalog._SEPARATORS),
            "counter": _pattern(catalog._COUNTER),
            "more_pages": _pattern(catalog._MORE_PAGES),
        },
        "apps": [
            {"name": app.name, "category": app.category, "surface": app.surface,
             "ids": list(app.ids), "browser": bool(app.browser)}
            for app in apps
        ],
        "sites": [
            {"name": site.name, "category": site.category, "aliases": list(site.aliases)}
            for site in sites
        ],
        "by_id": {key: app_index[id(app)] for key, app in catalog._BY_ID.items()},
        "site_by_key": {key: site_index[id(site)] for key, site in catalog._SITE_BY_KEY.items()},
        "featured": {category: list(names) for category, names in catalog.FEATURED.items()},
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
            print(f"{OUTPUT.relative_to(ROOT)} is out of date; run tools/gen/app_catalog.py")
            return 1
        print(f"{OUTPUT.relative_to(ROOT)} is up to date")
        return 0
    OUTPUT.parent.mkdir(parents=True, exist_ok=True)
    OUTPUT.write_bytes(text.encode("utf-8"))
    print(f"wrote {OUTPUT.relative_to(ROOT)}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
