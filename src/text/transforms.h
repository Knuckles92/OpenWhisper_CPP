#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "base/json.h"
#include "hotkeys/grammar.h"
#include "text/cleanup_profiles.h"

// Saved rewrite instructions applied to selected text, optionally by shortcut
// (services/text_transforms.py). Transforms are a list of {id, name, instruction, hotkey}
// objects under keys::text_transforms: absent means the four starters, an empty list means
// none. Validation errors are py::ValueError with the sentence shown to the user; the mutators
// edit the settings mapping they are given, for SettingsStore::mutate, and change nothing when
// they throw. Every function is thread-safe and pure over its arguments.
namespace fv::text {

// config.MAX_TEXT_TRANSFORMS.
inline constexpr std::size_t MAX_TRANSFORMS = 50;
// A transform name's longest length, in code points.
inline constexpr std::size_t NAME_MAX_CHARS = 80;

struct Transform {
    std::string id;
    std::string name;
    std::string instruction;
    std::string hotkey;

    friend bool operator==(const Transform&, const Transform&) = default;
};

// Transform.to_dict: {id, name, instruction, hotkey}.
Json transform_to_json(const Transform& transform);

// STARTER_TRANSFORMS: Polish, Make concise, Fix grammar, Prompt engineer.
const std::vector<Transform>& starter_transforms();

// The saved transforms, trimmed, in order; malformed rows and repeated ids are skipped, the
// list stops at MAX_TRANSFORMS, and a hotkey that is not a non-blank string reads as "".
std::vector<Transform>
load_transforms(const Json& settings,
                hotkeys::Vocabulary vocabulary = hotkeys::platform_vocabulary());
std::optional<Transform>
find_transform(const Json& settings, std::string_view transform_id,
               hotkeys::Vocabulary vocabulary = hotkeys::platform_vocabulary());

// The action, profile, other transform or saved action already using `hotkey`, or "".
std::string transform_hotkey_conflict(std::string_view hotkey, const Json& settings,
                                      std::string_view exclude_id = {},
                                      const ShortcutContext& context = {});

// Throws py::ValueError with the reason `transform` cannot be saved. The cap refuses only a
// transform that is not saved yet.
void validate_transform(const Transform& transform, const Json& settings,
                        const ShortcutContext& context = {});
// Adds `transform` (trimmed, shortcut normalized) at the end, or replaces the one with its id
// in place; throws as validate_transform, or "This transform has no id.".
void save_transform(Json& settings, const Transform& transform,
                    const ShortcutContext& context = {});
// Removes a transform; the saved list is rewritten from load_transforms.
void delete_transform(Json& settings, std::string_view transform_id,
                      hotkeys::Vocabulary vocabulary = hotkeys::platform_vocabulary());

} // namespace fv::text
