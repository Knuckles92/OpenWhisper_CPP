#pragma once

#include "base/json.h"

// meeting/agent/base.py merge_usage: a pass's token usage summed over its model responses.
// Affinity: thread-compatible (the caller owns `total`).
namespace fv::meeting_agent {

using Json = fv::Json;

// Adds one response's usage to `total`: prompt_tokens, completion_tokens and total_tokens when
// they are integers (a bool counts as 0 or 1, as Python's isinstance(x, int) has it), and one
// to "requests"; nothing when `usage` is null. Python reads the response's attributes; here
// they are the members of a JSON object, and a non-object adds only the request.
void merge_usage(Json& total, const Json& usage);

} // namespace fv::meeting_agent
