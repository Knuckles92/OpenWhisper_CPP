#include "text/detail/cleanup_rules.h"

#include "pyport/text.h"
#include "settings/keys.h"

namespace fv::text::detail {

std::vector<std::string> cleanup_rules(const Json& settings) {
    static const Json empty = Json::object();
    const Json raw = keys::transcript_cleanup_rules.read(settings.is_object() ? settings : empty);
    std::vector<std::string> rules;
    for (const Json& item : raw) {
        if (!item.is_string()) {
            continue;
        }
        std::string rule = py::strip(item.get_ref<const std::string&>());
        if (rule.empty()) {
            continue;
        }
        rules.push_back(std::move(rule));
        if (rules.size() >= kMaxCleanupRules) {
            break;
        }
    }
    return rules;
}

} // namespace fv::text::detail
