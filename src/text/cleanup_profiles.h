#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "base/json.h"
#include "base/platform_info.h"
#include "hotkeys/conflicts.h"
#include "hotkeys/grammar.h"

// Named, on-demand output formats, independent of standard dictation
// (services/cleanup_profiles.py). Profiles are a list of {id, name, instructions, hotkey,
// use_learned_rules} objects under keys::transcript_cleanup_profiles: absent (null) means the
// starters, anything but a list means none. Validation errors are py::ValueError with the
// sentence shown to the user; the mutators edit the settings mapping they are given, for
// SettingsStore::mutate, and change nothing when they throw. Every function is thread-safe and
// pure over its arguments.
namespace fv::text {

struct CleanupProfile {
    std::string id;
    std::string name;
    std::string instructions;
    std::string hotkey;
    bool use_learned_rules = true;

    friend bool operator==(const CleanupProfile&, const CleanupProfile&) = default;
};

// CleanupProfile.to_dict: {id, name, instructions, hotkey, use_learned_rules}.
Json profile_to_json(const CleanupProfile& profile);

// STARTER_PROFILES: "Support ticket" and "Email", shown until a library is saved.
const std::vector<CleanupProfile>& starter_profiles();

// What a shortcut check needs beside the settings. The global shortcut space also holds the
// saved actions, which fv_automation loads and fv_text cannot: a caller that has them passes
// them as `actions`. `standard_hotkeys` are standard shortcuts not saved yet; they replace the
// saved ones. The vocabulary and platform name whose key names and DEFAULT_HOTKEYS apply.
struct ShortcutContext {
    std::optional<Json> standard_hotkeys;
    std::vector<hotkeys::Binding> actions;
    hotkeys::Vocabulary vocabulary = hotkeys::platform_vocabulary();
    std::string_view platform = python_platform();
};

// normalize_hotkey: "kp_" spelled "kp ", modifiers in canonical order.
std::string normalize_hotkey(std::string_view hotkey,
                             hotkeys::Vocabulary vocabulary = hotkeys::platform_vocabulary());

// The valid saved profiles, trimmed, in order; rows without a non-blank id, name and
// instructions and repeated ids are skipped, a hotkey that is not text reads as "".
std::vector<CleanupProfile>
load_cleanup_profiles(const Json& settings,
                      hotkeys::Vocabulary vocabulary = hotkeys::platform_vocabulary());
std::optional<CleanupProfile>
find_cleanup_profile(const Json& settings, std::string_view profile_id,
                     hotkeys::Vocabulary vocabulary = hotkeys::platform_vocabulary());

// Whether a recording can reach AI cleanup with these settings: cleanup is on, Quick Record
// has a profile, a profile has a shortcut, or Command Mode or a transform has one (they always
// use the cleanup model).
bool cleanup_may_run(const Json& settings);

// The profiles, transforms and `actions` of `settings`, as hotkeys::hotkey_conflict reads them.
hotkeys::Bindings hotkey_bindings(const Json& settings, const ShortcutContext& context = {});

// The action, profile, transform or saved action already using `hotkey`, or "".
std::string profile_hotkey_conflict(std::string_view hotkey, const Json& settings,
                                    std::string_view exclude_id = {},
                                    const ShortcutContext& context = {});

// Throws py::ValueError with the reason `profile` cannot be saved.
void validate_profile(const CleanupProfile& profile, const Json& settings,
                      const ShortcutContext& context = {});
// Adds `profile` at the end, or replaces the one with its id in place; throws as
// validate_profile.
void save_cleanup_profile(Json& settings, const CleanupProfile& profile,
                          const ShortcutContext& context = {});
// Removes a profile; clears keys::quick_record_profile when it named it.
void delete_cleanup_profile(Json& settings, std::string_view profile_id,
                            hotkeys::Vocabulary vocabulary = hotkeys::platform_vocabulary());

// The system prompt of a profile's cleanup: the preamble (with the spoken-corrections
// sentence), the output instructions, and with use_learned_rules the numbered rules and the
// note that the instructions win over a conflicting rule.
std::string compose_profile_prompt(const CleanupProfile& profile,
                                   const std::vector<std::string>& rules);

} // namespace fv::text
