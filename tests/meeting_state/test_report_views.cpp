// services/settings.py resolve_meeting_report_views, as tests/test_settings.py's meeting-settings
// cases call it: the enabled views in display order, ["ribbon"] when every view is off.
#include <catch2/catch_test_macros.hpp>

#include <string>
#include <vector>

#include "meeting_state/report_views.h"

using fv::Json;
using fv::meeting_state::resolve_meeting_report_views;
using Views = std::vector<std::string>;

TEST_CASE("resolve_meeting_report_views keeps display order and never comes back empty",
          "[test_settings]") {
    // Defaults: every view on, ribbon, brief, signal.
    CHECK(resolve_meeting_report_views(Json::object()) == Views{"ribbon", "brief", "signal"});
    // The order is the display order, not the order of the saved keys.
    CHECK(resolve_meeting_report_views(Json{{"meeting_report_signal", true},
                                            {"meeting_report_brief", false},
                                            {"meeting_report_ribbon", true}}) ==
          Views{"ribbon", "signal"});
    CHECK(resolve_meeting_report_views(Json{{"meeting_report_ribbon", false}}) ==
          Views{"brief", "signal"});
    CHECK(resolve_meeting_report_views(Json{{"meeting_report_ribbon", true},
                                            {"meeting_report_brief", false},
                                            {"meeting_report_signal", false}}) == Views{"ribbon"});
    // Every view off: the ribbon, so a meeting never ends with an empty report.
    CHECK(resolve_meeting_report_views(Json{{"meeting_report_ribbon", false},
                                            {"meeting_report_brief", false},
                                            {"meeting_report_signal", false}}) == Views{"ribbon"});
    // A value that is not a boolean reads as the default (on).
    CHECK(resolve_meeting_report_views(Json{{"meeting_report_ribbon", "yes"}}) ==
          Views{"ribbon", "brief", "signal"});
}
