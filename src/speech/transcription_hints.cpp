#include "speech/transcription_hints.h"

#include "pyport/text.h"

namespace fv::speech {

namespace {

// recognition_context._bounded: whitespace runs collapsed, phrases over the per-phrase limit
// skipped, the first ones that fit kept.
std::vector<std::string> bounded(std::span<const std::string> phrases, std::size_t count,
                                 std::size_t chars, std::string_view separator) {
    std::vector<std::string> kept;
    std::size_t used = 0;
    for (const std::string& raw : phrases) {
        std::string phrase;
        for (const std::string& word : py::split(raw)) {
            phrase += (phrase.empty() ? "" : " ") + word;
        }
        const std::size_t length = py::len(phrase);
        if (phrase.empty() || length > kMaxPhraseChars) {
            continue;
        }
        const std::size_t cost = length + (kept.empty() ? 0 : py::len(separator));
        if (kept.size() >= count || used + cost > chars) {
            break;
        }
        kept.push_back(std::move(phrase));
        used += cost;
    }
    return kept;
}

} // namespace

std::string language_code(std::string_view language) {
    if (language.empty() || py::lower(language) == "auto") {
        return {};
    }
    std::string replaced(language);
    for (char& c : replaced) {
        if (c == '_') {
            c = '-';
        }
    }
    return py::lower(replaced.substr(0, replaced.find('-')));
}

std::vector<std::string> keywords(std::span<const std::string> phrases) {
    return bounded(phrases, kOpenAiMaxKeywords, kOpenAiMaxKeywords * kMaxPhraseChars, "");
}

std::string vocabulary_prompt(std::span<const std::string> phrases, std::size_t max_chars) {
    const auto kept = bounded(phrases, phrases.size(), max_chars - 1, ", ");
    if (kept.empty()) {
        return {};
    }
    std::string prompt;
    for (const std::string& phrase : kept) {
        prompt += (prompt.empty() ? "" : ", ") + phrase;
    }
    return prompt + ".";
}

bool TranscriptionHints::empty() const {
    return !keywords && !languages && !prompt && !language;
}

Json TranscriptionHints::to_json() const {
    Json out = Json::object();
    if (keywords) {
        out["keywords"] = *keywords;
    }
    if (languages) {
        out["languages"] = *languages;
    }
    if (prompt) {
        out["prompt"] = *prompt;
    }
    if (language) {
        out["language"] = *language;
    }
    return out;
}

TranscriptionHints transcription_hints(const SpeechEndpoint& endpoint, std::string_view api_model,
                                       std::string_view language,
                                       std::span<const std::string> phrases) {
    std::string code = language_code(language);
    TranscriptionHints hints;
    if (endpoint.is_openai() && api_model == "gpt-transcribe") {
        auto words = keywords(phrases);
        if (!words.empty()) {
            hints.keywords = std::move(words);
        }
        if (!code.empty()) {
            hints.languages = std::vector<std::string>{code};
        }
    } else {
        std::string prompt = vocabulary_prompt(phrases);
        if (!prompt.empty()) {
            hints.prompt = std::move(prompt);
        }
        if (code.empty()) {
            code = endpoint.auto_language;
        }
        if (!code.empty()) {
            hints.language = std::move(code);
        }
    }
    return hints;
}

std::string response_format(const SpeechEndpoint& endpoint, std::string_view api_model) {
    return endpoint.is_openai() && api_model != "gpt-transcribe" ? "text" : "json";
}

} // namespace fv::speech
