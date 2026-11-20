#include "meeting/engine_options.h"

#include "meeting/detail/py_value.h"
#include "meeting_state/review_ops.h"

namespace fv::meeting {

Json review_config(const Json& config, bool cloud_enabled) {
    const Json& consent_value = detail::get(config, "consent");
    const std::string consent =
        consent_value.is_null() ? std::string() : detail::py_str(consent_value);
    const Json& sensitivity_value = detail::get(config, "sensitivity");
    const Json sensitivity =
        config.is_object() && config.contains("sensitivity") ? sensitivity_value : Json("normal");
    const bool enabled =
        detail::truthy(detail::get(config, "enabled")) && consent == meeting_state::REVIEW_CONSENT;
    std::string status;
    if (!enabled) {
        status = "disabled";
    } else if (cloud_enabled) {
        status = "pending";
    } else {
        status = "unavailable";
    }
    Json out = Json::object();
    out["enabled"] = enabled;
    out["consent"] = config.is_object() && config.contains("consent") ? consent_value : Json("");
    out["sensitivity"] = sensitivity;
    out["status"] = status;
    out["questions"] = Json::array();
    out["message"] = cloud_enabled ? "" : "AI insights are off for this meeting.";
    return out;
}

} // namespace fv::meeting
