#include "text/detail/prompt_data.h"

#include <stdexcept>

#include "text/data/cleanup_prompts.json.h"

namespace fv::text::detail {

const Json& prompt_data() {
    static const Json data = Json::parse(embedded::text_cleanup_prompts);
    return data;
}

const std::string& prompt_text(std::string_view section, std::string_view name) {
    const Json& entries = prompt_data().at(std::string(section));
    const auto found = entries.find(std::string(name));
    if (found == entries.end() || !found->is_string()) {
        throw std::out_of_range("cleanup_prompts.json has no " + std::string(section) + "." +
                                std::string(name));
    }
    return found->get_ref<const std::string&>();
}

const Json& or_empty(const Json& settings) {
    static const Json empty = Json::object();
    return settings.is_object() ? settings : empty;
}

} // namespace fv::text::detail
