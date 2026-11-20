#include "meeting_agent/agent_settings.h"

#include <algorithm>

#include "pyport/text.h"
#include "settings/keys.h"

namespace fv::meeting_agent {

bool is_installed_core(std::string_view kind) {
    return std::ranges::find(INSTALLED_CORES, kind) != INSTALLED_CORES.end();
}

std::string resolve_meeting_agent_core(const Json& settings) {
    return keys::meeting_agent_core.read(settings);
}

std::vector<std::pair<std::string, std::string>>
resolve_meeting_agent_models(const Json& settings) {
    std::vector<std::pair<std::string, std::string>> models;
    const Json raw = keys::meeting_agent_models.read(settings);
    if (!raw.is_object()) {
        return models;
    }
    for (const auto& [agent, model] : raw.items()) {
        if (is_installed_core(agent) && model.is_string()) {
            models.emplace_back(agent, py::strip(model.get<std::string>()));
        }
    }
    return models;
}

std::string resolve_meeting_agent_model(std::string_view agent_id, const Json& settings) {
    for (auto& [agent, model] : resolve_meeting_agent_models(settings)) {
        if (agent == agent_id) {
            return std::move(model);
        }
    }
    return "";
}

std::string resolve_meeting_live_thinking(const Json& settings) {
    return keys::meeting_live_thinking.read(settings);
}

std::string resolve_meeting_final_thinking(const Json& settings) {
    return keys::meeting_final_thinking.read(settings);
}

std::string resolve_meeting_backup_agent(const Json& settings) {
    return keys::meeting_backup_agent.read(settings);
}

} // namespace fv::meeting_agent
