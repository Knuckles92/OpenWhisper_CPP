#pragma once

#include <optional>
#include <string>
#include <string_view>

#include "agents/installed_agents.h"

// What the agent picker says about each agent, and its timings (ui_qt/widgets/agent_picker.py
// :79-249). Qt-free, so the copy is golden-tested against OpenWhisper's.
namespace fv::agent_picker {

// Tile tones: what the scan says about one tile.
inline constexpr std::string_view kIdle = "idle";
inline constexpr std::string_view kScanning = "scanning";
inline constexpr std::string_view kFound = "found";
inline constexpr std::string_view kMissing = "missing";
inline constexpr std::string_view kWarning = "warning";
inline constexpr std::string_view kBuiltinTone = "builtin";

// Gap between one tile lighting up and the next, ms.
inline constexpr int kStaggerMs = 100;
// One tile's arrival (mark warms, pill appears, check draws), ms.
inline constexpr int kArrivalMs = 380;
inline constexpr int kShimmerFadeMs = 160;
inline constexpr int kHoverMs = 140;
inline constexpr int kLiftMs = 220;
inline constexpr double kShimmerPeriodMs = 1300.0;
// Where the band starts, so a scan shorter than one sweep still shows it (fraction of a period).
inline constexpr double kShimmerStart = 0.3;
// Each tile's shimmer trails its left neighbour's by this much, so the row ripples, ms.
inline constexpr double kShimmerOffsetMs = 140.0;
inline constexpr int kTickMs = 16;
// More model choices than this get a type-to-filter combo.
inline constexpr int kFilterThreshold = 20;

// The purposes the hosts pass; "AI insights" is the default and the word the copy is written in.
inline constexpr std::string_view kInsightsPurpose = "AI insights";
inline constexpr std::string_view kCleanupPurpose = "AI cleanup";

inline constexpr std::string_view kBuiltinDetail = "Your API key · the insights model below";
inline constexpr std::string_view kCheckingDetail = "Checking…";

// What one tile shows for a scan result.
struct TileState {
    std::string tone;   // one of the tile tones
    std::string pill;   // status pill text
    std::string detail; // line under the name ("2.1.281 · Claude Team", a problem)
    bool selectable = false;
    bool installed = false; // the agent is on this computer, so its mark is in colour
    bool operator==(const TileState&) const = default;
};

TileState idle_state();
// The built-in tile for AI insights: "Built in", the insights detail.
TileState builtin_state();
// The built-in tile for AI cleanup: "Built in", "The cleanup model below".
TileState cleanup_builtin_state();

// "Run `claude update`." with each `command` in typographic quotes instead of backticks.
std::string command_text(std::string_view text);
// The tile for agent_id given what the scan found (nullopt: not installed).
TileState describe_agent(std::string_view agent_id,
                         const std::optional<agents::InstalledAgent>& agent);
// The line under the heading once a scan is in.
std::string found_count_text(const agents::ScanResult& agents);
// What running meeting passes through agent means, in three sentences.
std::string usage_caption(const agents::InstalledAgent& agent);
// Why the chosen agent cannot run AI insights, or "" when it can.
std::string blocked_notice(std::string_view agent_id,
                           const std::optional<agents::InstalledAgent>& agent);

// The picker's own wording for a purpose (AgentPicker._apply_results, _sync_selection,
// _show_model_card): the count line, the blocked notice with the purpose in place of "AI
// insights", and the usage caption, which for AI cleanup speaks of cleanup requests.
std::string purpose_count_text(const agents::ScanResult& agents, std::string_view purpose);
std::string purpose_blocked_notice(std::string_view agent_id,
                                   const std::optional<agents::InstalledAgent>& agent,
                                   std::string_view purpose);
std::string purpose_usage_caption(const agents::InstalledAgent& agent, std::string_view purpose);

} // namespace fv::agent_picker
