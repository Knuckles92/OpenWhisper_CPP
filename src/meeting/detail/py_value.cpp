#include "meeting/detail/py_value.h"

#include <string>
#include <utility>

#include "pyport/errors.h"
#include "pyport/number.h"
#include "pyport/text.h"
#include "pyport/unicode.h"

namespace fv::meeting::detail {

namespace {

const Json& null_json() {
    static const Json value;
    return value;
}

std::string py_repr(const Json& value);

std::string scalar_str(const Json& value) {
    if (value.is_null()) {
        return "None";
    }
    if (value.is_boolean()) {
        return value.get<bool>() ? "True" : "False";
    }
    if (value.is_number_unsigned()) {
        return std::to_string(value.get<std::uint64_t>());
    }
    if (value.is_number_integer()) {
        return std::to_string(value.get<std::int64_t>());
    }
    if (value.is_number_float()) {
        return py::float_repr(value.get<double>());
    }
    if (value.is_string()) {
        return value.get<std::string>();
    }
    return py_repr(value);
}

std::string py_repr(const Json& value) {
    if (value.is_string()) {
        return py::repr(value.get_ref<const std::string&>());
    }
    if (value.is_array()) {
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
    if (value.is_object()) {
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
    return scalar_str(value);
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
        // NaN is true in Python.
        return !(value.get<double>() == 0.0);
    case Json::value_t::string:
        return !value.get_ref<const std::string&>().empty();
    case Json::value_t::array:
    case Json::value_t::object:
    case Json::value_t::binary:
        return !value.empty();
    }
    return false;
}

const Json& get(const Json& object, std::string_view key) {
    if (!object.is_object()) {
        return null_json();
    }
    const auto it = object.find(key);
    return it == object.end() ? null_json() : *it;
}

std::string py_str(const Json& value) {
    if (value.is_array() || value.is_object()) {
        return py_repr(value);
    }
    return scalar_str(value);
}

std::string str_or_empty(const Json& value) {
    return truthy(value) ? py_str(value) : std::string();
}

std::int64_t int_or_zero(const Json& value) {
    if (!truthy(value)) {
        return 0;
    }
    const auto parsed = py::py_int_checked(value);
    if (parsed) {
        return *parsed;
    }
    switch (parsed.error()) {
    case py::IntError::Type:
        throw py::TypeError("int() argument must be a string, a bytes-like object or a real "
                            "number, not '" +
                            std::string(value.is_array() ? "list" : "dict") + "'");
    case py::IntError::Overflow:
        throw py::OverflowError("cannot convert float infinity to integer");
    case py::IntError::Value:
        break;
    }
    throw py::ValueError("invalid literal for int() with base 10: " + py_repr(value));
}

Json list_or_empty(const Json& value) {
    if (!truthy(value)) {
        return Json::array();
    }
    if (value.is_array()) {
        return value;
    }
    Json out = Json::array();
    if (value.is_string()) {
        for (const char32_t cp : py::to_code_points(value.get_ref<const std::string&>())) {
            std::string one;
            py::append_utf8(one, cp);
            out.push_back(std::move(one));
        }
        return out;
    }
    if (value.is_object()) {
        for (const auto& [key, item] : value.items()) {
            out.push_back(key);
        }
        return out;
    }
    throw py::TypeError("object is not iterable");
}

} // namespace fv::meeting::detail
