#pragma once

#include <chrono>
#include <string_view>

#include "agents/agent_actions.h"

namespace fv::agents::detail {

// Starts a fresh, tool-denied ACP session for one transcript; provider sign-in remains in
// OpenCode's data store. All failures returned to the caller use fixed messages.
AskResult ask_opencode_cleanup(InstalledAgents& agents, const InstalledAgent& agent,
                               std::string_view text, std::string_view system_prompt,
                               std::string_view model, std::string_view effort,
                               std::chrono::steady_clock::time_point deadline,
                               const CancelToken& cancel);

} // namespace fv::agents::detail
