#include "remote/link.h"

#include <algorithm>

#include "remote/detail/pyvalue.h"

namespace fv::remote {

std::optional<double> RemoteTiming::network_s() const {
    if (!host_s) {
        return std::nullopt;
    }
    return std::max(0.0, round_trip_s - *host_s);
}

std::optional<std::vector<HostModel>> parse_host_models(const Json& raw) {
    if (!raw.is_array()) {
        return std::nullopt;
    }
    std::vector<HostModel> models;
    for (const auto& entry : raw) {
        if (!entry.is_object()) {
            continue;
        }
        const auto family = entry.find("family");
        const auto model = entry.find("model");
        if (family == entry.end() || model == entry.end() || !family->is_string() ||
            !model->is_string() || family->get_ref<const std::string&>().empty() ||
            model->get_ref<const std::string&>().empty()) {
            continue;
        }
        const auto label = entry.find("label");
        std::string text = label == entry.end() ? std::string() : detail::py_str_or_empty(*label);
        if (text.empty()) {
            text = model->get<std::string>();
        }
        models.push_back(HostModel{family->get<std::string>(), model->get<std::string>(), text});
    }
    return models;
}

} // namespace fv::remote
