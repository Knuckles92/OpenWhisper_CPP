#pragma once
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "agents/installed_agents.h"
#include "base/json.h"
#include "llm/profiles.h"
#include "meeting_agent/route.h"

namespace fv {

struct MeetingConsentCopy {
    std::string destination;
    std::string detail;
    bool remote = true;
    // The installed agent that runs AI insights ("" for the built-in engine), named in the body
    // instead of the endpoint.
    std::string agent_id;
    // The backup agent that may take over ("" when none is chosen).
    std::string backup_name;
};

// The copy for `route` alone: the destination, whether it is remote, and the installed agent.
MeetingConsentCopy meetingConsentCopy(const meeting_agent::MeetingAgentRoute&);
// The same, with the backup agent read from `settings` (_backup_agent_name). OpenWhisper also
// names a Fast judgments judge elsewhere (_fast_judgments_elsewhere); OpenWhisper_CPP's meetings
// use only a judge on this computer, which sends nothing, so there is none to name.
MeetingConsentCopy meetingConsentCopy(const meeting_agent::MeetingAgentRoute&,
                                      const Json& settings);
std::string meetingIntelligenceSummary(const Json&);

// What the AI insights lines read; empty functions use the app's own: the meeting text profile
// (resolve_meeting_llm_profile; nullopt when it cannot be resolved) and the installed agents'
// cached scan (never a scan, so the idle tab never waits on an agent's command line).
struct AiCopyDeps {
    std::function<std::optional<llm::TextLLMProfile>(const Json&)> meeting_profile;
    std::function<std::optional<agents::ScanResult>()> cached_scan;
};

// ai_insights_tooltip: the AI insights switch's hover hint.
std::string ai_insights_tooltip();
// ai_insights_destination: (where, privacy) under the AI insights switch, e.g.
// ("OpenRouter · deepseek-v4.1-flash", "sends transcript text, never audio").
std::pair<std::string, std::string> ai_insights_destination(const Json& settings,
                                                            const AiCopyDeps& deps = {});
// installed_agent_destination: the same line for an installed coding agent, from the cached scan.
std::pair<std::string, std::string> installed_agent_destination(std::string_view agent_id,
                                                                const Json& settings,
                                                                const AiCopyDeps& deps = {});
// meeting_text_summary: who runs AI insights, as the Settings rail shows it.
std::string meeting_text_summary(const Json& settings, const AiCopyDeps& deps = {});
// meeting_intelligence_overview: (value, detail) for Home's AI insights row.
std::pair<std::string, std::string> meeting_intelligence_overview(const Json& settings,
                                                                  const AiCopyDeps& deps = {});
// meeting_intelligence_is_remote: whether AI insights send transcript text off this computer.
// An installed agent sends it to whichever provider it signs in to, which the app cannot see,
// so it counts as leaving; otherwise `provider_is_remote` judges the meeting text provider.
bool meeting_intelligence_is_remote(
    const Json& settings, const std::function<bool(const std::string&)>& provider_is_remote);
// Home's AI insights line: the overview's parts joined, then the per-meeting reminder.
std::string meeting_intelligence_home_line(const Json& settings, const AiCopyDeps& deps = {});
// agent_core_label: the short name of a meeting agent core; the built-in core is the product.
std::string agent_core_label(std::string_view core);

// A grant is tied to the named model and its resolved destination. The saved boolean records
// that the dialog has been accepted before; it never starts another meeting on its own.
class MeetingConsentGrants {
public:
    void grant(const meeting_agent::MeetingAgentRoute&);
    bool allows(const meeting_agent::MeetingAgentRoute&) const;

private:
    mutable std::mutex mutex_;
    std::vector<meeting_agent::MeetingAgentRoute> routes_;
};

} // namespace fv
