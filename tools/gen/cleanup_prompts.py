"""Write src/text/data/cleanup_prompts.json: the text AI cleanup is told, as OpenWhisper has it.

    $env:FIORIVOCE_OW_ROOT = 'D:\\coding\\whisper_local'
    & $env:FIORIVOCE_OW_PYTHON -I -B tools/gen/cleanup_prompts.py [--check]

Prompt text is the product: one changed space changes what every model does, so no sentence is
retyped. The generator imports config, services.cleanup_profiles, services.text_transforms and
services.app_styles from the read-only checkout through tools/parity/ow_isolation.py and writes
every cleanup sentence, the level presets with the sentences each joins, the old default prompt,
the rule-polish prompt, the starter profiles and transforms, and the app-style tone blocks. It
fails when a preset is no longer the space-joined sentences named for it. --check compares with
the committed file instead of writing it and fails on a difference.
"""

from __future__ import annotations

import argparse
import json
import sys
from dataclasses import asdict
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools" / "parity"))
import ow_isolation  # noqa: E402

OUTPUT = ROOT / "src" / "text" / "data" / "cleanup_prompts.json"

# The config attributes of the cleanup sentences, in config.py's order.
SENTENCES = (
    "TRANSCRIPT_CLEANUP_INTRO",
    "TRANSCRIPT_CLEANUP_BASICS",
    "TRANSCRIPT_CLEANUP_LIGHT_ONLY",
    "TRANSCRIPT_CLEANUP_TIGHTEN",
    "TRANSCRIPT_CLEANUP_SPOKEN_CORRECTIONS",
    "TRANSCRIPT_CLEANUP_SPOKEN_LISTS",
    "TRANSCRIPT_CLEANUP_PARAGRAPHS",
    "TRANSCRIPT_CLEANUP_UNKNOWN_APP_LINES",
    "TRANSCRIPT_CLEANUP_TERMINAL_LINES",
    "TRANSCRIPT_CLEANUP_LANGUAGE",
    "TRANSCRIPT_CLEANUP_GUARD",
)

# Which sentences each level preset joins with one space (config.TRANSCRIPT_CLEANUP_LEVEL_PROMPTS).
LEVELS = {
    "light": ("INTRO", "BASICS", "LIGHT_ONLY", "LANGUAGE", "GUARD"),
    "medium": ("INTRO", "BASICS", "SPOKEN_CORRECTIONS", "SPOKEN_LISTS", "PARAGRAPHS",
               "LANGUAGE", "GUARD"),
    "high": ("INTRO", "BASICS", "TIGHTEN", "SPOKEN_CORRECTIONS", "SPOKEN_LISTS", "PARAGRAPHS",
             "LANGUAGE", "GUARD"),
}


def render(commit: str) -> str:
    from config import config
    from services import app_styles, cleanup_profiles, text_transforms

    sentences = {name: getattr(config, name) for name in SENTENCES}
    presets = dict(config.TRANSCRIPT_CLEANUP_LEVEL_PROMPTS)
    if list(presets) != list(LEVELS):
        raise SystemExit(f"The cleanup levels changed: {list(presets)}")
    levels = {}
    for level, parts in LEVELS.items():
        names = [f"TRANSCRIPT_CLEANUP_{part}" for part in parts]
        if " ".join(sentences[name] for name in names) != presets[level]:
            raise SystemExit(f"The {level} preset is no longer {', '.join(parts)} joined.")
        levels[level] = {"sentences": names, "prompt": presets[level]}
    document = {
        "source": ("config.py, services/cleanup_profiles.py, services/text_transforms.py, "
                   "services/app_styles.py"),
        "source_commit": commit,
        "sentences": sentences,
        "levels": levels,
        "transcript_cleanup_prompt": config.TRANSCRIPT_CLEANUP_PROMPT,
        "legacy_default_cleanup_prompts": list(config.LEGACY_DEFAULT_CLEANUP_PROMPTS),
        "rule_polish_prompt": config.TRANSCRIPT_CLEANUP_RULE_POLISH_PROMPT,
        "starter_profiles": [asdict(profile) for profile in cleanup_profiles.STARTER_PROFILES],
        "starter_transforms": [asdict(item) for item in text_transforms.STARTER_TRANSFORMS],
        "app_styles": {
            "writing": dict(app_styles._WRITING),
            "tone_prompts": dict(app_styles._TONE_PROMPTS),
            "surface_prompts": dict(app_styles._SURFACE_PROMPTS),
        },
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
    # config and the services import services.settings: check that OpenWhisper's paths resolved
    # into scratch, and swap its services for null ones, before importing them.
    ow_isolation.after_import_guards(data)
    text = render(commit)
    if args.check:
        current = OUTPUT.read_text(encoding="utf-8") if OUTPUT.is_file() else ""
        if current != text:
            print(f"{OUTPUT.relative_to(ROOT)} is out of date; run tools/gen/cleanup_prompts.py")
            return 1
        print(f"{OUTPUT.relative_to(ROOT)} is up to date")
        return 0
    OUTPUT.parent.mkdir(parents=True, exist_ok=True)
    OUTPUT.write_bytes(text.encode("utf-8"))
    print(f"wrote {OUTPUT.relative_to(ROOT)}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
