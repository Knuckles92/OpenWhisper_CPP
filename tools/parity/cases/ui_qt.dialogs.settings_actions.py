"""Curated inputs for ui_qt/dialogs/settings_actions.py (M5.13 actions-page): the Actions rail
value over the saved actions, the agent action counted apart.
"""


def _row(action_id, name, kind="clipboard", **extra):
    return {"id": action_id, "name": name, "kind": kind, **extra}


_AGENT = _row("agent", "Agent hotkey", "agent", params={"agent": "claude_code"})
_ONE = _row("a1", "Ask Claude", "open_link", params={"template": "claude://claude.ai/new?q={text}"})
_TWO = _row("h1", "Home", "webhook", params={"url": "https://example.com/hook"})

CASES = {
    "rail_value": [[settings] for settings in (
        {},
        {"actions": []},
        {"actions": [_AGENT]},
        {"actions": [_ONE]},
        {"actions": [_ONE, _TWO]},
        {"actions": [_AGENT, _ONE]},
        {"actions": [_AGENT, _ONE, _TWO]},
        {"actions": [_ONE, _row("a1", "Again")]},
        {"actions": [_row("agent", "Not an agent")]},
        {"actions": [_row("x", "Broken", "nonsense"), _ONE]},
        {"actions": [_row(f"c{n}", f"Copy {n}") for n in range(55)]},
        {"actions": "oops"},
        {"actions": None},
        {"actions": [_ONE, "oops", 7]},
    )],
}
