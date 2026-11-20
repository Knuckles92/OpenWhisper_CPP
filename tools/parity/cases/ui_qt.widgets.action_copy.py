"""Curated inputs for ui_qt/widgets/action_copy.py (M5.3 automation-core).

An action is a dict of Action fields; a result a dict of ActionResult fields. Paths follow the
platform the vectors were made on (pins.platform): summaries of program actions name the file
as that platform's os.path.basename does.

    summary(action) / spoken_hint(action)
    try_outcome(result)
    tables()        the module's constants and label tables, as data
"""


def _action(spec):
    from services.actions.model import Action

    spec = dict(spec)
    if "spoken_names" in spec:
        spec["spoken_names"] = tuple(spec["spoken_names"])
    return Action(**spec)


def _result(spec):
    from services.actions.runtime import ActionResult

    return ActionResult(**spec)


def _tables(module):
    return {
        "SAMPLE_WORDS": module.SAMPLE_WORDS,
        "TRY_OUTPUT_LINES": module.TRY_OUTPUT_LINES,
        "TRY_OUTPUT_CHARS": module.TRY_OUTPUT_CHARS,
        "KIND_ICONS": [[k, v] for k, v in module.KIND_ICONS.items()],
        "KIND_BLURBS": [[k, v] for k, v in module.KIND_BLURBS.items()],
        "OUTPUT_LABELS": [[k, v] for k, v in module.OUTPUT_LABELS.items()],
        "MODE_LABELS": [[k, v] for k, v in module.MODE_LABELS.items()],
        "SURFACE_LABELS": [[k, v] for k, v in module.SURFACE_LABELS.items()],
        "SURFACE_HINTS": [[k, v] for k, v in module.SURFACE_HINTS.items()],
        "NO_APP_HINT": module.NO_APP_HINT.format(name="OpenCode"),
    }


WRAPPERS = {
    "summary": lambda m, spec: m.summary(_action(spec)),
    "spoken_hint": lambda m, spec: m.spoken_hint(_action(spec)),
    "try_outcome": lambda m, spec: m.try_outcome(_result(spec)),
    "parse_spoken_names": lambda m, text: list(m.parse_spoken_names(text)),
    "tables": _tables,
}


def _a(kind, **params):
    return {"id": "x", "name": "X", "kind": kind, "params": params}


CASES = {
    "summary": [
        [_a("open_link", template="claude://claude.ai/new?q={text}")],
        [_a("open_link", template="codex://new?prompt={text}")],
        [_a("open_link", template="https://chatgpt.com/?q={text}")],
        [_a("open_link", template="https://www.perplexity.ai/search?q={text}")],
        [_a("open_link", template="obsidian://new?content={text}")],
        [_a("open_link", template="https://claude.ai/new?q={text}")],
        [_a("open_link", template="https://www.chatgpt.com/?q={text}")],
        [_a("open_link", template="http://[::1/x")],
        [_a("open_link", template="https:///nohost")],
        [_a("open_link", template="no scheme")],
        [_a("open_link", template="")],
        [_a("open_link")],
        [_a("open_link", template="MAILTO:a@b")],
        [_a("open_link", template="https://WWW.Example.COM:8080/x")],
        [_a("agent", agent="codex", mode="handoff")],
        [_a("agent", agent="codex", mode="handoff", new_folder=True)],
        [_a("agent", agent="claude_code", mode="ask", output="scratchpad")],
        [_a("agent", agent="opencode_cli", mode="ask", output="paste")],
        [_a("agent", agent="claude_code", mode="ask", output="notice")],
        [_a("agent", agent="claude_code", mode="ask", output="clipboard")],
        [_a("agent", agent="claude_code", mode="ask", output="none")],
        [_a("agent", agent="gpt", mode="handoff")],
        [_a("agent")],
        [_a("agent", agent="codex", new_folder=1)],
        [_a("channel")],
        [_a("webhook", url="http://homeassistant.local/api/webhook/x", output="notice")],
        [_a("webhook", url="https://n8n.example.com/hook", output="none")],
        [_a("webhook", url="", output="paste")],
        [_a("webhook", url="http://[::1/x", output="scratchpad")],
        [_a("webhook", url="https://USER@Host.Example:9/x", output="clipboard")],
        [_a("webhook")],
        [_a("program", program="C:\\tools\\journal.exe", output="clipboard")],
        [_a("program", program="tools\\journal.exe", output="none")],
        [_a("program", program="C:/tools/journal.exe", output="notice")],
        [_a("program", program="C:journal.exe", output="paste")],
        [_a("program", program="C:\\tools\\", output="scratchpad")],
        [_a("program", program="\\\\server\\share", output="none")],
        [_a("program", program="\\\\server\\share\\run.cmd", output="none")],
        [_a("program", program="", output="none")],
        [_a("program")],
        [_a("clipboard")],
        [_a("scripts")],
        [_a("shell")],
    ],
    "link_destination": [
        ["claude://x"], ["codex://x"], ["https://chatgpt.com/"], ["https://www.claude.ai/x"],
        ["https://example.com"], ["http://"], ["obsidian://new"], [""], ["\u00e9t\u00e9:x"],
        ["ms-word:x"], ["https://www.www.example.com"],
    ],
    "shortened": [
        [""], [None], ["one line"], ["  padded  "], ["a\nb\nc\nd\ne\nf"], ["a\nb\nc\nd\ne\nf\ng"],
        ["a\r\nb\rc\x0bd\x0ce\x1cf\x1dg"], ["x" * 500], ["x" * 501], ["x" * 499 + " y"],
        ["\u00e9" * 501], ["line\n\n\nmore"], ["a\u2028b\u2029c\x85d"], ["  \n  trailing  \n"],
        [("w" * 100 + "\n") * 6], ["a" * 498 + "   b"],
    ],
    "try_outcome": [
        [{"ok": True, "message": ""}], [{"ok": False, "message": ""}],
        [{"ok": True, "message": "Sent to Home"}],
        [{"ok": True, "message": "", "output": "The answer"}],
        [{"ok": False, "message": "Failed", "output": "a\nb\nc\nd\ne\nf\ng\nh"}],
        [{"ok": True, "message": "Done", "output": None}],
        [{"ok": True, "message": "Done", "output": ""}],
        [{"ok": True, "message": "Done", "output": "   "}],
        [{"ok": False, "message": "Canceled", "stopped": True}],
    ],
    "spoken_hint": [
        [{"id": "x", "name": "X", "kind": "clipboard", "spoken_names": ["House"]}],
        [{"id": "x", "name": "X", "kind": "clipboard", "spoken_names": ["a", "b"]}],
        [{"id": "x", "name": "X", "kind": "clipboard"}],
    ],
    "parse_spoken_names": [
        [" Claude ,  ask   Claude,, "], [""], [None], [","], ["one"], ["a\tb , c\nd"],
        ["Claude, claude"],
    ],
    "parse_arguments": [
        ["--append\n\n  --title My day \n"], [""], [None], ["a\r\nb\rc"], ["  \n \t \n"],
        ["one two"],
    ],
    "agent_name": [["claude_code"], ["codex"], ["opencode_cli"], ["gpt"], [""]],
    "tables": [[]],
}
