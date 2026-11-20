#include "llm/cleanup_agent_offer.h"

#include <algorithm>
#include <stdexcept>
#include <string>

#include "settings/keys.h"
#include "settings/store.h"

namespace fv::llm::cleanup_agent_offer {

namespace {

bool offered(std::string_view agent_id) {
    return std::ranges::find(OFFERED, agent_id) != OFFERED.end();
}

} // namespace

bool pending(const Json& settings) {
    return keys::transcript_cleanup_enabled.read(settings) &&
           !keys::transcript_cleanup_provider.stored(settings).has_value() &&
           !keys::cleanup_agent_offer_answered.read(settings);
}

std::vector<agents::InstalledAgent> offerable(const agents::ScanResult* scan) {
    std::vector<agents::InstalledAgent> agents;
    if (scan == nullptr) {
        return agents;
    }
    for (const auto agent_id : OFFERED) {
        const auto found = scan->find(std::string(agent_id));
        if (found == scan->end() || !found->second) {
            continue;
        }
        const auto& agent = *found->second;
        if (agent.problem.empty() && agent.signed_in == true) {
            agents.push_back(agent);
        }
    }
    return agents;
}

void accept(std::string_view agent_id, SettingsStore& settings, bool consented) {
    if (!offered(agent_id)) {
        throw std::invalid_argument("Not an offered cleanup agent: " + std::string(agent_id));
    }
    const std::string id(agent_id);
    settings.mutate([&](Json& saved) {
        Json memory = keys::cleanup_model_memory.read(saved);
        if (!memory.is_object()) {
            memory = Json::object();
        }
        const auto remembered = memory.find(id);
        const std::string model = remembered != memory.end() && remembered->is_string()
                                      ? remembered->get<std::string>()
                                      : std::string();
        memory[id] = model;
        saved[keys::cleanup_model_memory.key()] = std::move(memory);
        saved[keys::transcript_cleanup_provider.key()] = id;
        saved[keys::transcript_cleanup_model.key()] = model;
        saved[keys::cleanup_agent_offer_answered.key()] = true;
        if (consented) {
            saved[keys::fv_cleanup_agent_consent.key()] = id;
        }
    });
}

void decline(SettingsStore& settings) {
    settings.save_setting(keys::cleanup_agent_offer_answered.key(), Json(true));
}

} // namespace fv::llm::cleanup_agent_offer
