#pragma once

#include <string>

#include "base/json.h"

// meeting/corrections.py guidance_prompt: the block that leads every agent prompt with the
// humans' corrections (term and one-occurrence corrections and free clarifications).
// Affinity: thread-safe (pure function).
namespace fv::meeting_agent {

using Json = fv::Json;

// "## HUMAN MEETING GUIDANCE" with its rules and a JSON list of {text, data, evidence} for every
// live, correction-authored user note whose data.kind is agent_insight, term_correction or
// occurrence_correction; "" when there is none. `state` is a MeetingState::to_json() snapshot.
std::string guidance_prompt(const Json& state);

} // namespace fv::meeting_agent
