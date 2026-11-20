#include "meeting_agent/detail/py_value.h"

#include <cstdint>
#include <string>

#include "pyport/errors.h"
#include "pyport/number.h"
#include "pyport/text.h"
#include "pyport/unicode.h"

namespace fv::meeting_agent::detail {

namespace {

const Json& null_json() {
    static const Json value;
    return value;
}

const Json& empty_object() {
    static const Json value = Json::object();
    return value;
}

std::string container(const Json& value) {
    std::string out;
    if (value.is_array()) {
        out = "[";
        bool first = true;
        for (const auto& item : value) {
            out += first ? "" : ", ";
            out += py_repr(item);
            first = false;
        }
        return out + "]";
    }
    out = "{";
    bool first = true;
    for (const auto& [key, item] : value.items()) {
        out += first ? "" : ", ";
        out += py::repr(key) + ": " + py_repr(item);
        first = false;
    }
    return out + "}";
}

} // namespace

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
    case Json::value_t::array:
    case Json::value_t::object:
        return container(value);
    case Json::value_t::binary:
        break;
    }
    return "None";
}

std::string py_repr(const Json& value) {
    if (value.is_string()) {
        return py::repr(value.get<std::string>());
    }
    return py_str(value);
}

bool py_truthy(const Json& value) {
    switch (value.type()) {
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

Json py_list_or_empty(const Json& value) {
    if (!py_truthy(value)) {
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
    const char* type = value.is_boolean() ? "bool" : value.is_number_float() ? "float" : "int";
    throw py::TypeError(std::string("'") + type + "' object is not iterable");
}

const Json& get(const Json& object, std::string_view key) {
    return get_or(object, key, null_json());
}

const Json& get_or(const Json& object, std::string_view key, const Json& fallback) {
    if (!object.is_object()) {
        return fallback;
    }
    const auto found = object.find(key);
    return found == object.end() ? fallback : *found;
}

const Json& get_object(const Json& object, std::string_view key) {
    const Json& value = get(object, key);
    return value.is_object() ? value : empty_object();
}

std::string str_or_empty(const Json& value) {
    return py_truthy(value) ? py_str(value) : std::string();
}

bool is_text(const Json& value, std::string_view text) {
    return value.is_string() && value.get_ref<const std::string&>() == text;
}

} // namespace fv::meeting_agent::detail
