#include "text/cleanup_config.h"

#include <stdexcept>

#include "text/detail/cleanup_rules.h"
#include "text/detail/prompt_data.h"

namespace fv::text::config {

static_assert(MAX_TRANSCRIPT_CLEANUP_RULES == detail::kMaxCleanupRules);

namespace {

const std::string& named(std::string_view name) {
    return detail::prompt_text("sentences", name);
}

} // namespace

const CleanupSentences& cleanup_sentences() {
    static const CleanupSentences sentences{
        named("TRANSCRIPT_CLEANUP_INTRO"),
        named("TRANSCRIPT_CLEANUP_BASICS"),
        named("TRANSCRIPT_CLEANUP_LIGHT_ONLY"),
        named("TRANSCRIPT_CLEANUP_TIGHTEN"),
        named("TRANSCRIPT_CLEANUP_SPOKEN_CORRECTIONS"),
        named("TRANSCRIPT_CLEANUP_SPOKEN_LISTS"),
        named("TRANSCRIPT_CLEANUP_PARAGRAPHS"),
        named("TRANSCRIPT_CLEANUP_UNKNOWN_APP_LINES"),
        named("TRANSCRIPT_CLEANUP_TERMINAL_LINES"),
        named("TRANSCRIPT_CLEANUP_LANGUAGE"),
        named("TRANSCRIPT_CLEANUP_GUARD"),
    };
    return sentences;
}

const std::vector<std::pair<std::string, std::string>>& level_prompts() {
    static const std::vector<std::pair<std::string, std::string>> prompts = [] {
        std::vector<std::pair<std::string, std::string>> out;
        for (const auto& [level, entry] : detail::prompt_data().at("levels").items()) {
            out.emplace_back(level, entry.at("prompt").get<std::string>());
        }
        return out;
    }();
    return prompts;
}

const std::vector<std::string>& level_sentence_names(std::string_view level) {
    static const std::vector<std::pair<std::string, std::vector<std::string>>> names = [] {
        std::vector<std::pair<std::string, std::vector<std::string>>> out;
        for (const auto& [name, entry] : detail::prompt_data().at("levels").items()) {
            out.emplace_back(name, entry.at("sentences").get<std::vector<std::string>>());
        }
        return out;
    }();
    for (const auto& [name, list] : names) {
        if (name == level) {
            return list;
        }
    }
    throw std::out_of_range("No cleanup level " + std::string(level));
}

const std::string& sentence(std::string_view name) {
    return named(name);
}

const std::string& transcript_cleanup_prompt() {
    static const std::string prompt =
        detail::prompt_data().at("transcript_cleanup_prompt").get<std::string>();
    return prompt;
}

const std::vector<std::string>& legacy_default_cleanup_prompts() {
    static const std::vector<std::string> prompts =
        detail::prompt_data().at("legacy_default_cleanup_prompts").get<std::vector<std::string>>();
    return prompts;
}

const std::string& rule_polish_prompt() {
    static const std::string prompt =
        detail::prompt_data().at("rule_polish_prompt").get<std::string>();
    return prompt;
}

} // namespace fv::text::config
