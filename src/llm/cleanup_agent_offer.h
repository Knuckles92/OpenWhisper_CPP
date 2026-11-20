#pragma once

#include <array>
#include <string_view>
#include <vector>

#include "agents/installed_agents.h"
#include "base/json.h"

// Offer a detected Claude Code or Codex as the AI cleanup runner
// (services/cleanup_agent_offer.py). Cleanup starts on the built-in OpenRouter default, which
// needs an API key; a coding agent the user is already signed in to needs none, so the first time
// cleanup is on and no runner was ever chosen, the UI asks once whether to use the agent instead.
// Nothing here switches without that answer.
//
// OpenWhisper_CPP differs in one respect: the person dictating approves a named CLI before speech
// reaches it (fv_cleanup_agent_consent). accept() records that consent only when the caller says
// the person approved it; the offer's UI asks for that approval before it accepts.
namespace fv {
class SettingsStore;
}

namespace fv::llm::cleanup_agent_offer {

// The agents offered, preferred first.
inline constexpr std::array<std::string_view, 2> OFFERED = {"claude_code", "codex"};

// True while cleanup is on, its runner is the untouched default (none stored) and the offer has
// not been answered.
bool pending(const Json& settings);

// The offered agents `scan` found installed and signed in, in OFFERED's order.
std::vector<agents::InstalledAgent> offerable(const agents::ScanResult* scan);

// Runs cleanup through `agent_id` with the model last used for it, or the agent's own default
// (""), and marks the offer answered, in one settings mutation. `consented`: the person approved
// sending dictation through that CLI, which is recorded as fv_cleanup_agent_consent; without it
// the runner is selected but not authorized. Throws std::invalid_argument for an agent that is
// not offered.
void accept(std::string_view agent_id, SettingsStore& settings, bool consented);

// Keeps the current runner and never asks again.
void decline(SettingsStore& settings);

} // namespace fv::llm::cleanup_agent_offer
