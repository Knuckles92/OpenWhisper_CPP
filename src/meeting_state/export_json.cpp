#include "meeting_state/export_json.h"

#include <algorithm>

#include "pyport/json.h"

namespace fv::meeting_state {

Json export_json_payload(const Json& meeting, const Json& state,
                         const std::vector<Json>& segments) {
    Json safe = Json::object();
    if (meeting.is_object()) {
        for (const auto& [key, value] : meeting.items()) {
            if (std::find(VOLATILE_MEETING_FIELDS.begin(), VOLATILE_MEETING_FIELDS.end(), key) ==
                VOLATILE_MEETING_FIELDS.end()) {
                safe[key] = value;
            }
        }
    }
    Json listed = Json::array();
    for (const Json& segment : segments) {
        listed.push_back(segment);
    }
    Json payload = Json::object();
    payload["format_version"] = EXPORT_FORMAT_VERSION;
    payload["meeting"] = std::move(safe);
    payload["state"] = state;
    payload["segments"] = std::move(listed);
    return payload;
}

std::string export_json(const Json& meeting, const Json& state, const std::vector<Json>& segments) {
    py::DumpOptions options;
    options.indent = 2;
    options.ensure_ascii = false;
    return py::dumps(export_json_payload(meeting, state, segments), options);
}

} // namespace fv::meeting_state
