#include "llm/detail/py_repr.h"

#include "pyport/number.h"
#include "pyport/text.h"

namespace fv::llm::detail {

std::string py_repr(const Json& value) {
    switch (value.type()) {
    case Json::value_t::null:
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

std::string_view py_type_name(const Json& value) {
    switch (value.type()) {
    case Json::value_t::object:
        return "dict";
    case Json::value_t::array:
        return "list";
    case Json::value_t::string:
        return "str";
    case Json::value_t::boolean:
        return "bool";
    case Json::value_t::number_integer:
    case Json::value_t::number_unsigned:
        return "int";
    case Json::value_t::number_float:
        return "float";
    default:
        return "NoneType";
    }
}

} // namespace fv::llm::detail
