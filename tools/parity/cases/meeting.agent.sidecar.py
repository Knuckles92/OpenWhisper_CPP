"""Curated inputs for the label tables of meeting/agent/sidecar.py (M8.2 agent-prompts-and-policy).

    _progress_detail(event, delta, pass_kind)
    _activity_kind(event, delta)
    AgentActivity(kind, label, tool, pass_kind, ts)   .to_dict()
Every event and delta of the tables, the pass kinds, and names outside them.
"""

_EVENTS = [
    "thinking_start", "thinking_delta", "thinking_end", "text_delta", "toolcall_start",
    "toolcall_delta", "tool_execution_start", "tool_execution_update", "tool_execution_end",
    "turn_start", "turn_end", "agent_start", "agent_end", "agent_settled", "auto_retry_start",
    "auto_retry_end", "compaction_start", "compaction_end", "auto_compaction_start",
    "auto_compaction_end", "message_update", "message_start", "my_tool_thing", "TOOL_upper",
    "unknown_event", "",
]
_DELTAS = [
    "thinking_start", "thinking_delta", "thinking_end", "text_delta", "toolcall_start",
    "toolcall_delta", "toolcall_end", "tool_execution_start", "agent_start", "agent_end",
    "unknown_delta",
]
_PASSES = ["cards", "notes", "polish", "consolidation", "", "other"]

CASES = {
    "_progress_detail": (
        [[event, "", kind] for event in _EVENTS for kind in _PASSES]
        + [["message_update", delta, kind] for delta in _DELTAS for kind in _PASSES]
        + [["tool_execution_start", "text_delta", kind] for kind in _PASSES]
        + [["agent_start", "unknown_delta", kind] for kind in _PASSES]
    ),
    "_activity_kind": (
        [[event, ""] for event in _EVENTS]
        + [["message_update", delta] for delta in _DELTAS]
        + [["turn_start", "text_delta"], ["agent_end", "unknown_delta"],
           ["my_tool_thing", "unknown_delta"], ["unknown_event", "toolcall_end"]]
    ),
    "AgentActivity": [
        ["tool", "Updating the dashboard…", "patch_state", "cards", "2026-11-17T09:30:00+00:00"],
        ["update", "", "", "", ""],
    ],
}

WRAPPERS = {
    "AgentActivity": lambda module, *fields: module.AgentActivity(*fields).to_dict(),
}
