#include "text/cleanup_profiles.h"

#include <algorithm>
#include <cstdint>
#include <set>

#include "hotkeys/defaults.h"
#include "pyport/errors.h"
#include "pyport/text.h"
#include "settings/keys.h"
#include "text/cleanup_config.h"
#include "text/cleanup_prompts.h"
#include "text/detail/prompt_data.h"
#include "text/transforms.h"

namespace fv::text {

namespace {

// A profile name's longest length, in code points.
constexpr std::size_t kNameMaxChars = 80;

// Python's truth value of a JSON value.
bool truthy(const Json& value) {
    if (value.is_null()) {
        return false;
    }
    if (value.is_boolean()) {
        return value.get<bool>();
    }
    if (value.is_number_integer()) {
        return value.get<std::int64_t>() != 0;
    }
    if (value.is_number()) {
        return value.get<double>() != 0;
    }
    if (value.is_string()) {
        return !value.get_ref<const std::string&>().empty();
    }
    if (value.is_array() || value.is_object()) {
        return !value.empty();
    }
    return true;
}

// The text of a field when it is a non-blank string, stripped.
std::optional<std::string> required_text(const Json& item, std::string_view key) {
    const auto found = item.find(std::string(key));
    if (found == item.end() || !found->is_string()) {
        return std::nullopt;
    }
    std::string value = py::strip(found->get_ref<const std::string&>());
    if (value.empty()) {
        return std::nullopt;
    }
    return value;
}

hotkeys::ConflictOptions conflict_options(const Json& settings, std::string_view kind,
                                          std::string_view exclude_id,
                                          const ShortcutContext& context) {
    hotkeys::ConflictOptions options;
    options.exclude = {std::string(kind), std::string(exclude_id)};
    // hotkeys::hotkey_conflict counts a standard shortcut that is "" (unset, or a default with no
    // shortcut) as set, so a shortcut that parses to nothing ("   ") would collide with it. Python
    // skips falsy values, and so does hotkey_conflict for null: each "" goes in as null. The
    // order of the actions is unchanged, so the first conflict named is Python's.
    Json standard = context.standard_hotkeys ? *context.standard_hotkeys
                                             : keys::hotkeys.read(detail::or_empty(settings));
    if (!standard.is_object()) {
        standard = Json::object();
    }
    for (const auto& [action, hotkey] : hotkeys::default_hotkeys(context.platform)) {
        if (hotkey.empty() && !standard.contains(action)) {
            standard[action] = nullptr;
        }
    }
    for (auto& [action, value] : standard.items()) {
        if (value.is_string() && value.get_ref<const std::string&>().empty()) {
            value = nullptr;
        }
    }
    options.standard_hotkeys = std::move(standard);
    options.bindings = hotkey_bindings(settings, context);
    options.vocabulary = context.vocabulary;
    options.platform = context.platform;
    return options;
}

} // namespace

Json profile_to_json(const CleanupProfile& profile) {
    Json out = Json::object();
    out["id"] = profile.id;
    out["name"] = profile.name;
    out["instructions"] = profile.instructions;
    out["hotkey"] = profile.hotkey;
    out["use_learned_rules"] = profile.use_learned_rules;
    return out;
}

const std::vector<CleanupProfile>& starter_profiles() {
    static const std::vector<CleanupProfile> starters = [] {
        std::vector<CleanupProfile> out;
        for (const Json& row : detail::prompt_data().at("starter_profiles")) {
            out.push_back({row.at("id").get<std::string>(), row.at("name").get<std::string>(),
                           row.at("instructions").get<std::string>(),
                           row.at("hotkey").get<std::string>(),
                           row.at("use_learned_rules").get<bool>()});
        }
        return out;
    }();
    return starters;
}

std::string normalize_hotkey(std::string_view hotkey, hotkeys::Vocabulary vocabulary) {
    return hotkeys::normalize_hotkey(hotkey, vocabulary);
}

std::vector<CleanupProfile> load_cleanup_profiles(const Json& settings,
                                                  hotkeys::Vocabulary vocabulary) {
    const Json raw = keys::transcript_cleanup_profiles.read(detail::or_empty(settings));
    if (raw.is_null()) {
        return starter_profiles();
    }
    std::vector<CleanupProfile> profiles;
    if (!raw.is_array()) {
        return profiles;
    }
    std::set<std::string> seen;
    for (const Json& item : raw) {
        if (!item.is_object()) {
            continue;
        }
        auto id = required_text(item, "id");
        auto name = required_text(item, "name");
        auto instructions = required_text(item, "instructions");
        if (!id || !name || !instructions) {
            continue;
        }
        if (!seen.insert(*id).second) {
            continue;
        }
        CleanupProfile profile{std::move(*id), std::move(*name), std::move(*instructions), "",
                               true};
        const auto hotkey = item.find("hotkey");
        if (hotkey == item.end()) {
            profile.hotkey = hotkeys::normalize_hotkey("", vocabulary);
        } else if (hotkey->is_string()) {
            profile.hotkey =
                hotkeys::normalize_hotkey(hotkey->get_ref<const std::string&>(), vocabulary);
        }
        // `is not False`: only a stored false turns the shared rules off.
        const auto rules = item.find("use_learned_rules");
        profile.use_learned_rules =
            rules == item.end() || !rules->is_boolean() || rules->get<bool>();
        profiles.push_back(std::move(profile));
    }
    return profiles;
}

std::optional<CleanupProfile> find_cleanup_profile(const Json& settings,
                                                   std::string_view profile_id,
                                                   hotkeys::Vocabulary vocabulary) {
    for (CleanupProfile& profile : load_cleanup_profiles(settings, vocabulary)) {
        if (profile.id == profile_id) {
            return std::move(profile);
        }
    }
    return std::nullopt;
}

bool cleanup_may_run(const Json& settings) {
    const Json& values = detail::or_empty(settings);
    if (keys::transcript_cleanup_enabled.read(values)) {
        return true;
    }
    if (!keys::quick_record_profile.read(values).empty()) {
        return true;
    }
    for (const CleanupProfile& profile : load_cleanup_profiles(values)) {
        if (!profile.hotkey.empty()) {
            return true;
        }
    }
    const Json saved = keys::hotkeys.read(values);
    if (saved.is_object()) {
        const auto command = saved.find("command_mode");
        if (command != saved.end() && truthy(*command)) {
            return true;
        }
    }
    const auto transforms = load_transforms(values);
    return std::any_of(transforms.begin(), transforms.end(),
                       [](const Transform& transform) { return !transform.hotkey.empty(); });
}

hotkeys::Bindings hotkey_bindings(const Json& settings, const ShortcutContext& context) {
    hotkeys::Bindings bindings;
    for (const CleanupProfile& profile : load_cleanup_profiles(settings, context.vocabulary)) {
        bindings.profiles.push_back({profile.id, profile.name, profile.hotkey});
    }
    for (const Transform& transform : load_transforms(settings, context.vocabulary)) {
        bindings.transforms.push_back({transform.id, transform.name, transform.hotkey});
    }
    bindings.actions = context.actions;
    return bindings;
}

std::string profile_hotkey_conflict(std::string_view hotkey, const Json& settings,
                                    std::string_view exclude_id, const ShortcutContext& context) {
    return hotkeys::hotkey_conflict(
        hotkey, settings, conflict_options(settings, hotkeys::kProfile, exclude_id, context));
}

std::string transform_hotkey_conflict(std::string_view hotkey, const Json& settings,
                                      std::string_view exclude_id, const ShortcutContext& context) {
    return hotkeys::hotkey_conflict(
        hotkey, settings, conflict_options(settings, hotkeys::kTransform, exclude_id, context));
}

void validate_profile(const CleanupProfile& profile, const Json& settings,
                      const ShortcutContext& context) {
    const std::string name = py::strip(profile.name);
    if (name.empty()) {
        throw py::ValueError("Give the profile a name.");
    }
    if (py::len(name) > kNameMaxChars) {
        throw py::ValueError("Use a name with 80 characters or fewer.");
    }
    if (py::strip(profile.instructions).empty()) {
        throw py::ValueError("Add instructions for the output you want.");
    }
    const std::string folded = py::casefold(name);
    for (const CleanupProfile& other : load_cleanup_profiles(settings, context.vocabulary)) {
        if (other.id != profile.id && py::casefold(other.name) == folded) {
            throw py::ValueError("A profile with that name already exists.");
        }
    }
    const std::string conflict =
        profile_hotkey_conflict(profile.hotkey, settings, profile.id, context);
    if (!conflict.empty()) {
        throw py::ValueError("That shortcut is already used by " + conflict + ". Choose another.");
    }
}

void save_cleanup_profile(Json& settings, const CleanupProfile& profile,
                          const ShortcutContext& context) {
    validate_profile(profile, settings, context);
    std::vector<CleanupProfile> profiles = load_cleanup_profiles(settings, context.vocabulary);
    const auto found = std::find_if(profiles.begin(), profiles.end(),
                                    [&](const CleanupProfile& p) { return p.id == profile.id; });
    if (found == profiles.end()) {
        profiles.push_back(profile);
    } else {
        *found = profile;
    }
    Json list = Json::array();
    for (const CleanupProfile& item : profiles) {
        list.push_back(profile_to_json(item));
    }
    settings[std::string(keys::transcript_cleanup_profiles.key())] = std::move(list);
}

void delete_cleanup_profile(Json& settings, std::string_view profile_id,
                            hotkeys::Vocabulary vocabulary) {
    Json list = Json::array();
    for (const CleanupProfile& item : load_cleanup_profiles(settings, vocabulary)) {
        if (item.id != profile_id) {
            list.push_back(profile_to_json(item));
        }
    }
    settings[std::string(keys::transcript_cleanup_profiles.key())] = std::move(list);
    if (keys::quick_record_profile.read(settings) == profile_id) {
        settings[std::string(keys::quick_record_profile.key())] = "";
    }
}

std::string compose_profile_prompt(const CleanupProfile& profile,
                                   const std::vector<std::string>& rules) {
    std::string prompt =
        "Transform this speech-to-text dictation into the requested output. Fix punctuation, "
        "remove fillers, and preserve meaning, names, and facts. " +
        config::cleanup_sentences().spoken_corrections +
        " Do not invent missing details. Return only the finished text, without commentary or "
        "surrounding code fences.\n\nOutput instructions:\n" +
        profile.instructions;
    if (profile.use_learned_rules && !rules.empty()) {
        prompt = compose_transcript_cleanup_prompt(prompt, rules);
        prompt += "\nIf a learned formatting rule conflicts with the output instructions, follow "
                  "the output instructions. Keep learned spellings and terminology.";
    }
    return prompt;
}

} // namespace fv::text
