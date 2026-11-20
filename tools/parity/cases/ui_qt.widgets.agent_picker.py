"""Curated inputs for ui_qt/widgets/agent_picker.py (M5.18 agent-picker).

An agent is a dict of InstalledAgent fields ({"id", "path", "version", "account", "signed_in",
"problem"}), or None for an agent the scan did not find; a scan is {agent id: agent or None}.
A TileState comes back as [tone, pill, detail, selectable, installed].

    command_text(text), describe_agent(id, agent), found_count_text(scan), usage_caption(agent),
    blocked_notice(id, agent)
        the module's functions as they are.
    picker_count_text(scan, purpose), picker_usage_caption(agent, purpose),
    picker_blocked_notice(id, agent, purpose)
        what an AgentPicker for `purpose` shows: its count line after a cached scan, the usage
        caption of its model row and its blocked notice once the agent is chosen. The picker is
        built offscreen with the scan as its cached result and every blocking seam replaced, so
        no agent is looked for or started.
"""

import os

PURPOSES = ["AI insights", "AI cleanup"]

_APP = None


def _agent(spec):
    from services.installed_agents import InstalledAgent

    return None if spec is None else InstalledAgent(**spec)


def _scan(scan):
    return {agent_id: _agent(spec) for agent_id, spec in scan.items()}


def _state(state):
    return [state.tone, state.pill, state.detail, state.selectable, state.installed]


def _picker(module, scan, purpose):
    """An AgentPicker for `purpose` showing `scan`, with no worker able to reach an agent."""
    global _APP
    os.environ.setdefault("QT_QPA_PLATFORM", "offscreen")
    from PyQt6.QtWidgets import QApplication

    _APP = QApplication.instance() or QApplication([])

    def refuse(*_args, **_kwargs):
        raise RuntimeError("agents are not scanned while vectors are made")

    module.scan_installed_agents = refuse
    module.list_agent_models = lambda agent: []
    module.cached_agents = lambda: dict(scan)
    return module.AgentPicker(purpose=purpose)


def _picker_count_text(module, scan, purpose):
    picker = _picker(module, _scan(scan), purpose)
    return picker.count_label.text()


def _picker_usage_caption(module, spec, purpose):
    agent = _agent(spec)
    picker = _picker(module, {agent.id: agent}, purpose)
    picker.set_choice(agent.id)
    return picker.usage_label.text()


def _picker_blocked_notice(module, agent_id, spec, purpose):
    picker = _picker(module, {agent_id: _agent(spec)}, purpose)
    picker.set_choice(agent_id)
    return picker.notice_label.text()


WRAPPERS = {
    "describe_agent": lambda m, agent_id, spec: _state(m.describe_agent(agent_id, _agent(spec))),
    "found_count_text": lambda m, scan: m.found_count_text(_scan(scan)),
    "usage_caption": lambda m, spec: m.usage_caption(_agent(spec)),
    "blocked_notice": lambda m, agent_id, spec: m.blocked_notice(agent_id, _agent(spec)),
    "picker_count_text": _picker_count_text,
    "picker_usage_caption": _picker_usage_caption,
    "picker_blocked_notice": _picker_blocked_notice,
}


def _a(agent_id, version="2.1.0", account="", signed_in=None, problem=""):
    return {"id": agent_id, "path": f"C:/bin/{agent_id}.exe", "version": version,
            "account": account, "signed_in": signed_in, "problem": problem}


CLAUDE = _a("claude_code", "2.1.281", "Claude Team", True)
CLAUDE_MAX = _a("claude_code", "2.1.3", "Claude Max", True)
CLAUDE_KEY = _a("claude_code", "2.1.0", "API key", True)
CLAUDE_BEDROCK = _a("claude_code", "2.1.0", "Amazon Bedrock", True)
CLAUDE_NO_VERSION = _a("claude_code", "", "Claude Pro", True)
CLAUDE_SIGNED_OUT = _a("claude_code", "2.1.0", "", False)
CODEX = _a("codex", "0.158.0", "ChatGPT", True)
CODEX_SIGNED_OUT = _a("codex", "0.158.0", "", False)
CODEX_OLD = _a("codex", "0.100.0", problem=(
    "OpenWhisper needs Codex 0.120.0 or newer. Run `npm install -g @openai/codex` or update the "
    "Codex app."))
CODEX_UNKNOWN_ACCOUNT = _a("codex", "0.158.0", "Workspace", True)
OPENCODE = _a("opencode_cli", "2.1.0")
OPENCODE_OLD = _a("opencode_cli", "1.4.2", problem=(
    "OpenWhisper needs OpenCode 2.0.0 or newer. Run `opencode upgrade`."))
OPENCODE_SIGNED_OUT = _a("opencode_cli", "2.1.0", signed_in=False)

SCANS = [
    {},
    {"claude_code": CLAUDE, "codex": CODEX, "opencode_cli": None},
    {"claude_code": None, "codex": CODEX, "opencode_cli": None},
    {"claude_code": None, "codex": None, "opencode_cli": None},
    {"claude_code": CLAUDE, "codex": CODEX_OLD, "opencode_cli": OPENCODE},
    {"opencode_cli": OPENCODE_SIGNED_OUT},
]

CASES = {
    "command_text": [
        ["Run `claude update`."],
        [""],
        ["no commands here"],
        ["`a` and `b`"],
        ["``a`"],
        ["empty `` pair"],
        ["unclosed `quote"],
        ["Run `npm install -g @openai/codex` or update the Codex app."],
        ["`émoji ✓`"],
    ],
    "describe_agent": [
        ["claude_code", CLAUDE],
        ["claude_code", None],
        ["claude_code", CLAUDE_NO_VERSION],
        ["claude_code", CLAUDE_SIGNED_OUT],
        ["codex", CODEX_OLD],
        ["codex", CODEX_SIGNED_OUT],
        ["codex", _a("codex", "", "")],
        ["opencode_cli", OPENCODE],
        ["opencode_cli", OPENCODE_OLD],
        ["opencode_cli", OPENCODE_SIGNED_OUT],
        ["opencode_cli", None],
    ],
    "found_count_text": [[scan] for scan in SCANS],
    "usage_caption": [
        [CLAUDE], [CLAUDE_MAX], [CLAUDE_KEY], [CLAUDE_BEDROCK], [CODEX], [CODEX_UNKNOWN_ACCOUNT],
        [OPENCODE],
    ],
    "blocked_notice": [
        ["claude_code", None],
        ["claude_code", CLAUDE],
        ["codex", CODEX_OLD],
        ["codex", CODEX_SIGNED_OUT],
        ["opencode_cli", OPENCODE],
        ["opencode_cli", OPENCODE_OLD],
        ["opencode_cli", None],
    ],
    "picker_count_text": [[scan, purpose] for scan in SCANS for purpose in PURPOSES],
    "picker_usage_caption": [
        [agent, purpose]
        for agent in (CLAUDE, CLAUDE_MAX, CLAUDE_KEY, CLAUDE_BEDROCK, CODEX, OPENCODE)
        for purpose in PURPOSES
    ],
    "picker_blocked_notice": [
        [agent_id, agent, purpose]
        for agent_id, agent in (("claude_code", None), ("codex", CODEX_OLD),
                                ("codex", CODEX_SIGNED_OUT), ("opencode_cli", OPENCODE))
        for purpose in PURPOSES
    ],
}
