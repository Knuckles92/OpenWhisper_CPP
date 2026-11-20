#pragma once

#include "base/json.h"

// meeting/agent/tool_specs.py: the meeting tools every agent core offers, as JSON schemas in
// chat-completions function-tool shape, and the same tools in MCP tools/list shape for the
// installed coding agents' loopback server. The tool names stay OpenWhisper's (patch_state,
// ask_question, resolve_question, search_past_meetings, search_context_files); descriptions
// name the product through identity(). tool_policy.h answers every call.
// Affinity: thread-safe (immutable data, built once).
namespace fv::meeting_agent {

using Json = fv::Json;

// MEETING_TOOLS: the five tools, in Python's order, each {"type": "function", "function":
// {"name", "description", "parameters"}}.
const Json& meeting_tools();
// NOOP_TOOL: the capability probe's tool.
const Json& noop_tool();
// [{"name", "description", "inputSchema"}] for the five tools.
Json mcp_tool_definitions();

} // namespace fv::meeting_agent
