#pragma once

#include <optional>
#include <string>

#include "base/json.h"
#include "http/client.h"
#include "llm/credentials.h"

// services/meeting_agent_route.py: which agent runs a meeting's intelligence, and with what
// model. One answer for the live meeting, the archived dashboard, re-runs and custom reports. A
// meeting recorded with an installed agent (Claude Code, Codex, OpenCode) keeps it, as a meeting
// recorded on a text endpoint keeps that endpoint; otherwise the current settings decide.
// OpenWhisper's packaged Pi and OpenCode SDK payloads are gone (the built-in loop runs in
// process), and with them the route's payload directory.
// Affinity: thread-safe (functions over the snapshots they are given).
namespace fv::meeting_agent {

using Json = fv::Json;

struct MeetingAgentRoute {
    // A MeetingAgentCore value (agent_settings.h).
    std::string kind;
    // The text-endpoint profile id; for an installed agent, its id.
    std::string provider;
    // The model id; "" lets an installed agent use its own default.
    std::string model;
    // The text-endpoint snapshot (TextLLMSnapshot::to_dict), or null for an installed agent.
    Json endpoint;

    bool installed() const;
    bool operator==(const MeetingAgentRoute&) const = default;
};

// The route for a new meeting, or for the stored meeting row `meeting` (re-runs and reports on
// it). A missing or empty row is a new meeting. Throws what the endpoint snapshot throws.
MeetingAgentRoute resolve_meeting_agent_route(const Json& settings,
                                              const std::optional<Json>& meeting = std::nullopt);
// The same, first asking a text server the user runs about its models for a new meeting's
// endpoint snapshot, as OpenWhisper does. Blocks on the network: never on the GUI thread.
MeetingAgentRoute resolve_meeting_agent_route(const Json& settings,
                                              const std::optional<Json>& meeting,
                                              http::Client& client, llm::KeySources& keys);

} // namespace fv::meeting_agent
