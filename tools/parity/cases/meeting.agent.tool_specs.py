"""The data of meeting/agent/tool_specs.py (M8.2 agent-prompts-and-policy).

    MEETING_TOOLS(), NOOP_TOOL()   the module's constants
    mcp_tool_definitions()
The descriptions name the product; the C++ replay compares them rebranded.
"""

CASES = {
    "MEETING_TOOLS": [[]],
    "NOOP_TOOL": [[]],
    "mcp_tool_definitions": [[]],
}

WRAPPERS = {
    "MEETING_TOOLS": lambda module: module.MEETING_TOOLS,
    "NOOP_TOOL": lambda module: module.NOOP_TOOL,
}
