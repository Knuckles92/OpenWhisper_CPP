#include "meeting_agent/route.h"

#include <functional>

#include "llm/cleanup_settings.h"
#include "llm/local_servers.h"
#include "llm/profiles.h"
#include "meeting_agent/agent_settings.h"
#include "meeting_agent/detail/py_value.h"

namespace fv::meeting_agent {

namespace {

MeetingAgentRoute resolve(const Json& settings, const std::optional<Json>& meeting_row,
                          const std::function<Json()>& new_meeting_endpoint) {
    static const Json kEmpty = Json::object();
    // `meeting or {}`: a missing or empty row is a new meeting.
    const bool has_meeting = meeting_row && detail::py_truthy(*meeting_row);
    const Json& meeting = has_meeting ? *meeting_row : kEmpty;
    const std::string recorded = detail::str_or_empty(detail::get(meeting, "agent_provider"));
    if (is_installed_core(recorded)) {
        return {recorded, recorded, detail::str_or_empty(detail::get(meeting, "agent_model")),
                Json()};
    }

    const std::string kind = resolve_meeting_agent_core(settings);
    if (is_installed_core(kind)) {
        return {kind, kind, resolve_meeting_agent_model(kind, settings), Json()};
    }

    if (!has_meeting) {
        return {kind, llm::resolve_meeting_llm_provider(settings),
                llm::resolve_meeting_llm_model(settings), new_meeting_endpoint()};
    }
    const std::string provider =
        recorded.empty() ? llm::resolve_meeting_llm_provider(settings) : recorded;
    std::string model = detail::str_or_empty(detail::get(meeting, "agent_model"));
    if (model.empty()) {
        model = llm::resolve_meeting_llm_model(settings);
    }
    return {kind, provider, model,
            llm::snapshot_from_meeting(meeting, settings, provider).to_dict()};
}

} // namespace

bool MeetingAgentRoute::installed() const {
    return is_installed_core(kind);
}

MeetingAgentRoute resolve_meeting_agent_route(const Json& settings,
                                              const std::optional<Json>& meeting) {
    return resolve(settings, meeting,
                   [&settings] { return llm::resolve_meeting_llm_endpoint(settings); });
}

MeetingAgentRoute resolve_meeting_agent_route(const Json& settings,
                                              const std::optional<Json>& meeting,
                                              http::Client& client, llm::KeySources& keys) {
    return resolve(settings, meeting,
                   [&] { return llm::resolve_meeting_llm_endpoint(settings, client, keys); });
}

} // namespace fv::meeting_agent
