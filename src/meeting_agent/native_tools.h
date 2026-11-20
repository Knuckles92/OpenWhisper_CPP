#pragma once
#include "meeting_agent/tool_policy.h"

// The built-in loop's tools (sidecar/src/tools.ts, what OpenWhisper's Pi and OpenCode sidecars
// offered their model) as chat-completions function tools. Their names, descriptions and
// schemas are prompt data and stay tools.ts's word for word, rebranded; the installed agents
// keep tool_specs.py's definitions (tool_specs.h), as OpenWhisper's did over MCP.
// Affinity: thread-safe (immutable data, built once).
namespace fv::meeting_agent {

// patch_state, ask_question, resolve_question, search_past_meetings, search_context_files, in
// tools.ts's order, each {"type": "function", "function": {"name", "description",
// "parameters"}}. The parameters are what Pi's toTypebox compiled tools.ts's specs to: every
// object has its additionalProperties (false unless the spec says true) and a `required` list
// only when it names a property.
const Json& native_tools();
// The capability probe's tool: sidecar.py's "noop", described "Verify tool support."
const Json& native_probe_tool();

std::string native_tool_result(std::string_view name, const Json& result,
                               const std::vector<meeting_state::OpResult>& ops);

} // namespace fv::meeting_agent
