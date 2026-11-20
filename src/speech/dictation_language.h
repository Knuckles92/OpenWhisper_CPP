#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "base/json.h"

// The languages a user dictates in, and the one the next dictation uses
// (services/dictation_language.py). Switching never reloads the engine and never writes
// local_asr_language; the active language is read when a dictation's final pass starts.
//
// Every function takes a settings mapping (a SettingsStore snapshot); the mutators change it in
// place, for SettingsStore::mutate. The engines are OpenWhisper_CPP's: "parakeet", "api" and
// "remote". The paired host's own languages arrive with the remote engine; until then "remote"
// reads as a host that has not reported in (every Whisper language, auto as its own).
// Affinity: thread-safe. Remote capability reports are in memory only.
namespace fv::speech {

struct RemoteVoiceCapabilities {
    std::string host;
    std::string engine_label;
    std::string family;
    std::optional<std::vector<std::string>> languages;
    std::string own_language;
    bool recognition_hints = false;
};

void note_remote_runtime(std::string_view fingerprint, RemoteVoiceCapabilities runtime);
void forget_remote_runtime(std::string_view fingerprint);
std::optional<RemoteVoiceCapabilities> remote_voice_capabilities(const Json& settings);

// The recording engine the settings select ("parakeet", "api" or "remote"); a pre-2.0 API model
// name reads as "api".
std::string engine(const Json& settings);
// Every concrete language the current engine accepts, in display order.
std::vector<std::string> accepted_languages(const Json& settings);
// Why the current engine takes only one language, or "" when it takes more.
std::string single_language_reason(const Json& settings);
// dictation_languages as saved: known codes, in order, without repeats (never "auto").
std::vector<std::string> chosen_languages(const Json& settings);
// Codes from dictation_languages that the current engine accepts.
std::vector<std::string> language_choices(const Json& settings);
// The language for a dictation starting now; "" for the engine's own setting.
std::string job_language(const Json& settings);
// The language the engine uses on its own: a code, or "auto".
std::string engine_language(const Json& settings);
// What the next dictation uses, for display: its language or the engine's own.
std::string current_language(const Json& settings);

// Makes `code` the active language when it is one of the choices.
void set_active(Json& settings, std::string_view code);
// Saves the languages a user dictates in, keeping the active one valid: it stays when still
// chosen, else becomes the first choice the engine accepts ("" when there is none).
void set_chosen(Json& settings, const std::vector<std::string>& codes);
// Adds `code` without changing what the next dictation uses: while the engine's own language
// is in effect it joins the list (unless it is "auto") and stays active.
void add_chosen(Json& settings, std::string_view code);
// Makes the next choice active, wrapping; nothing with fewer than two choices.
void cycle(Json& settings);

// Display name for a language code, such as "English"; the code itself when unknown.
std::string label(std::string_view code);
// Chip text for a language code, such as "EN"; "AUTO" for auto.
std::string short_label(std::string_view code);

} // namespace fv::speech
