#pragma once

#include <string>
#include <string_view>
#include <vector>

#include "base/json.h"

// The loader of services/actions/model.py (30-157, 205-290): saved actions as the app reads
// them, for the backup sanitizer. Private until the action model itself is ported, which then
// owns this loading.
namespace fv::backup::detail {

// ActionKind values.
inline constexpr std::string_view kOpenLink = "open_link";
inline constexpr std::string_view kAgent = "agent";
inline constexpr std::string_view kChannel = "channel";
inline constexpr std::string_view kWebhook = "webhook";
inline constexpr std::string_view kProgram = "program";
inline constexpr std::string_view kClipboard = "clipboard";
inline constexpr std::string_view kScripts = "scripts";

inline constexpr std::size_t kMaxActions = 50;
inline constexpr std::size_t kDescriptionMaxChars = 300;
inline constexpr std::size_t kMaxSpokenNames = 8;
inline constexpr std::size_t kSpokenNameMaxChars = 40;

// One loaded Action.
struct ActionRow {
    std::string id;
    std::string name;
    std::string kind;
    Json params = Json::object(); // the kind's PARAMS keys, in their order, coerced
    std::string hotkey;
    std::string shaping;
    std::vector<std::string> spoken_names;
    std::string description;
    bool enabled = true;

    // Action.to_dict().
    Json to_json() const;
};

// load_actions({"actions": actions}): malformed rows, unknown kinds, repeated ids and rows past
// the 50th are skipped; kinds and text are stripped, parameters coerced to their defaults'
// types, shortcuts normalized (an unreadable one reads as none).
std::vector<ActionRow> load_action_rows(const Json& actions);

} // namespace fv::backup::detail
