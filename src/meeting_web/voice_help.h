#pragma once

#include <string>
#include <vector>

#include "base/identity.h"
#include "base/json.h"

// meeting/voice_help.py: the voice command examples the desktop and the dashboard show, and
// services/settings.py's resolve_typesafe_voice_command_names.
// Affinity: thread-safe (functions).
namespace fv::meeting_web {

// The names participants call the note taker by when the settings store none: "note taker",
// "notetaker", "assistant" and the product's two spoken wake names (`defaults_from`).
std::vector<std::string> default_voice_command_names(const AppIdentity& defaults_from = identity());

// The stored typesafe_voice_command_names, each stripped, lower-cased and with its whitespace
// runs joined by one space, blank and non-text entries dropped; the defaults when the setting
// is unset, invalid or leaves nothing. `settings` is a settings mapping.
std::vector<std::string> resolve_voice_command_names(const Json& settings,
                                                     const AppIdentity& defaults_from = identity());

// {names, primary, examples[{label, phrase}]}: display names in configured order ("notetaker"
// folded into "note taker" when both are set, a wake name of either identity shown as that
// identity's product, any other name capitalized, duplicates dropped, "Assistant" first), the
// first of them as primary, and the six example phrases addressed to it.
Json voice_command_guide(const Json& settings, const AppIdentity& defaults_from = identity());

} // namespace fv::meeting_web
