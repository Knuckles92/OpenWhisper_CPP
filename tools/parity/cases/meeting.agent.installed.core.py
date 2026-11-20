"""Curated inputs for meeting/agent/installed/core.py (M6.2): what installed_agent says.

    installed_agent(agent_id, found)    with resolve_agent giving `found` (InstalledAgent
                                        keyword arguments, or None): {"agent": [id, path,
                                        version]} for a runnable agent, else
                                        {"unavailable": message}
    constants()                         MEETING_AGENT_SETTING, FINAL_EFFORT and
                                        CONSOLIDATION_STALL_S
"""


def _installed_agent(core, agent_id, found):
    from services.installed_agents import InstalledAgent

    real = core.resolve_agent
    core.resolve_agent = lambda _agent_id: None if found is None else InstalledAgent(**found)
    try:
        agent = core.installed_agent(agent_id)
        return {"agent": [agent.id, agent.path, agent.version]}
    except core.AgentUnavailable as exc:
        return {"unavailable": str(exc)}
    finally:
        core.resolve_agent = real


def _constants(core):
    return {"MEETING_AGENT_SETTING": core.MEETING_AGENT_SETTING,
            "FINAL_EFFORT": core.FINAL_EFFORT,
            "CONSOLIDATION_STALL_S": core.CONSOLIDATION_STALL_S}


WRAPPERS = {"installed_agent": _installed_agent, "constants": _constants}

CASES = {
    "installed_agent": [
        ["claude_code", None],
        ["codex", None],
        ["opencode_cli", None],
        ["pi", None],
        ["claude_code", {"id": "claude_code", "path": "c", "version": "1.0",
                         "problem": "OpenWhisper needs Claude Code 2.0.0"}],
        ["claude_code", {"id": "claude_code", "path": "c", "version": "2.1", "signed_in": False}],
        ["codex", {"id": "codex", "path": "x", "version": "0.163.0", "signed_in": False}],
        ["opencode_cli", {"id": "opencode_cli", "path": "o", "version": "2.0.18",
                          "signed_in": False}],
        ["claude_code", {"id": "claude_code", "path": "claude", "version": "2.1.281",
                         "account": "Claude Team", "signed_in": True}],
        ["opencode_cli", {"id": "opencode_cli", "path": "o", "version": "2.0.18"}],
        ["codex", {"id": "codex", "path": "x", "version": "0.1", "problem": "Too old",
                   "signed_in": False}],
    ],
    "constants": [[]],
}
