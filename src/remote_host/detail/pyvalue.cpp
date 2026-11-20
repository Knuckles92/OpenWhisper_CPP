#include "remote_host/detail/pyvalue.h"

#include <cmath>

#include "pyport/errors.h"
#include "pyport/number.h"
#include "pyport/text.h"

namespace fv::remote_host::detail {

namespace {

std::string number_text(const Json& value) {
    if (value.is_number_integer()) {
        return value.is_number_unsigned() ? std::to_string(value.get<std::uint64_t>())
                                          : std::to_string(value.get<std::int64_t>());
    }
    return py::float_repr(value.get<double>());
}

} // namespace

bool truthy(const Json& value) {
    switch (value.type()) {
    case Json::value_t::null:
        return false;
    case Json::value_t::boolean:
        return value.get<bool>();
    case Json::value_t::number_integer:
        return value.get<std::int64_t>() != 0;
    case Json::value_t::number_unsigned:
        return value.get<std::uint64_t>() != 0;
    case Json::value_t::number_float: {
        const double number = value.get<double>();
        return std::isnan(number) || number != 0.0;
    }
    case Json::value_t::string:
        return !value.get_ref<const std::string&>().empty();
    case Json::value_t::array:
    case Json::value_t::object:
        return !value.empty();
    default:
        return true;
    }
}

std::string py_repr(const Json& value) {
    switch (value.type()) {
    case Json::value_t::null:
        return "None";
    case Json::value_t::boolean:
        return value.get<bool>() ? "True" : "False";
    case Json::value_t::number_integer:
    case Json::value_t::number_unsigned:
    case Json::value_t::number_float:
        return number_text(value);
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
    default:
        return "None";
    }
}

std::string py_str(const Json& value) {
    if (value.is_string()) {
        return value.get<std::string>();
    }
    return py_repr(value);
}

bool nonempty_string(const Json& value) {
    return value.is_string() && !value.get_ref<const std::string&>().empty();
}

bool has_other_keys(const Json& header, std::initializer_list<std::string_view> allowed) {
    if (!header.is_object()) {
        return false;
    }
    for (const auto& [key, item] : header.items()) {
        bool known = false;
        for (const auto name : allowed) {
            if (key == name) {
                known = true;
                break;
            }
        }
        if (!known) {
            return true;
        }
    }
    return false;
}

const Json& get(const Json& header, std::string_view key) {
    static const Json null_value;
    if (!header.is_object()) {
        return null_value;
    }
    const auto found = header.find(std::string(key));
    return found == header.end() ? null_value : *found;
}

bool is_text(const Json& value, std::string_view text) {
    return value.is_string() && value.get_ref<const std::string&>() == text;
}

std::string error_text(const std::exception& error) {
    std::string text = error.what();
    if (!text.empty()) {
        return text;
    }
    if (const auto* python = dynamic_cast<const py::PyError*>(&error)) {
        return std::string(python->type_name());
    }
    return "RuntimeError";
}

std::span<const std::byte> bytes_of(std::string_view text) {
    return {reinterpret_cast<const std::byte*>(text.data()), text.size()};
}

std::string_view chars_of(std::span<const std::byte> bytes) {
    return {reinterpret_cast<const char*>(bytes.data()), bytes.size()};
}

} // namespace fv::remote_host::detail
