#include "ui_meeting/ai_insights_copy.h"

#include <algorithm>

#include "base/identity.h"
#include "base/log.h"
#include "llm/cleanup_settings.h"
#include "meeting_agent/agent_settings.h"

namespace fv {
namespace {

constexpr log::Logger kLog{"ui_meeting.ai_copy"};

constexpr std::string_view kIntelligenceSettings =
    "Settings \xE2\x86\x92 Meeting Mode \xE2\x86\x92 "
    "Intelligence";
constexpr std::string_view kDot = " \xC2\xB7 ";

std::optional<llm::TextLLMProfile> meetingProfile(const Json& settings, const AiCopyDeps& deps) {
    if (deps.meeting_profile) {
        return deps.meeting_profile(settings);
    }
    return llm::resolve_meeting_llm_profile(settings);
}

std::optional<agents::ScanResult> cachedScan(const AiCopyDeps& deps) {
    if (deps.cached_scan) {
        return deps.cached_scan();
    }
    const auto agents = agents::installed_agents_if_set();
    return agents ? agents->cached_scan() : std::nullopt;
}

// _backup_agent_name.
std::string backupName(const Json& settings) {
    try {
        const auto backup = meeting_agent::resolve_meeting_backup_agent(settings);
        const auto* spec = agents::find_agent_spec(backup);
        return spec != nullptr ? std::string(spec->name) : "";
    } catch (const std::exception& error) {
        kLog.warning("Could not read the backup agent: {}", error.what());
        return "";
    }
}

} // namespace

MeetingConsentCopy meetingConsentCopy(const meeting_agent::MeetingAgentRoute& route) {
    if (route.installed()) {
        const auto* spec = agents::find_agent_spec(route.kind);
        return {spec != nullptr ? std::string(spec->name)
                                : llm::profile_display_name(route.kind, Json::object()),
                "The coding agent uses its own sign-in and model provider. It can send meeting "
                "text to that provider.",
                true,
                route.kind,
                {}};
    }
    const auto profile =
        llm::profile_from_agent_config(route.provider, route.endpoint, Json::object());
    const auto destination = llm::consent_destination(profile);
    const bool remote = llm::destination_is_remote(profile);
    return {destination,
            remote ? "This destination is remote. Meeting text leaves this computer."
                   : "The selected text endpoint is on this computer.",
            remote,
            {},
            {}};
}

MeetingConsentCopy meetingConsentCopy(const meeting_agent::MeetingAgentRoute& route,
                                      const Json& settings) {
    auto copy = meetingConsentCopy(route);
    copy.backup_name = backupName(settings);
    return copy;
}

std::string meetingIntelligenceSummary(const Json& settings) {
    try {
        const auto route = meeting_agent::resolve_meeting_agent_route(settings);
        return meetingConsentCopy(route).destination;
    } catch (const std::exception&) {
        return "Choose an insights model";
    }
}

std::string ai_insights_tooltip() {
    return "On: send transcript text to the AI model chosen\n"
           "in Settings for live insights and a final report.\n"
           "Off: keep the recording and transcript only.\n"
           "Audio routing is set separately in Voice & speakers.";
}

std::pair<std::string, std::string> ai_insights_destination(const Json& settings,
                                                            const AiCopyDeps& deps) {
    std::optional<llm::TextLLMProfile> profile;
    std::string model;
    try {
        const auto core = meeting_agent::resolve_meeting_agent_core(settings);
        if (meeting_agent::is_installed_core(core)) {
            return installed_agent_destination(core, settings, deps);
        }
        profile = meetingProfile(settings, deps);
        model = llm::resolve_meeting_llm_model(settings);
    } catch (const std::exception& error) {
        kLog.warning("Could not resolve the meeting AI model: {}", error.what());
        profile.reset();
    }
    if (!profile) {
        return {"No AI model chosen", "pick one in " + std::string(kIntelligenceSettings)};
    }
    std::string where;
    std::string privacy;
    if (profile->is_local()) {
        where = profile->name + " on this PC";
        // OpenWhisper drops the claim while Fast judgments use Jev or a hosted model, which
        // receive excerpts. FioriVoce's meetings use only a judge on this computer, so the
        // claim holds whatever Fast judgments are set to.
        privacy = "transcript text stays on this computer";
    } else {
        where = profile->name;
        privacy = "sends transcript text, never audio";
    }
    // OpenRouter ids carry the vendor ("deepseek/deepseek-v4.1-flash"); the provider already
    // names the route, so the short name reads better.
    const auto slash = model.rfind('/');
    const auto shortModel = slash == std::string::npos ? model : model.substr(slash + 1);
    if (!shortModel.empty()) {
        where += std::string(kDot) + shortModel;
    }
    return {where, privacy};
}

std::pair<std::string, std::string> installed_agent_destination(std::string_view agent_id,
                                                                const Json& settings,
                                                                const AiCopyDeps& deps) {
    const auto& spec = agents::agent_spec(agent_id);
    const std::string name(spec.name);
    const std::string elsewhere = "choose another agent in " + std::string(kIntelligenceSettings);
    if (const auto scan = cachedScan(deps)) {
        const auto found = scan->find(std::string(agent_id));
        if (found == scan->end() || !found->second) {
            return {name + " not found", elsewhere};
        }
        if (!found->second->problem.empty()) {
            return {name + " needs an update", "update it, or " + elsewhere};
        }
        if (found->second->signed_in == false) {
            return {name + " isn't signed in", "sign in, or " + elsewhere};
        }
    }
    return {agents::describe_choice(agent_id,
                                    meeting_agent::resolve_meeting_agent_model(agent_id, settings)),
            "your agent receives transcript text, never audio"};
}

std::string agent_core_label(std::string_view core) {
    // The native agent loop replaced OpenWhisper's Pi sidecar; it carries the product's name.
    if (core == meeting_agent::CORE_PI || core == meeting_agent::CORE_DIRECT) {
        return std::string(identity().product);
    }
    if (core == meeting_agent::CORE_OPENCODE) {
        return "OpenCode SDK";
    }
    if (const auto* spec = agents::find_agent_spec(core)) {
        return std::string(spec->name);
    }
    return "Unknown agent";
}

std::string meeting_text_summary(const Json& settings, const AiCopyDeps& deps) {
    const auto core = meeting_agent::resolve_meeting_agent_core(settings);
    if (meeting_agent::is_installed_core(core)) {
        if (const auto scan = cachedScan(deps)) {
            const std::string name(agents::agent_spec(core).name);
            const auto found = scan->find(core);
            if (found == scan->end() || !found->second) {
                return name + std::string(kDot) + "not installed";
            }
            if (!found->second->problem.empty()) {
                return name + std::string(kDot) + "update needed";
            }
            if (found->second->signed_in == false) {
                return name + std::string(kDot) + "sign in needed";
            }
        }
        return agents::describe_choice(core,
                                       meeting_agent::resolve_meeting_agent_model(core, settings));
    }
    return llm::profile_display_name(llm::resolve_meeting_llm_provider(settings), settings) +
           std::string(kDot) + llm::resolve_meeting_llm_model(settings);
}

std::pair<std::string, std::string> meeting_intelligence_overview(const Json& settings,
                                                                  const AiCopyDeps& deps) {
    const auto core = meeting_agent::resolve_meeting_agent_core(settings);
    if (meeting_agent::is_installed_core(core)) {
        const auto model = meeting_agent::resolve_meeting_agent_model(core, settings);
        const std::string name(agents::agent_spec(core).name);
        std::string account;
        if (const auto scan = cachedScan(deps)) {
            if (const auto found = scan->find(core); found != scan->end() && found->second) {
                account = found->second->account;
            }
        }
        const auto detail = account.empty()
                                ? name + std::string(kDot) + "your sign-in"
                                : name + std::string(kDot) + "your " + account + " sign-in";
        return {agents::model_display_name(core, model), detail};
    }
    const auto provider =
        llm::profile_display_name(llm::resolve_meeting_llm_provider(settings), settings);
    return {llm::resolve_meeting_llm_model(settings),
            provider + std::string(kDot) + agent_core_label(core)};
}

bool meeting_intelligence_is_remote(
    const Json& settings, const std::function<bool(const std::string&)>& provider_is_remote) {
    if (meeting_agent::is_installed_core(meeting_agent::resolve_meeting_agent_core(settings))) {
        return true;
    }
    return provider_is_remote(llm::resolve_meeting_llm_provider(settings));
}

std::string meeting_intelligence_home_line(const Json& settings, const AiCopyDeps& deps) {
    std::string line;
    try {
        const auto [value, detail] = meeting_intelligence_overview(settings, deps);
        for (const auto& part : {value, detail}) {
            if (!part.empty()) {
                line += (line.empty() ? "" : std::string(kDot)) + part;
            }
        }
    } catch (const std::exception& error) {
        kLog.warning("Could not read the meeting intelligence overview: {}", error.what());
    }
    return line + ". Turn it on for each meeting when you start it.";
}

void MeetingConsentGrants::grant(const meeting_agent::MeetingAgentRoute& route) {
    std::lock_guard lock(mutex_);
    if (std::ranges::find(routes_, route) == routes_.end()) {
        routes_.push_back(route);
    }
}

bool MeetingConsentGrants::allows(const meeting_agent::MeetingAgentRoute& route) const {
    std::lock_guard lock(mutex_);
    return std::ranges::find(routes_, route) != routes_.end();
}

} // namespace fv
