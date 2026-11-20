"""Curated inputs for services/walkthrough/write_up.py (fv_walkthrough's write_up.h).

    document_text(reply, names)         [text, [text_color, heading_color, font]]
    _split_look(text)                   [[text_color, heading_color, font], rest]
    _notes(notes, names)                notes as [text, section]
    agent_name(agent)
    _prompts(agent, instructions, follow_notes, notes, names, duration_s, markdown)
        what write_up() hands the agent, caught by a stand-in run that answers "canceled":
        {"system", "user", "folder", "images"}. The walkthrough lives in a scratch folder under
        out/parity (walkthrough.pdf, walkthrough.md holding `markdown`, an empty file per
        name); its real path reads "<root>" (the platform's separators after it). folder is
        Claude Code's, images the others', each None for the other kind. A walkthrough.md of
        None is missing: {"error": the exception's type}.

The system prompt names OpenWhisper, which FioriVoce's names itself: compare it rebranded.
"""
from __future__ import annotations

import os
import shutil
import tempfile
from pathlib import Path

REPO = Path(__file__).resolve().parents[3]
SCRATCH = REPO / "out" / "parity" / "walkthrough_write_up"


def _look(look):
    return [look.text_color, look.heading_color, look.font]


def _document_text(module, reply, names):
    text, look = module.document_text(reply, names)
    return [text, _look(look)]


def _split_look(module, text):
    look, rest = module._split_look(text)
    return [_look(look), rest]


def _notes(module, notes, names):
    from services.walkthrough.session import Note

    return module._notes([Note(text, section) for text, section in notes], names)


def _prompts(module, agent, instructions, follow_notes, notes, names, duration_s, markdown):
    from meeting.agent.installed.drivers import PassOutcome
    from services.walkthrough.session import Note, WalkthroughResult

    SCRATCH.mkdir(parents=True, exist_ok=True)
    root = Path(tempfile.mkdtemp(dir=SCRATCH))
    real = os.path.realpath(root)
    try:
        (root / "walkthrough.pdf").write_bytes(b"%PDF")
        if markdown is not None:
            (root / "walkthrough.md").write_text(markdown, encoding="utf-8", newline="\n")
        shots = []
        for name in names:
            (root / name).write_bytes(b"")
            shots.append(root / name)
        result = WalkthroughResult(
            folder=root, pdf=root / "walkthrough.pdf", markdown=root / "walkthrough.md",
            clipboard_text="", history_text="", history_raw=None, shots=len(names),
            duration_s=duration_s, first_shot=None,
        )
        seen = {}

        def run(agent_id, **kwargs):
            seen.update(kwargs)
            return PassOutcome(ok=False, canceled=True, error="canceled")

        try:
            module.write_up(result, shots, agent=agent, instructions=instructions,
                            follow_notes=follow_notes,
                            notes=[Note(text, section) for text, section in notes], run=run)
        except module.WriteUpFailed:
            pass
        except Exception as exc:  # the type is the vector
            return {"error": type(exc).__name__}
        if not seen:
            return {"error": "WriteUpFailed"}

        def rooted(text):
            return text.replace(real, "<root>")

        return {
            "system": seen["system_prompt"],
            "user": rooted(seen["user_prompt"]),
            "folder": rooted(seen["folder"]) if "folder" in seen else None,
            "images": [rooted(image) for image in seen["images"]] if "images" in seen else None,
        }
    finally:
        shutil.rmtree(root)


WRAPPERS = {
    "document_text": _document_text,
    "_split_look": _split_look,
    "_notes": _notes,
    "_prompts": _prompts,
}

REPLY = (
    "# Changing the backend\n\n"
    "Open Quick Record and pick the **Backend**.\n\n"
    "![The backend menu](shot-1.png)\n\n"
    "## Then the model\n\n"
    "![The model menu](shot-2.png)\n"
)
NAMES = ["shot-1.png", "shot-2.png"]
HELD = [["make this one bigger", 1], ["leave out the setup part", 2]]
SPOKEN = "# Walkthrough\n\nOpen the menu.\r\n\r\n![Screenshot 1](shot-1.png)\n\n"

CASES = {
    "document_text": [
        [REPLY, NAMES],
        ["```markdown\n# Title\n\nWords\n```", []],
        ["```\n# Title\n```", []],
        ["```md\r\n# Title\r\n\r\nWords\r\n```", []],
        ["Before\n```python\ncode\n```\nAfter", []],
        ["![one](C:\\captures\\run\\shot-1.png)\n\n![two](<./shot-2.png> \"Second\")\n\n"
         "![tracker](https://example.com/pixel.png)\n\n![secret](../../secret.png)\n\n"
         "Words stay.", NAMES],
        ["---\nText colour: purple\nheading_color: \"#5b2a86\"\nfont: Georgia\nmargins: wide\n"
         "---\n\n" + REPLY, NAMES],
        ["---\n\nIntro words.\n\n---\n\nMore.", []],
        ["---\ntext color: purple\n---", []],
        ["   ", []],
        ["```", []],
        ["", []],
        ["!!! ???", []],
        ["![only](shot-1.png)", NAMES],
        ["![a]( shot-1.png )", NAMES],
        ["![a](< shot-2.png >)", NAMES],
        ["![a](shot-3.png)", NAMES],
        ["Ünïcödé wörds", []],
        ["\u3000\u3000", []],
    ],
    "_split_look": [
        ["---\ntext color: purple\n---\n\n# Title"],
        ["---\nHeadings Colour: '#123456'\nFONT:  \"Segoe UI\" \n---\nWords"],
        ["---  \ntext-color: red\nheading color: blue\n---"],
        ["---\nnot a key line\n---\nWords"],
        ["---\n1color: red\n---\nWords"],
        ["# Title\n---\ntext color: red\n---"],
        ["---\ntext color: red\n--- \nAfter"],
        ["---\ntext color: red\ntext color: blue\nfont:\n---\nAfter"],
        ["---\ntext_colour : green\n---"],
        ["---\ntext color: a: b\n---\nx"],
        [""],
    ],
    "_notes": [
        [[], NAMES],
        [HELD, NAMES],
        [[["about the first", 0]], NAMES],
        [[["said with no screenshots", 0]], []],
        [[["after", 5]], ["shot-1.png"]],
    ],
    "agent_name": [["claude_code"], ["codex"], ["opencode_cli"], ["unknown"]],
    "_prompts": [
        ["claude_code", "", False, [], NAMES, 75.0, "as spoken"],
        ["claude_code", "Make it a checklist.", False, [], NAMES, 3725.0, "as spoken"],
        ["claude_code", "  Spaced instructions.  ", True, [], [], 0.4, "as spoken"],
        ["claude_code", "", True, HELD, NAMES, 75.0, "as spoken"],
        ["claude_code", "", False, HELD, NAMES, 75.0, "as spoken"],
        ["codex", "", False, [], NAMES, 75.0, SPOKEN],
        ["codex", "", True, HELD, NAMES, 61.0, SPOKEN],
        ["opencode_cli", "", False, HELD, [], 9.0, "  \n"],
        ["opencode_cli", "Bug report, please.", True, [], NAMES, 75.0, SPOKEN],
        ["codex", "", False, [], NAMES, 75.0, None],
    ],
}
