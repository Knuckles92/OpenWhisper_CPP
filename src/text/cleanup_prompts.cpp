#include "text/cleanup_prompts.h"

#include <set>
#include <stdexcept>

#include "focus/synthetic_keys.h"
#include "pyport/text.h"
#include "settings/keys.h"
#include "settings/resolve.h"
#include "text/cleanup_config.h"
#include "text/detail/cleanup_rules.h"
#include "text/detail/prompt_data.h"

namespace fv::text {

namespace {

// _normalized: " ".join(text.split()).
std::string normalized(std::string_view text) {
    std::string out;
    for (const std::string& word : py::split(text)) {
        if (!out.empty()) {
            out += ' ';
        }
        out += word;
    }
    return out;
}

const std::set<std::string>& built_in_prompts() {
    static const std::set<std::string> prompts = [] {
        std::set<std::string> out;
        for (const std::string& prompt : config::legacy_default_cleanup_prompts()) {
            out.insert(normalized(prompt));
        }
        for (const auto& [level, prompt] : config::level_prompts()) {
            out.insert(normalized(prompt));
        }
        return out;
    }();
    return prompts;
}

bool is_stored(std::string_view level) {
    for (const CleanupLevel stored : STORED_LEVELS) {
        if (to_string(stored) == level) {
            return true;
        }
    }
    return false;
}

} // namespace

std::string_view to_string(CleanupLevel level) {
    switch (level) {
    case CleanupLevel::None:
        return "none";
    case CleanupLevel::Light:
        return "light";
    case CleanupLevel::Medium:
        return "medium";
    case CleanupLevel::High:
        return "high";
    }
    return "none";
}

std::optional<CleanupLevel> parse_cleanup_level(std::string_view value) {
    for (const CleanupLevel level :
         {CleanupLevel::None, CleanupLevel::Light, CleanupLevel::Medium, CleanupLevel::High}) {
        if (to_string(level) == value) {
            return level;
        }
    }
    return std::nullopt;
}

std::string_view level_name(CleanupLevel level) {
    switch (level) {
    case CleanupLevel::None:
        return "Off";
    case CleanupLevel::Light:
        return "Light";
    case CleanupLevel::Medium:
        return "Medium";
    case CleanupLevel::High:
        return "High";
    }
    return "Off";
}

bool is_built_in(std::string_view prompt) {
    return built_in_prompts().contains(normalized(prompt));
}

CleanupLevel resolve_level(const Json& settings) {
    const Json& values = detail::or_empty(settings);
    if (!resolve_bool_setting(keys::transcript_cleanup_enabled.key(), values)) {
        return CleanupLevel::None;
    }
    // The setting falls back to "medium" for anything but its three choices.
    return parse_cleanup_level(keys::transcript_cleanup_level.read(values))
        .value_or(CleanupLevel::Medium);
}

std::string custom_prompt(const Json& settings) {
    const std::string stripped =
        py::strip(keys::transcript_cleanup_prompt.read(detail::or_empty(settings)));
    if (stripped.empty() || is_built_in(stripped)) {
        return "";
    }
    return stripped;
}

const std::string& preset(std::string_view level) {
    const auto& prompts = config::level_prompts();
    for (const auto& [name, prompt] : prompts) {
        if (name == level && !prompt.empty()) {
            return prompt;
        }
    }
    for (const auto& [name, prompt] : prompts) {
        if (name == to_string(CleanupLevel::Medium)) {
            return prompt;
        }
    }
    throw std::out_of_range("cleanup_prompts.json has no medium preset");
}

const std::string& preset(CleanupLevel level) {
    return preset(to_string(level));
}

std::string base_prompt(const Json& settings, std::string_view level) {
    std::string custom = custom_prompt(settings);
    if (!custom.empty()) {
        return custom;
    }
    if (!is_stored(level)) {
        return preset(keys::transcript_cleanup_level.read(detail::or_empty(settings)));
    }
    return preset(level);
}

std::string base_prompt(const Json& settings, CleanupLevel level) {
    return base_prompt(settings, to_string(level));
}

std::string inline_lists_block(const Json& settings,
                               const std::optional<focus::FocusSnapshot>& snapshot,
                               const TerminalCheck& is_terminal) {
    const CleanupLevel level = resolve_level(settings);
    if (!custom_prompt(settings).empty() ||
        (level != CleanupLevel::Medium && level != CleanupLevel::High)) {
        return "";
    }
    if (!snapshot || !snapshot->identity) {
        return config::cleanup_sentences().unknown_app_lines;
    }
    const bool terminal =
        is_terminal ? is_terminal(*snapshot->identity) : focus::is_terminal(snapshot->identity);
    return terminal ? config::cleanup_sentences().terminal_lines : std::string();
}

std::string level_label(const Json& settings) {
    const CleanupLevel level = resolve_level(settings);
    if (level == CleanupLevel::None) {
        return std::string(level_name(CleanupLevel::None));
    }
    if (!custom_prompt(settings).empty()) {
        return std::string(CUSTOM_LABEL);
    }
    return std::string(level_name(level));
}

std::string resolve_transcript_cleanup_prompt(const Json& settings) {
    return base_prompt(settings, keys::transcript_cleanup_level.read(detail::or_empty(settings)));
}

std::vector<std::string> resolve_transcript_cleanup_rules(const Json& settings) {
    return detail::cleanup_rules(settings);
}

std::string compose_transcript_cleanup_prompt(std::string_view base_prompt,
                                              const std::vector<std::string>& rules) {
    std::string out(base_prompt);
    if (rules.empty()) {
        return out;
    }
    out += "\n\nAdditional user-taught rules (always apply):\n";
    for (std::size_t i = 0; i < rules.size(); ++i) {
        if (i > 0) {
            out += '\n';
        }
        out += std::to_string(i + 1) + ". " + rules[i];
    }
    return out;
}

const std::string& polish_prompt() {
    return config::rule_polish_prompt();
}

const std::string& legacy_default_prompt() {
    return config::transcript_cleanup_prompt();
}

} // namespace fv::text
