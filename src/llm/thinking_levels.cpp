#include "llm/thinking_levels.h"

#include <algorithm>
#include <cstdlib>
#include <mutex>
#include <set>
#include <stdexcept>

#include "llm/cleanup_settings.h"
#include "llm/model_catalog.h"
#include "llm/profiles.h"

namespace fv::llm {

namespace {

// What an API reasoning model takes unless its catalog entry says otherwise.
constexpr std::array<std::string_view, 4> kEndpointBase{"off", "low", "medium", "high"};
// claude --effort (checked with Claude Code 2.1.306).
constexpr std::array<std::string_view, 5> kClaudeCode{"low", "medium", "high", "xhigh", "max"};
// Codex models outside its cache, and OpenCode, whose levels depend on the model and are only
// known inside a session.
constexpr std::array<std::string_view, 3> kAgentFallback{"low", "medium", "high"};

std::mutex& sources_mutex() {
    static std::mutex mutex;
    return mutex;
}
CodexEfforts& codex_efforts() {
    static CodexEfforts efforts;
    return efforts;
}
AgentDefaultModel& agent_default_model() {
    static AgentDefaultModel model;
    return model;
}

template <class Range> std::vector<std::string> ordered(const Range& levels) {
    std::vector<std::string> out;
    for (const auto level : kThinkingOrder) {
        if (std::ranges::find(levels, level) != std::ranges::end(levels)) {
            out.emplace_back(level);
        }
    }
    return out;
}

std::ptrdiff_t order_index(std::string_view level) {
    const auto found = std::ranges::find(kThinkingOrder, level);
    return found == kThinkingOrder.end() ? -1 : found - kThinkingOrder.begin();
}

} // namespace

std::optional<std::string_view> thinking_label(std::string_view level) {
    for (const auto& [value, label] : kThinkingLabels) {
        if (value == level) {
            return label;
        }
    }
    return std::nullopt;
}

void set_codex_efforts(CodexEfforts efforts, AgentDefaultModel default_model) {
    std::lock_guard lock(sources_mutex());
    codex_efforts() = std::move(efforts);
    agent_default_model() = std::move(default_model);
}

std::vector<std::string> agent_levels(std::string_view agent_id, std::string_view model) {
    if (agent_id == "claude_code") {
        return ordered(kClaudeCode);
    }
    if (agent_id == "codex") {
        CodexEfforts read_efforts;
        AgentDefaultModel read_default;
        {
            std::lock_guard lock(sources_mutex());
            read_efforts = codex_efforts();
            read_default = agent_default_model();
        }
        const auto efforts =
            read_efforts ? read_efforts() : std::map<std::string, std::vector<std::string>>{};
        std::string chosen(model);
        if (chosen.empty() && read_default) {
            chosen = read_default(agent_id);
        }
        if (const auto found = efforts.find(chosen); found != efforts.end()) {
            return ordered(
                std::vector<std::string_view>(found->second.begin(), found->second.end()));
        }
        if (!efforts.empty()) {
            // An unknown or unnamed model: what every listed model takes.
            std::set<std::string> common(efforts.begin()->second.begin(),
                                         efforts.begin()->second.end());
            for (const auto& [name, levels] : efforts) {
                std::set<std::string> next;
                for (const auto& level : levels) {
                    if (common.contains(level)) {
                        next.insert(level);
                    }
                }
                common = std::move(next);
            }
            if (!common.empty()) {
                return ordered(std::vector<std::string_view>(common.begin(), common.end()));
            }
        }
    }
    std::vector<std::string> fallback;
    for (const auto level : kAgentFallback) {
        fallback.emplace_back(level);
    }
    return fallback;
}

std::vector<std::string> endpoint_levels(const TextLLMProfile* profile, std::string_view model) {
    if (profile == nullptr) {
        return {};
    }
    std::optional<TextModelSpec> spec;
    if (!model.empty()) {
        try {
            spec = model_spec(*profile, model);
        } catch (const std::invalid_argument&) {
            spec.reset();
        }
    }
    bool supported = false;
    if (profile->kind == kProfileKindOpenAi) {
        supported = !spec || spec->reasoning;
    } else {
        // OpenRouter takes a reasoning object for any model; a server the user runs says per
        // model which control it accepts.
        supported =
            profile->kind == kProfileKindOpenRouter ||
            (spec && (spec->reasoning_format == "openai" || spec->reasoning_format == "deepseek"));
    }
    if (!supported) {
        return {};
    }
    std::vector<std::string> levels(kEndpointBase.begin(), kEndpointBase.end());
    if (spec && spec->thinking_levels && spec->thinking_levels->is_object()) {
        for (const auto& [level, wire] : spec->thinking_levels->items()) {
            if (wire.is_null()) {
                std::erase(levels, level);
            } else if (std::ranges::find(levels, level) == levels.end()) {
                levels.push_back(level);
            }
        }
    }
    return ordered(levels);
}

std::vector<std::string> levels_for(std::string_view provider, std::string_view model,
                                    const Json& settings) {
    if (is_installed_cleanup_agent(provider)) {
        return agent_levels(provider, model);
    }
    const auto profile = get_profile(provider, settings);
    return endpoint_levels(profile ? &*profile : nullptr, model);
}

std::optional<std::string> nearest(std::string_view level,
                                   const std::vector<std::string>& offered) {
    if (offered.empty()) {
        return std::nullopt;
    }
    if (std::ranges::find(offered, level) != offered.end()) {
        return std::string(level);
    }
    const auto index = std::max<std::ptrdiff_t>(order_index(level), 0);
    const std::string* best = nullptr;
    std::pair<std::ptrdiff_t, std::ptrdiff_t> best_key{};
    for (const auto& option : offered) {
        const auto position = order_index(option);
        if (position < 0) {
            // ORDER.index raises ValueError for an option outside ORDER, as Python does.
            throw std::invalid_argument("'" + option + "' is not in list");
        }
        const std::pair<std::ptrdiff_t, std::ptrdiff_t> key{std::abs(position - index), position};
        if (best == nullptr || key < best_key) {
            best = &option;
            best_key = key;
        }
    }
    return *best;
}

std::string effective_level(std::string_view level, const std::vector<std::string>& offered) {
    return nearest(level, offered).value_or("off");
}

} // namespace fv::llm
