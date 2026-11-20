#include "history/detail/py_value.h"

#include <cstdint>

#include "pyport/json.h"
#include "pyport/number.h"

namespace fv::history_detail {

bool truthy(const Json& value) {
    if (value.is_null()) {
        return false;
    }
    if (value.is_boolean()) {
        return value.get<bool>();
    }
    if (value.is_number_float()) {
        return value.get<double>() != 0.0;
    }
    if (value.is_number_unsigned()) {
        return value.get<std::uint64_t>() != 0;
    }
    if (value.is_number_integer()) {
        return value.get<std::int64_t>() != 0;
    }
    if (value.is_string()) {
        return !value.get_ref<const std::string&>().empty();
    }
    return !value.empty();
}

std::string py_str(const Json& value) {
    if (value.is_string()) {
        return value.get<std::string>();
    }
    if (value.is_null()) {
        return "None";
    }
    if (value.is_boolean()) {
        return value.get<bool>() ? "True" : "False";
    }
    if (value.is_number_float()) {
        return py::float_repr(value.get<double>());
    }
    if (value.is_number_unsigned()) {
        return std::to_string(value.get<std::uint64_t>());
    }
    if (value.is_number_integer()) {
        return std::to_string(value.get<std::int64_t>());
    }
    return py::dumps(value, {.ensure_ascii = false});
}

const Json& field(const Json& record, std::string_view key) {
    static const Json none = nullptr;
    if (!record.is_object()) {
        return none;
    }
    const auto found = record.find(key);
    return found == record.end() ? none : *found;
}

std::string text_or(const Json& record, std::string_view key, std::string_view fallback) {
    const Json& value = field(record, key);
    return truthy(value) ? py_str(value) : std::string(fallback);
}

} // namespace fv::history_detail
