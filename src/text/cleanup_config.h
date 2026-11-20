#pragma once

#include <cstddef>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "focus/snapshot.h"
#include "text/dictionary.h"
#include "text/snippets.h"

// config.py's AI cleanup settings (AppConfig.TRANSCRIPT_CLEANUP_* and the limits beside them), and
// the prompt text tools/gen/cleanup_prompts.py writes to data/cleanup_prompts.json from the
// pinned OpenWhisper source. The prompt text is the product: it is generated, never retyped.
// Every function is thread-safe.
namespace fv::text::config {

inline constexpr bool TRANSCRIPT_CLEANUP_ENABLED = false;
// Each request's timeout, seconds.
inline constexpr double TRANSCRIPT_CLEANUP_TIMEOUT_S = 8.0;
// One retry rides out a dropped connection or a brief 5xx; the paste waits on cleanup.
inline constexpr int TRANSCRIPT_CLEANUP_MAX_RETRIES = 1;
inline constexpr std::string_view TRANSCRIPT_CLEANUP_PROVIDER = "openrouter";
inline constexpr std::string_view TRANSCRIPT_CLEANUP_MODEL = "gpt-6-luna";
inline constexpr std::string_view TRANSCRIPT_CLEANUP_OPENROUTER_MODEL = "openrouter/free";
inline constexpr std::string_view TRANSCRIPT_CLEANUP_MODEL_SORT = "alphabetical";
inline constexpr std::string_view TRANSCRIPT_CLEANUP_REASONING = "off";
inline constexpr std::string_view OPENROUTER_BASE_URL = "https://openrouter.ai/api/v1";
// The preset while AI cleanup is on: "light", "medium" or "high".
inline constexpr std::string_view TRANSCRIPT_CLEANUP_LEVEL = "medium";

// Learned cleanup rules appended to the base prompt.
inline constexpr std::size_t MAX_TRANSCRIPT_CLEANUP_RULES = 50;
inline constexpr std::size_t MAX_DICTIONARY_TERMS = text::MAX_TERMS;
inline constexpr std::size_t MAX_RECOGNITION_PHRASES = text::MAX_RECOGNITION_PHRASES;
inline constexpr std::size_t MAX_SNIPPETS = text::MAX_SNIPPETS;
inline constexpr std::size_t MAX_TEXT_TRANSFORMS = 50;

// Text read around the caret, in code points, and how long a recording start waits for it.
inline constexpr std::size_t CONTEXT_BEFORE_CHARS = focus::CONTEXT_BEFORE_CHARS;
inline constexpr std::size_t CONTEXT_AFTER_CHARS = focus::CONTEXT_AFTER_CHARS;
inline constexpr double CONTEXT_CAPTURE_DEADLINE_S = 0.4;
// How long Command Mode and transforms wait for a synthetic copy to change the clipboard.
inline constexpr int COMMAND_SELECTION_TIMEOUT_MS = 700;

// A stitched multi-part transcript is far longer than a dictation, so it gets its own timeout
// (seconds) and keeps the SDK's two retries: nothing waits to paste it.
inline constexpr double TRANSCRIPT_BATCH_CLEANUP_TIMEOUT_S = 120.0;
inline constexpr int TRANSCRIPT_BATCH_CLEANUP_MAX_RETRIES = 2;

// The sentences the level presets are built from (TRANSCRIPT_CLEANUP_INTRO ... GUARD).
struct CleanupSentences {
    std::string intro;
    std::string basics;
    std::string light_only;
    std::string tighten;
    std::string spoken_corrections;
    std::string spoken_lists;
    std::string paragraphs;
    // Added after a Medium or High preset when a live dictation goes where line breaks may not
    // belong: an app the capture cannot identify, or a terminal.
    std::string unknown_app_lines;
    std::string terminal_lines;
    std::string language;
    std::string guard;
};
const CleanupSentences& cleanup_sentences();

// TRANSCRIPT_CLEANUP_LEVEL_PROMPTS: (level, preset) for "light", "medium" and "high", in order.
const std::vector<std::pair<std::string, std::string>>& level_prompts();
// The config attribute names of the sentences a level's preset joins with one space.
const std::vector<std::string>& level_sentence_names(std::string_view level);
// A sentence by its config attribute name ("TRANSCRIPT_CLEANUP_GUARD").
const std::string& sentence(std::string_view name);

// TRANSCRIPT_CLEANUP_PROMPT: the built-in prompt before cleanup levels.
const std::string& transcript_cleanup_prompt();
// LEGACY_DEFAULT_CLEANUP_PROMPTS: Reset used to save these, so a saved copy is not custom.
const std::vector<std::string>& legacy_default_cleanup_prompts();
// TRANSCRIPT_CLEANUP_RULE_POLISH_PROMPT.
const std::string& rule_polish_prompt();

} // namespace fv::text::config
