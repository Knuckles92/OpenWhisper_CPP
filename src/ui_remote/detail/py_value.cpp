#include "ui_remote/detail/py_value.h"

#include <cstdint>
#include <string>

#include "pyport/number.h"
#include "pyport/text.h"

namespace fv::ui_remote::detail {

namespace {

void repr_into(std::string& out, const Json& value) {
    if (value.is_string()) {
        out += py::repr(value.get_ref<const std::string&>());
    } else {
        out += py_str(value);
    }
}

} // namespace

bool truthy(const Json& value) {
    switch (value.type()) {
    case Json::value_t::null:
    case Json::value_t::discarded:
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
    case Json::value_t::binary:
        return !value.empty();
    }
    return false;
}

std::string py_str(const Json& value) {
    switch (value.type()) {
    case Json::value_t::null:
    case Json::value_t::discarded:
        return "None";
    case Json::value_t::boolean:
        return value.get<bool>() ? "True" : "False";
    case Json::value_t::number_integer:
        return std::to_string(value.get<std::int64_t>());
    case Json::value_t::number_unsigned:
        return std::to_string(value.get<std::uint64_t>());
    case Json::value_t::number_float:
        return py::float_repr(value.get<double>());
    case Json::value_t::string:
        return value.get<std::string>();
    case Json::value_t::array: {
        std::string out = "[";
        bool first = true;
        for (const auto& item : value) {
            if (!first) {
                out += ", ";
            }
            first = false;
            repr_into(out, item);
        }
        return out + "]";
    }
    case Json::value_t::object: {
        std::string out = "{";
        bool first = true;
        for (const auto& [key, item] : value.items()) {
            if (!first) {
                out += ", ";
            }
            first = false;
            out += py::repr(key);
            out += ": ";
            repr_into(out, item);
        }
        return out + "}";
    }
    case Json::value_t::binary:
        return "b''";
    }
    return "";
}

std::string py_str_or_empty(const Json& value) {
    return truthy(value) ? py_str(value) : std::string();
}

const Json& get(const Json& value, const char* key) {
    static const Json none = nullptr;
    if (!value.is_object()) {
        return none;
    }
    const auto found = value.find(key);
    return found == value.end() ? none : *found;
}

} // namespace fv::ui_remote::detail
