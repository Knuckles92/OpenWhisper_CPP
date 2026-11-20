#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// What a dictation tells the speech engine beyond its audio (services/recognition_context.py):
// only the user's own language choice and dictionary terms, never text read from other apps.
// The helpers take the language and phrases as plain arguments; the recognition context itself
// is dictation::RecognitionContext. faster-whisper's hotwords are not ported (no Local
// Whisper). Every function is thread-safe and pure.
namespace fv::text {

// Whether a backend's speech model takes the phrases, or only the replacement and cleanup
// steps after recognition use the dictionary.
inline constexpr std::string_view SUPPORT_MODEL = "model";
inline constexpr std::string_view SUPPORT_AFTER = "after";

// OpenAI's keyword list, documented without limits; a longer list risks a rejected request.
inline constexpr std::size_t OPENAI_MAX_KEYWORDS = 50;
// Longest phrase passed on, in code points.
inline constexpr std::size_t MAX_PHRASE_CHARS = 64;
// Whisper-style prompts are read as preceding text and kept to the last 224 tokens; code points.
inline constexpr std::size_t OPENAI_PROMPT_MAX_CHARS = 600;

// What a dictation engine family (a keys::selected_model value) does with phrases: "model" for
// the API engine, "" for a remote engine (only its connected host knows), else "after"
// (Parakeet among them: phrases fix the text and never reach the speech model).
std::string engine_support(std::string_view engine);

// The ISO 639-1 code Whisper and OpenAI take ("pt-BR" and "pt_BR" give "pt"), or "" to detect
// (no language, or "auto" in any case).
std::string language_code(const std::optional<std::string_view>& language);

// gpt-transcribe `keywords`: the first phrases that fit, whitespace collapsed, at most
// OPENAI_MAX_KEYWORDS of at most MAX_PHRASE_CHARS each; longer or blank phrases are skipped.
std::vector<std::string> keywords(const std::vector<std::string>& phrases);

// A prompt that primes Whisper-style models with the right spellings: the phrases that fit,
// joined by ", " and ended with "."; "" when none fits.
std::string vocabulary_prompt(const std::vector<std::string>& phrases,
                              std::size_t max_chars = OPENAI_PROMPT_MAX_CHARS);

} // namespace fv::text
