#pragma once

#include <string>
#include <string_view>

#include "base/json.h"

namespace fv::llm::detail {

// Python's bool() of the value json.loads makes of `value`.
inline bool truthy(const Json& value) {
    switch (value.type()) {
    case Json::value_t::null:
        return false;
    case Json::value_t::boolean:
        return value.get<bool>();
    case Json::value_t::number_integer:
        return value.get<std::int64_t>() != 0;
    case Json::value_t::number_unsigned:
        return value.get<std::uint64_t>() != 0;
    case Json::value_t::number_float:
        return value.get<double>() != 0.0;
    case Json::value_t::string:
        return !value.get_ref<const std::string&>().empty();
    case Json::value_t::array:
    case Json::value_t::object:
        return !value.empty();
    default:
        return false;
    }
}

// mapping.get(key): the value, or null when `mapping` is not an object or lacks the key.
inline const Json& get(const Json& mapping, std::string_view key) {
    static const Json null_value;
    if (!mapping.is_object()) {
        return null_value;
    }
    const auto found = mapping.find(key);
    return found == mapping.end() ? null_value : *found;
}

inline bool has(const Json& mapping, std::string_view key) {
    return mapping.is_object() && mapping.find(key) != mapping.end();
}

// A JSON string's text, or nullptr for anything else.
inline const std::string* text(const Json& value) {
    return value.is_string() ? &value.get_ref<const std::string&>() : nullptr;
}

} // namespace fv::llm::detail
