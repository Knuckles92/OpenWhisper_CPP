#include "meeting_state/report_views.h"

#include "settings/keys.h"

namespace fv::meeting_state {

std::vector<std::string> resolve_meeting_report_views(const Json& settings) {
    std::vector<std::string> views;
    if (keys::meeting_report_ribbon.read(settings)) {
        views.emplace_back("ribbon");
    }
    if (keys::meeting_report_brief.read(settings)) {
        views.emplace_back("brief");
    }
    if (keys::meeting_report_signal.read(settings)) {
        views.emplace_back("signal");
    }
    if (views.empty()) {
        views.emplace_back("ribbon");
    }
    return views;
}

} // namespace fv::meeting_state
