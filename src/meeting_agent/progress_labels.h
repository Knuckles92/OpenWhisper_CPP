#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <utility>

// meeting/agent/sidecar.py's progress and activity tables: the sentences the finalization card
// and the activity strip show for an agent's session events (Pi's AgentSessionEvent names,
// which the built-in loop keeps), per pass kind, and the activity category of each event.
// Installed coding agents report their own event kinds, mapped to the same events through
// pi_event (installed/core.py _PI_EVENTS).
// Affinity: thread-safe (pure functions).
namespace fv::meeting_agent {

// The sentence for one event, or for its assistant-message delta (the `delta` of a
// message_update), during a pass of `pass_kind` (tool_policy.h's PASS_*, or "" when unknown):
// the pass's own wording first, then the default wording, then the pass's fallback.
std::string progress_detail(std::string_view event, std::string_view delta = "",
                            std::string_view pass_kind = "");

// One of interfaces.h's ACTIVITY_KINDS for an event and its delta; "update" when unclassified.
std::string activity_kind(std::string_view event, std::string_view delta = "");

// The (event, delta) an installed agent driver's event kind (agents/drivers.h EVENT_KINDS)
// stands for; nullopt for a kind outside the table.
std::optional<std::pair<std::string_view, std::string_view>> pi_event(std::string_view kind);

} // namespace fv::meeting_agent
