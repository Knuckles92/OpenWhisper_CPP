#pragma once

#include <array>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>

#include "base/json.h"

// What the live API and MCP may do for the user; checked on every request
// (services/automation/permissions.py). Each permission is a settings key.
namespace fv::automation::permissions {

enum class Permission { LiveEvents, RunActions, ControlRecording, TextFilters, AgentPrompts };

// The settings keys: api_live_events, api_run_actions, api_control_recording,
// api_text_filters, and api_agent_prompts (running an agent or Claude Code channel action for a
// program, which hands its words to the user's coding agent; needs RUN_ACTIONS as well).
inline constexpr std::string_view LIVE_EVENTS = "api_live_events";
inline constexpr std::string_view RUN_ACTIONS = "api_run_actions";
inline constexpr std::string_view CONTROL_RECORDING = "api_control_recording";
inline constexpr std::string_view TEXT_FILTERS = "api_text_filters";
inline constexpr std::string_view AGENT_PROMPTS = "api_agent_prompts";
inline constexpr std::array<std::string_view, 5> ALL = {LIVE_EVENTS, RUN_ACTIONS, CONTROL_RECORDING,
                                                        TEXT_FILTERS, AGENT_PROMPTS};

std::string_view key(Permission permission);
// The permission a settings key names, or nullopt for any other key.
std::optional<Permission> from_key(std::string_view key);
// The short name API and MCP responses use: live_events, run_actions, control_recording,
// text_filters, agent_prompts.
std::string_view public_name(Permission permission);

// A refusal: `message` tells the caller what to turn on, naming no internals.
class PermissionDenied : public std::runtime_error {
public:
    explicit PermissionDenied(std::string permission);
    // The settings key that was refused.
    const std::string& permission() const noexcept { return permission_; }
    std::string message() const { return what(); }

private:
    std::string permission_;
};

// True when the permission is on; a missing or malformed value means its default (live events
// on, the rest off). Any key that is not a permission is never granted.
bool granted(const Json& settings, std::string_view permission);
bool granted(const Json& settings, Permission permission);
// Throws PermissionDenied unless granted.
void require(const Json& settings, std::string_view permission);
void require(const Json& settings, Permission permission);
// {public name: granted} for every permission, in ALL order.
Json describe(const Json& settings);

} // namespace fv::automation::permissions
