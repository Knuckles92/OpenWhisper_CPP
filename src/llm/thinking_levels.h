#pragma once

#include <array>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "base/json.h"

// services/thinking_levels.py: which thinking levels a cleanup runner and model accept, and the
// one that runs. One saved level (transcript_cleanup_reasoning) is shared by every runner;
// Settings offers levels_for, the runtime sends effective_level. Thread-safe.
namespace fv::llm {

struct TextLLMProfile;

// TranscriptCleanupReasoning.ALL, the order every level list follows.
inline constexpr std::array<std::string_view, 8> kThinkingOrder{"off",  "minimal", "low", "medium",
                                                                "high", "xhigh",   "max", "ultra"};

// LABELS: the text Settings shows for each level, in kThinkingOrder.
inline constexpr std::array<std::pair<std::string_view, std::string_view>, 8> kThinkingLabels{{
    {"off", "Off"},
    {"minimal", "Minimal"},
    {"low", "Low"},
    {"medium", "Medium"},
    {"high", "High"},
    {"xhigh", "Extra high"},
    {"max", "Max"},
    {"ultra", "Ultra"},
}};
// The label of a level, or nullopt for an unknown one.
std::optional<std::string_view> thinking_label(std::string_view level);

// Codex's per-model reasoning efforts (installed_agents.codex_model_efforts), model -> levels.
using CodexEfforts = std::function<std::map<std::string, std::vector<std::string>>()>;
// The model an installed agent runs when none is chosen (configured_default_model).
using AgentDefaultModel = std::function<std::string(std::string_view agent_id)>;
// Installs both process-wide; until then Codex knows no models (every model gets the fallback
// low, medium, high). Empty functions restore that.
void set_codex_efforts(CodexEfforts efforts, AgentDefaultModel default_model);

// Levels an installed agent takes for `model`: Claude Code's CLI levels, a Codex model's own
// (an unknown or unnamed one gets what every listed model takes), else low, medium, high.
std::vector<std::string> agent_levels(std::string_view agent_id, std::string_view model);
// Levels a text endpoint's model takes; empty when it has no reasoning control (or no profile).
std::vector<std::string> endpoint_levels(const TextLLMProfile* profile, std::string_view model);
// Levels the cleanup runner `provider` (an installed agent or an endpoint id) offers.
std::vector<std::string> levels_for(std::string_view provider, std::string_view model,
                                    const Json& settings);
// `level` when offered, else the closest offered one (the lower on a tie); nullopt when nothing
// is offered. An unknown level counts as "off".
std::optional<std::string> nearest(std::string_view level, const std::vector<std::string>& offered);
// What to send for a saved level: the nearest offered, or "off" when none is.
std::string effective_level(std::string_view level, const std::vector<std::string>& offered);

} // namespace fv::llm
