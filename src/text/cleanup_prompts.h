#pragma once

#include <array>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "base/json.h"
#include "focus/snapshot.h"

// Which AI cleanup preset applies, and its base prompt (services/cleanup_prompts.py), with the
// rule and prompt resolvers of services/settings.py. The AI cleanup switch stays the master
// on/off; a level only picks the preset while it is on. A saved custom prompt replaces the
// preset. `settings` is a settings mapping; anything else reads as {}. Every function is
// thread-safe and pure over its arguments.
namespace fv::text {

enum class CleanupLevel { None, Light, Medium, High };

// CleanupLevel.STORED: the levels a setting can hold, in order.
inline constexpr std::array<CleanupLevel, 3> STORED_LEVELS{
    CleanupLevel::Light, CleanupLevel::Medium, CleanupLevel::High};

// "none", "light", "medium", "high".
std::string_view to_string(CleanupLevel);
// The level a stored value names, or nullopt.
std::optional<CleanupLevel> parse_cleanup_level(std::string_view value);
// CleanupLevel.LABELS: "Off", "Light", "Medium", "High".
std::string_view level_name(CleanupLevel);

// What the level control and rail show while a custom prompt replaces the preset.
inline constexpr std::string_view CUSTOM_LABEL = "Custom";

// Whether `prompt` is a level preset or the old default, spacing aside.
bool is_built_in(std::string_view prompt);

// The preset in use: None while AI cleanup is off.
CleanupLevel resolve_level(const Json& settings);

// The user's saved prompt, stripped, or "" when none is saved. A saved copy of a built-in
// prompt counts as none: Reset used to store the old default.
std::string custom_prompt(const Json& settings);

// `level`'s built-in prompt; Medium's for a level that has none ("none", "bogus").
const std::string& preset(std::string_view level);
const std::string& preset(CleanupLevel level);

// The custom prompt, or `level`'s preset. For a level that is not stored ("none": cleanup off),
// the saved level's preset: the prompt cleanup would use once it is turned on.
std::string base_prompt(const Json& settings, std::string_view level);
std::string base_prompt(const Json& settings, CleanupLevel level);

// Decides whether an app is a terminal; focus::is_terminal when empty.
using TerminalCheck = std::function<bool(const focus::AppIdentity&)>;

// Keeps a live dictation on one line where line breaks may not belong: at Medium and High
// without a custom prompt, the unknown-app sentence when `snapshot` has no identity (or there
// is no snapshot), the terminal sentence for a terminal, else "".
std::string inline_lists_block(const Json& settings,
                               const std::optional<focus::FocusSnapshot>& snapshot,
                               const TerminalCheck& is_terminal = {});

// "Off", "Custom", or the level's name, as the rail and Basic show it.
std::string level_label(const Json& settings);

// services/settings.py resolve_transcript_cleanup_prompt: the prompt standard cleanup starts
// from, the custom prompt or the saved level's preset.
std::string resolve_transcript_cleanup_prompt(const Json& settings);

// resolve_transcript_cleanup_rules: the stored rules that are strings, stripped, blank ones
// dropped, capped at config::MAX_TRANSCRIPT_CLEANUP_RULES. Not a list: none.
std::vector<std::string> resolve_transcript_cleanup_rules(const Json& settings);

// compose_transcript_cleanup_prompt: `base_prompt`, then the numbered rules under "Additional
// user-taught rules (always apply):"; `base_prompt` alone without rules.
std::string compose_transcript_cleanup_prompt(std::string_view base_prompt,
                                              const std::vector<std::string>& rules);

// config.TRANSCRIPT_CLEANUP_RULE_POLISH_PROMPT and config.TRANSCRIPT_CLEANUP_PROMPT.
const std::string& polish_prompt();
const std::string& legacy_default_prompt();

} // namespace fv::text
