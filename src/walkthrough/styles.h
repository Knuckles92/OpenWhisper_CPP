#pragma once

#include <array>
#include <cstddef>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "base/json.h"

// Write-up styles: named prompts a walkthrough can be written up with
// (services/walkthrough/styles.py). The default, "Walkthrough", is built in and always first.
// The rest are the user's to add, replace and delete, saved in walkthrough_write_up_styles;
// until any is saved, the starters show. Picking a style only fills in the instructions: what's
// saved for the next walkthrough is still the words, not the name.
// Affinity: plain functions over a settings snapshot; any thread.
namespace fv::walkthrough {

// The longest name a style may have, in characters.
inline constexpr std::size_t MAX_NAME = 60;
// How many styles may be saved beside the default.
inline constexpr std::size_t MAX_STYLES = 30;

struct WriteUpStyle {
    std::string id;
    std::string name;
    std::string instructions;

    // {"id", "name", "instructions"}, as saved.
    Json to_dict() const;
    friend bool operator==(const WriteUpStyle&, const WriteUpStyle&) = default;
};

const WriteUpStyle& default_style();
// Step-by-step guide, Bug report, Design feedback, Task for an agent.
const std::array<WriteUpStyle, 4>& starter_styles();

// The user's styles, without the default: the starters until the setting exists; entries that
// aren't objects with three non-blank strings, repeated ids and the default's id are skipped;
// each field trimmed; at most MAX_STYLES.
std::vector<WriteUpStyle> saved_styles(const Json& settings);
// Every style to pick from: the default, then the user's.
std::vector<WriteUpStyle> load_styles(const Json& settings);

// The style called `name` (trimmed, regardless of case) among `styles`.
std::optional<WriteUpStyle> style_named(std::string_view name,
                                        std::span<const WriteUpStyle> styles);
// The same among load_styles(settings).
std::optional<WriteUpStyle> style_named(const Json& settings, std::string_view name);

// The style these instructions are, word for word once trimmed, or nullopt for the user's own;
// "" is the default's.
std::optional<WriteUpStyle> style_for(std::string_view instructions,
                                      std::span<const WriteUpStyle> styles);
// The same among load_styles(settings).
std::optional<WriteUpStyle> style_for(const Json& settings, std::string_view instructions);

// Saves `instructions` as the style `name` in `settings`, replacing one by that name (its id
// stays). A new style's id is 12 hex characters of a random UUID, or `id` when given. Call it
// inside SettingsStore's mutate to keep concurrent edits. fv::py::ValueError saying what to fix:
// "Give the style a name.", "Use a name with 60 characters or fewer.", "Add the instructions
// for the write-up you want.", "\"Walkthrough\" is the built-in style. Choose another name.",
// "You can keep up to 30 styles. Delete one first."; `settings` is then unchanged.
WriteUpStyle save_style(Json& settings, std::string_view name, std::string_view instructions,
                        std::string_view id = {});

// Removes the saved style `id`; the default stays. Deleting a starter saves the others.
void delete_style(Json& settings, std::string_view id);

} // namespace fv::walkthrough
