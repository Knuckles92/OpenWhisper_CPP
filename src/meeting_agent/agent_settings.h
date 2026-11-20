#pragma once

#include <array>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "base/json.h"

// The meeting-agent resolvers of services/settings.py (MeetingAgentCore and the
// meeting_agent_core, meeting_agent_models, meeting_live_thinking, meeting_final_thinking and
// meeting_backup_agent keys). The stored values stay OpenWhisper's: "pi", "opencode" and the
// legacy "direct" all run the built-in agent loop; "claude_code", "codex" and "opencode_cli"
// the installed coding agents.
// Affinity: thread-safe (pure functions over a settings snapshot).
namespace fv::meeting_agent {

using Json = fv::Json;

// MeetingAgentCore's values.
inline constexpr std::string_view CORE_PI = "pi";
inline constexpr std::string_view CORE_DIRECT = "direct"; // legacy, read as an invalid choice
inline constexpr std::string_view CORE_CLAUDE_CODE = "claude_code";
inline constexpr std::string_view CORE_CODEX = "codex";
inline constexpr std::string_view CORE_OPENCODE = "opencode";
inline constexpr std::string_view CORE_OPENCODE_CLI = "opencode_cli";
inline constexpr std::array<std::string_view, 3> INSTALLED_CORES = {CORE_CLAUDE_CODE, CORE_CODEX,
                                                                    CORE_OPENCODE_CLI};

// True for the installed coding agents.
bool is_installed_core(std::string_view kind);

// The chosen core; an unset or unknown value (the retired "direct" among them) reads as "pi".
std::string resolve_meeting_agent_core(const Json& settings);
// {agent_id: model} for the installed agents only, models stripped, non-text models dropped, in
// the stored order; "" keeps the agent's own default.
std::vector<std::pair<std::string, std::string>> resolve_meeting_agent_models(const Json& settings);
// The model chosen for `agent_id`, or "" for its own default.
std::string resolve_meeting_agent_model(std::string_view agent_id, const Json& settings);
// An installed agent's thinking level for live passes ("low" by default) and for the final
// report ("medium").
std::string resolve_meeting_live_thinking(const Json& settings);
std::string resolve_meeting_final_thinking(const Json& settings);
// The installed agent that takes over when the chosen one fails, or "" for none.
std::string resolve_meeting_backup_agent(const Json& settings);

} // namespace fv::meeting_agent
