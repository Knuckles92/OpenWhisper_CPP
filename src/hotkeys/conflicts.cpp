#include "hotkeys/conflicts.h"

#include <stdexcept>

#include "hotkeys/defaults.h"
#include "settings/keys.h"

namespace fv::hotkeys {

namespace {

ParsedHotkey signature(std::string_view hotkey, Vocabulary vocabulary) {
    return parse_hotkey(normalize_hotkey(hotkey, vocabulary), vocabulary);
}

// Python's truth value of a JSON value.
bool truthy(const Json& value) {
    if (value.is_null()) {
        return false;
    }
    if (value.is_boolean()) {
        return value.get<bool>();
    }
    if (value.is_number()) {
        return value.get<double>() != 0;
    }
    // nlohmann's empty() is false for every string, so "" needs its own test.
    if (value.is_string()) {
        return !value.get_ref<const std::string&>().empty();
    }
    if (value.is_array() || value.is_object()) {
        return !value.empty();
    }
    return true;
}

std::string label_of(const std::string& action) {
    for (const auto& [key, label] : standard_labels()) {
        if (key == action) {
            return std::string(label);
        }
    }
    std::string label = action;
    for (char& c : label) {
        if (c == '_') {
            c = ' ';
        }
    }
    return label;
}

std::optional<std::string> standard_conflict(const ParsedHotkey& wanted, const Json& settings,
                                             const ConflictOptions& options) {
    Json saved;
    if (options.standard_hotkeys) {
        saved = *options.standard_hotkeys;
    } else {
        saved = keys::hotkeys.read(settings);
        if (!truthy(saved)) {
            saved = Json::object();
        }
    }
    // {**config.DEFAULT_HOTKEYS, **saved}: the defaults' order, saved values over them, then
    // the actions only the saved map has.
    Json standard = Json::object();
    for (const auto& [action, hotkey] : default_hotkeys(options.platform)) {
        standard[action] = hotkey;
    }
    if (saved.is_object()) {
        for (const auto& [action, value] : saved.items()) {
            standard[action] = value;
        }
    }
    for (const auto& [action, value] : standard.items()) {
        if (options.exclude.first == kStandard && options.exclude.second == action) {
            continue;
        }
        if (!truthy(value)) {
            continue;
        }
        if (!value.is_string()) {
            throw std::invalid_argument("A standard shortcut is not text.");
        }
        if (signature(value.get<std::string>(), options.vocabulary) == wanted) {
            return label_of(action);
        }
    }
    return std::nullopt;
}

std::optional<std::string> binding_conflict(const std::vector<Binding>& bindings,
                                            std::string_view kind, const ParsedHotkey& wanted,
                                            const ConflictOptions& options, bool normalized) {
    for (const auto& binding : bindings) {
        if (options.exclude.first == kind && options.exclude.second == binding.id) {
            continue;
        }
        if (binding.hotkey.empty()) {
            continue;
        }
        const ParsedHotkey theirs = normalized ? signature(binding.hotkey, options.vocabulary)
                                               : parse_hotkey(binding.hotkey, options.vocabulary);
        if (theirs == wanted) {
            return binding.name;
        }
    }
    return std::nullopt;
}

} // namespace

const std::vector<std::pair<std::string_view, std::string_view>>& standard_labels() {
    static const std::vector<std::pair<std::string_view, std::string_view>> labels{
        {"record_toggle", "Recording"},
        {"cancel", "Cancel"},
        {"meeting_toggle", "Meeting Mode"},
        {"enable_disable", "Enable/disable"},
        {"minimize_tray", "Minimize to tray"},
        {"command_mode", "Command Mode"},
        {"scratchpad_toggle", "Scratchpad"},
        {"cycle_language", "Switch language"},
        {"paste_last_original", "Paste original"},
        {"route_record", "Route by voice"},
        {"walkthrough", "Walkthrough"},
        {"screenshot", "Screenshot"},
        {"paste_walkthrough", "Paste latest walkthrough"},
    };
    return labels;
}

std::string normalize_hotkey(std::string_view hotkey, Vocabulary vocabulary) {
    std::string spaced(hotkey);
    for (std::size_t at = spaced.find("kp_"); at != std::string::npos;
         at = spaced.find("kp_", at + 3)) {
        spaced[at + 2] = ' ';
    }
    const ParsedHotkey parsed = parse_hotkey(spaced, vocabulary);
    return format_hotkey(parsed.modifiers, parsed.main_key, vocabulary);
}

std::string hotkey_conflict(std::string_view hotkey, const Json& settings,
                            const ConflictOptions& options) {
    if (hotkey.empty()) {
        return "";
    }
    const ParsedHotkey wanted = signature(hotkey, options.vocabulary);
    if (auto conflict = standard_conflict(wanted, settings, options)) {
        return *conflict;
    }
    // Profiles were normalized when they were loaded, so they compare as parsed.
    if (auto conflict =
            binding_conflict(options.bindings.profiles, kProfile, wanted, options, false)) {
        return *conflict;
    }
    if (auto conflict =
            binding_conflict(options.bindings.transforms, kTransform, wanted, options, true)) {
        return *conflict;
    }
    if (auto conflict =
            binding_conflict(options.bindings.actions, kAction, wanted, options, true)) {
        return *conflict;
    }
    return "";
}

} // namespace fv::hotkeys
