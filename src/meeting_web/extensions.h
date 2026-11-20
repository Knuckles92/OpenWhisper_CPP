#pragma once

#include <string>
#include <string_view>

#include "base/json.h"
#include "meeting_web/dashboard_api.h"

// The dashboard routes that need the meeting intelligence (M8): insight review, tailored report
// requests, note requests, the AI insights switch and the semantic ranking of history search.
// FioriVoce without them (M7) registers no extensions, and the routes answer:
//   POST /api/meetings/{id}/review, POST /api/meetings/{id}/reports and
//   POST /api/meeting/notes/request   409 "Turn on AI insights in an active meeting first."
//                                      (the archive dashboard's wording), after their own
//                                      validation (400s)
//   POST /api/meeting/cloud            enabling: 403, the desktop consent sentence (no consent
//                                      dialog exists yet); disabling: the engine's
//                                      set_cloud_enabled(false) and {ok: true, enabled: false}
//   GET /api/search?mode=semantic      the keyword results (search_history's keyword branch)
//                                      with its "enable Semantic history search" message, or
//                                      "Semantic ranking unavailable. Showing keyword results."
//                                      when the setting is on
// Each method runs on the API's io executor after the route's checks named below; it answers
// with a DashboardResponse or throws HttpError. The context (stores, claims, saved rows) is the
// DashboardApi's public surface.
// Affinity: implementations are thread-safe.
namespace fv::meeting_web {

class DashboardExtensions {
public:
    virtual ~DashboardExtensions() = default;

    // After the host check, the JSON body and the op check ("invalid review action").
    virtual DashboardResponse review(DashboardApi& api, const std::string& meeting_id,
                                     const Json& body) = 0;
    // After the host check, the JSON body and the request text check.
    virtual DashboardResponse request_report(DashboardApi& api, const std::string& meeting_id,
                                             const std::string& request) = 0;
    // After the token check (host or guest), the JSON body and the text check.
    virtual DashboardResponse request_note(DashboardApi& api, const std::string& role,
                                           const std::string& text) = 0;
    // After the host check and the JSON body; `enabled` is bool(body.get("enabled")).
    virtual DashboardResponse set_cloud(DashboardApi& api, bool enabled) = 0;
    // GET /api/search?mode=semantic after the host check: {results, mode, message}.
    virtual Json semantic_search(DashboardApi& api, std::string_view query) = 0;
};

} // namespace fv::meeting_web
