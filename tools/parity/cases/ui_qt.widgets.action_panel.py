"""Curated inputs for ui_qt/widgets/action_panel.py (M5.13 actions-page), and for
ui_qt/widgets/action_editor.py's onto_saved, which the panel's editor runs on every save.

An action is a dict of Action fields; the results of onto_saved are Action.to_dict().

    unique_name(name, settings)
    saved_note(action)
    onto_saved(saved, baseline, draft)
"""


def _action(spec):
    from services.actions.model import Action

    spec = dict(spec)
    if "spoken_names" in spec:
        spec["spoken_names"] = tuple(spec["spoken_names"])
    return Action(**spec)


def _onto_saved(_module, saved, baseline, draft):
    from ui_qt.widgets import action_editor

    return action_editor.onto_saved(_action(saved), _action(baseline), _action(draft)).to_dict()


WRAPPERS = {
    "saved_note": lambda m, spec: m.saved_note(_action(spec)),
    "onto_saved": _onto_saved,
}


def _row(action_id, name, kind="clipboard", **extra):
    return {"id": action_id, "name": name, "kind": kind, **extra}


_LINK = {"id": "a1", "name": "Ask Claude", "kind": "open_link",
         "params": {"template": "claude://claude.ai/new?q={text}"}, "hotkey": "ctrl+alt+c",
         "shaping": "cleanup", "spoken_names": ["Claude"], "description": "", "enabled": True}
_AGENT = {"id": "agent", "name": "Agent hotkey", "kind": "agent",
          "params": {"agent": "claude_code", "mode": "handoff", "surface": "auto"},
          "hotkey": "ctrl+alt+shift+space", "shaping": "cleanup", "spoken_names": ["Claude Code"],
          "description": "Coding tasks for Claude Code", "enabled": True}


def _with(base, **changes):
    return {**base, **changes}


def _params(base, **changes):
    return {**base, "params": {**base["params"], **changes}}


CASES = {
    "unique_name": [
        ["Ask Claude", {}],
        ["Ask Claude", {"actions": []}],
        ["Ask Claude", {"actions": [_row("a", "Ask Claude")]}],
        ["Ask Claude", {"actions": [_row("a", "Ask Claude"), _row("b", "Ask Claude 2")]}],
        ["Ask Claude", {"actions": [_row("a", "ASK CLAUDE"), _row("b", "ask claude 2"),
                                    _row("c", "Ask Claude 4")]}],
        ["Webhook", {"actions": [_row("a", "Ask Claude")]}],
        ["Stra\u00dfe", {"actions": [_row("a", "STRASSE")]}],
        ["Copy only", {"actions": [_row("a", "Copy only"), _row("b", "Copy only 3")]}],
        ["Quick answer", {"actions": "oops"}],
        ["Quick answer", {"actions": [_row("a", "Quick answer", "nonsense")]}],
        ["Run a program", {"actions": [_row("a", "  Run a program  ")]}],
    ],
    "saved_note": [
        [_row("x", "Ask Claude", hotkey="ctrl+alt+c", spoken_names=["Claude"])],
        [_row("x", "Ask Claude", hotkey="ctrl+alt+shift+space")],
        [_row("x", "Home", spoken_names=["House", "Home"])],
        [_row("x", "Copy only")],
        [_row("x", "Numpad", hotkey="kp *")],
        [_row("x", "Mouse", hotkey="ctrl+mouse4")],
    ],
    "onto_saved": [
        # Nothing edited: the action as saved now.
        [_with(_LINK, enabled=False), _LINK, _LINK],
        # A name edited while the switch was turned off elsewhere: both kept.
        [_with(_LINK, enabled=False, hotkey="ctrl+alt+j"), _LINK, _with(_LINK, name="Claude chat")],
        # A parameter edited, another one changed elsewhere.
        [_params(_AGENT, mode="ask"), _AGENT, _params(_AGENT, surface="terminal")],
        # The same parameter changed in both: the edit wins.
        [_params(_AGENT, agent="codex"), _AGENT, _params(_AGENT, agent="opencode_cli")],
        # A parameter the baseline lacked is an edit even when equal to the saved one.
        [_params(_AGENT, model="haiku"), _AGENT, _params(_AGENT, model="haiku", folder="C:\\x")],
        # Spoken names, description, shaping and the shortcut edited at once.
        [_LINK, _LINK, _with(_LINK, spoken_names=["Claude", "ask Claude"], description="Questions",
                             shaping="raw", hotkey="")],
        # The draft drops a parameter the saved action still has: the saved one stays.
        [_params(_AGENT, output="notice"), _params(_AGENT, output="notice"), _AGENT],
        # Edited back to what was saved before the outside change: still an edit of nothing.
        [_with(_LINK, name="Elsewhere"), _LINK, _LINK],
    ],
}
