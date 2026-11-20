#include "meeting_agent/guidance.h"

#include "meeting_agent/detail/prompt_text.h"
#include "meeting_agent/detail/py_value.h"
#include "meeting_state/corrections.h"
#include "pyport/errors.h"
#include "pyport/json.h"

namespace fv::meeting_agent {

namespace {

bool is_guidance_kind(const Json& kind) {
    return detail::is_text(kind, "agent_insight") || detail::is_text(kind, "term_correction") ||
           detail::is_text(kind, "occurrence_correction");
}

} // namespace

std::string guidance_prompt(const Json& state) {
    using detail::get;
    const Json& notes = get(detail::get_object(state, "cards"), "user_notes");
    Json listed = Json::array();
    if (notes.is_array()) {
        for (const auto& item : notes) {
            if (!item.is_object() || detail::is_text(get(item, "status"), "removed") ||
                !meeting_state::is_correction_author(item)) {
                continue;
            }
            const Json& data = get(item, "data");
            if (detail::py_truthy(data) && !data.is_object()) {
                // (n.get("data") or {}).get("kind") on a list or a string.
                const char* type = data.is_array()          ? "list"
                                   : data.is_string()       ? "str"
                                   : data.is_boolean()      ? "bool"
                                   : data.is_number_float() ? "float"
                                                            : "int";
                throw py::PyError("AttributeError",
                                  std::string("'") + type + "' object has no attribute 'get'");
            }
            if (!is_guidance_kind(get(detail::get_object(item, "data"), "kind"))) {
                continue;
            }
            listed.push_back(Json{{"text", get(item, "text")},
                                  {"data", get(item, "data")},
                                  {"evidence", get(item, "evidence")}});
        }
    }
    if (listed.empty()) {
        return "";
    }
    py::DumpOptions options;
    options.ensure_ascii = false;
    return std::string(detail::kGuidancePreamble) + py::dumps(listed, options);
}

} // namespace fv::meeting_agent
