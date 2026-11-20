#pragma once

#include <array>
#include <string>
#include <string_view>
#include <vector>

#include "base/json.h"

// services/settings.py resolve_meeting_report_views: the post-meeting report views the settings
// enable, in display order.
// Affinity: thread-safe (pure function).
namespace fv::meeting_state {

// Every view, in display order, with the setting that enables it.
inline constexpr std::array<std::string_view, 3> REPORT_VIEWS = {"ribbon", "brief", "signal"};

// The views whose meeting_report_<view> setting is on in `settings` (a settings snapshot,
// defaults filling missing keys); ["ribbon"] when every view is off, so a meeting never ends
// with an empty report.
std::vector<std::string> resolve_meeting_report_views(const Json& settings);

} // namespace fv::meeting_state
