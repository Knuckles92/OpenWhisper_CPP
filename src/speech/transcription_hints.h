#pragma once

#include <cstddef>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "base/json.h"
#include "speech/endpoints.h"

namespace fv::speech {

// services/recognition_context.py budgets.
// OpenAI's keyword list, which the SDK documents without limits; a longer list risks a rejected
// request.
inline constexpr std::size_t kOpenAiMaxKeywords = 50;
inline constexpr std::size_t kMaxPhraseChars = 64;
// Whisper-style prompts are read as preceding text and kept to the last 224 tokens.
inline constexpr std::size_t kOpenAiPromptMaxChars = 600;

// The ISO 639-1 code Whisper and OpenAI take ("de-DE" and "de_DE" -> "de"), or "" to detect
// ("" and "auto").
std::string language_code(std::string_view language);
// gpt-transcribe `keywords`: the first phrases that fit, at most 50 of at most 64 code points,
// whitespace runs collapsed.
std::vector<std::string> keywords(std::span<const std::string> phrases);
// A prompt that primes Whisper-style models with the right spellings: "Ksenia, Kubernetes.", or
// "" without phrases.
std::string vocabulary_prompt(std::span<const std::string> phrases,
                              std::size_t max_chars = kOpenAiPromptMaxChars);

// The hint fields a model takes, each present only when it has a value. OpenAI's gpt-transcribe
// takes `keywords` and `languages`; every other model, OpenAI's older ones and other servers'
// Whisper models alike, takes a `prompt` and one `language`. A server that assumes a language
// without the field is asked to detect it (SpeechEndpoint::auto_language).
struct TranscriptionHints {
    std::optional<std::vector<std::string>> keywords;
    std::optional<std::vector<std::string>> languages;
    std::optional<std::string> prompt;
    std::optional<std::string> language;

    bool empty() const;
    // As Python's dict: {"keywords": [...], "languages": [...]} or {"prompt": ..., "language":
    // ...}, in that order.
    Json to_json() const;
    bool operator==(const TranscriptionHints&) const = default;
};

// `language` and `phrases` are the dictation's RecognitionContext ("" and none: no context).
TranscriptionHints transcription_hints(const SpeechEndpoint& endpoint, std::string_view api_model,
                                       std::string_view language,
                                       std::span<const std::string> phrases);

// OpenAI's older models answer in plain text ("text"), as they always have here; everything else
// gets "json", the API's default and so the one format every compatible server takes (LocalAI,
// for one, rejects "text").
std::string response_format(const SpeechEndpoint& endpoint, std::string_view api_model);

} // namespace fv::speech
