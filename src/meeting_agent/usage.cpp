#include "meeting_agent/usage.h"

#include <array>
#include <cstdint>
#include <string_view>

namespace fv::meeting_agent {

namespace {

bool is_python_int(const Json& value) {
    return value.is_number_integer() || value.is_boolean();
}

std::int64_t as_int(const Json& value) {
    return value.is_boolean() ? (value.get<bool>() ? 1 : 0) : value.get<std::int64_t>();
}

// total.get(key, 0) + value with Python's int and float arithmetic.
Json plus(const Json& current, const Json& value) {
    if (current.is_number_float()) {
        return Json(current.get<double>() + static_cast<double>(as_int(value)));
    }
    return Json(as_int(current) + as_int(value));
}

} // namespace

void merge_usage(Json& total, const Json& usage) {
    if (usage.is_null()) {
        return;
    }
    if (!total.is_object()) {
        total = Json::object();
    }
    constexpr std::array<std::string_view, 3> kKeys = {"prompt_tokens", "completion_tokens",
                                                       "total_tokens"};
    for (const auto key : kKeys) {
        if (!usage.is_object()) {
            break;
        }
        const auto value = usage.find(key);
        if (value == usage.end() || !is_python_int(*value)) {
            continue;
        }
        const std::string name(key);
        total[name] = plus(total.contains(name) ? total[name] : Json(0), *value);
    }
    total["requests"] = plus(total.contains("requests") ? total["requests"] : Json(0), Json(1));
}

} // namespace fv::meeting_agent
