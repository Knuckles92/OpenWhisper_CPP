#include "meeting_web/detail/py_value.h"

#include <string>

#include "pyport/number.h"
#include "pyport/text.h"

namespace fv::meeting_web::detail {

namespace {

const Json kNull;

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

std::string py_repr(const Json& value) {
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
        return py::repr(value.get_ref<const std::string&>());
    case Json::value_t::array: {
        std::string out = "[";
        bool first = true;
        for (const auto& item : value) {
            if (!first) {
                out += ", ";
            }
            first = false;
            out += py_repr(item);
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
            out += py::repr(key) + ": " + py_repr(item);
        }
        return out + "}";
    }
    case Json::value_t::binary:
        break;
    }
    return "None";
}

std::string py_str(const Json& value) {
    if (value.is_string()) {
        return value.get<std::string>();
    }
    return py_repr(value);
}

const Json& py_or(const Json& a, const Json& b) {
    return truthy(a) ? a : b;
}

const Json& get(const Json& object, std::string_view key) {
    if (!object.is_object()) {
        return kNull;
    }
    const auto found = object.find(std::string(key));
    return found == object.end() ? kNull : *found;
}

} // namespace fv::meeting_web::detail
