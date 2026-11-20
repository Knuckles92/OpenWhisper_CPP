#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "base/json.h"
#include "base/platform_info.h"
#include "hotkeys/grammar.h"

// services/hotkey_conflicts.py: one answer to "is this shortcut taken?" across standard actions,
// cleanup profiles, transforms and saved actions, which share the global shortcut space.
namespace fv::hotkeys {

// Binding kinds, as `exclude` names them.
inline constexpr std::string_view kStandard = "standard";
inline constexpr std::string_view kProfile = "profile";
inline constexpr std::string_view kTransform = "transform";
inline constexpr std::string_view kAction = "action";

// STANDARD_LABELS: how messages name each standard shortcut's action.
const std::vector<std::pair<std::string_view, std::string_view>>& standard_labels();

// One profile, transform or saved action with a shortcut, as its module loads it.
struct Binding {
    std::string id;
    std::string name;
    std::string hotkey;
};

// The bindings each module loads from the settings (load_cleanup_profiles, load_transforms,
// load_actions), in their order. Profile shortcuts are already normalized by their loader.
struct Bindings {
    std::vector<Binding> profiles;
    std::vector<Binding> transforms;
    std::vector<Binding> actions;
};

struct ConflictOptions {
    // (kind, id) of the binding being edited, so it does not conflict with itself; kind is
    // kStandard (id is the action), kProfile, kTransform or kAction.
    std::pair<std::string, std::string> exclude{"", ""};
    // Standard shortcuts not saved yet (an object of action -> hotkey); they replace the saved
    // ones.
    std::optional<Json> standard_hotkeys;
    Bindings bindings;
    Vocabulary vocabulary = platform_vocabulary();
    std::string_view platform = python_platform(); // whose DEFAULT_HOTKEYS
};

// cleanup_profiles.normalize_hotkey: "kp_" spelled "kp ", modifiers in canonical order.
std::string normalize_hotkey(std::string_view hotkey,
                             Vocabulary vocabulary = platform_vocabulary());

// What already uses `hotkey` (modifier aliases and order do not matter), or "" when it is free:
// the standard shortcut's label, else the first profile's, transform's or action's name. The
// standard shortcuts are the defaults with the saved `hotkeys` (or `standard_hotkeys`) over
// them, so a default never saved still conflicts; an empty shortcut never conflicts. Throws
// std::invalid_argument for a standard value that is set but not a string, where Python fails.
std::string hotkey_conflict(std::string_view hotkey, const Json& settings,
                            const ConflictOptions& options = {});

} // namespace fv::hotkeys
