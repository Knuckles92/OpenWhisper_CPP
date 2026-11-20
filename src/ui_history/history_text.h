#pragma once

#include <optional>
#include <string>
#include <string_view>

#include "base/json.h"
#include "history/entry.h"

// The words a History card and the entry dialog show for an entry (the module-level helpers of
// ui_qt/widgets/history_sidebar.py), as plain functions so golden vectors pin them.
// Affinity: thread-safe (pure functions).
namespace fv {

// _format_model_name: a backend model identifier for a compact badge. Stored values may carry
// device detail, "local_whisper (turbo | cuda (float16))" -> "Local · turbo".
//
// OpenWhisper_CPP stores its Parakeet transcriptions as "parakeet (<model label> | <device>)"
// (TranscriptionRuntime's model info), which OpenWhisper's table would show as
// "parakeet · Parakeet TDT 0.6B v3". The label already names the engine, so a "parakeet" base
// shows the label alone, and "Parakeet" when there is none.
std::string format_model_name(std::string_view model);

// _cleanup_level_label: "Light", "Medium", "High", "Custom" or "Profile" for the entry's
// cleanup_level (case-insensitive), "" for anything else.
std::string cleanup_level_label(const HistoryEntry& entry);

// services.text_llm.profile_display_name: the installed agents' and the built-in profiles'
// names ("Claude Code", "Ollama", "Groq"), else the id stripped. A custom profile's own name
// comes from the settings with M4's profile store; until then its id is shown.
std::string profile_display_name(std::string_view profile_id);

// _cleanup_provider: "OpenAI" and "OpenRouter", else profile_display_name, or the id itself
// when that is empty; "" without a provider.
std::string cleanup_provider_name(const HistoryEntry& entry);

// _format_cleanup_info: "Medium · gpt-4o-mini". Entries from before levels show the provider in
// the level's place, and entries written before the cleanup columns existed may carry raw_text
// without provider or model: those read "Cleaned".
std::string format_cleanup_info(const HistoryEntry& entry);

// _cleanup_tooltip: "Transcript cleaned with OpenAI · gpt-4o-mini (Medium)", or "Transcript was
// cleaned (model not recorded)".
std::string cleanup_tooltip(const HistoryEntry& entry);

// _entry_was_cleaned: the text shown is AI cleanup's (legacy entries included); false once the
// user chose the original back.
bool entry_was_cleaned(const HistoryEntry& entry);

// _kind_label: "Transcription failed", "Command", "Transform · Polish", "Action · Send",
// "Walkthrough", or "" for a dictation or a file. The runtime saves a transform's source name
// already prefixed ("Transform · Polish"); older or synced rows may hold the bare name.
std::string kind_label(const HistoryEntry& entry);

// _failed_recording_hint: what a failed recording's card and dialog say in place of its text.
std::string failed_recording_hint(bool has_audio);

// _location_chip: where an entry is kept, "" for an entry kept here alone.
struct LocationChip {
    std::string text;
    std::string tooltip;
    friend bool operator==(const LocationChip&, const LocationChip&) = default;
};
LocationChip location_chip(const HistoryEntry& entry);

// _card_fingerprint: what a card shows, so an unchanged entry keeps its card (and the playback
// it shows) across a refresh. Every field of the entry, the cleanup display text, and, for an
// entry with a recording kept here, whether the file still exists.
Json card_fingerprint(const HistoryEntry& entry, std::optional<bool> audio_available);

} // namespace fv
