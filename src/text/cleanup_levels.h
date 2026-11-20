#pragma once

#include <array>
#include <cstddef>
#include <string_view>
#include <utility>

// Copy for the AI cleanup level control, shared by the AI cleanup page, Home and guided setup
// (ui_qt/dialogs/cleanup_levels.py). Thread-safe constants.
namespace fv::text::cleanup_levels {

// The control's choice while a saved custom prompt replaces the presets.
inline constexpr std::string_view CUSTOM = "custom";

struct Segment {
    std::string_view value;
    std::string_view title;
    std::string_view detail;
};

// SEGMENTS: one per segment, in order, Custom last.
inline constexpr std::array<Segment, 4> SEGMENTS{{
    {"light", "Light", "Punctuation & fillers"},
    {"medium", "Medium", "+ corrections & lists"},
    {"high", "High", "+ tighter wording"},
    {CUSTOM, "Custom", "Your own prompt"},
}};

// NOTES: what the chosen level does, under the AI cleanup page's control.
inline constexpr std::array<std::pair<std::string_view, std::string_view>, 4> NOTES{{
    {"light", "Fixes punctuation, capitalization, filler words, and obvious mishearings. Your "
              "words stay as you said them."},
    {"medium", "Also keeps just the fix when you correct yourself (“at 2, actually 3” becomes "
               "“at 3”), turns spoken lists into lists, and adds paragraph breaks."},
    {"high", "Also drops false starts and repeats, and tightens the wording while keeping your "
             "meaning and voice."},
    {CUSTOM, "Your custom prompt below replaces the presets. Pick a level to go back to one."},
}};

// SHORT_NOTES: the same, in a few words, for Home's row.
inline constexpr std::array<std::pair<std::string_view, std::string_view>, 4> SHORT_NOTES{{
    {"light", "Punctuation and filler words only."},
    {"medium", "Also handles self-corrections and spoken lists."},
    {"high", "Also tightens your wording."},
    {CUSTOM, "Uses your custom prompt from AI cleanup."},
}};

// NOTES[value] and SHORT_NOTES[value]; "" for a value without one.
std::string_view note(std::string_view value);
std::string_view short_note(std::string_view value);

// The segment for a level or CUSTOM; Medium's for anything else.
std::size_t segment_index(std::string_view value);

} // namespace fv::text::cleanup_levels
