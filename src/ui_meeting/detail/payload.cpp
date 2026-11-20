#include "ui_meeting/detail/payload.h"

#include <cstdint>
#include <string>

#include <QByteArray>
#include <QJsonDocument>

#include "pyport/errors.h"
#include "pyport/number.h"
#include "pyport/text.h"
#include "pyport/unicode.h"

namespace fv::ui_meeting::detail {

namespace {

const Json& nullJson() {
    static const Json value;
    return value;
}

std::string pyRepr(const Json& value);

std::string scalarStr(const Json& value) {
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
    return pyRepr(value);
}

std::string pyRepr(const Json& value) {
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
            out += pyRepr(item);
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
            out += py::repr(key) + ": " + pyRepr(item);
        }
        return out + "}";
    }
    return scalarStr(value);
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
        return nullJson();
    }
    const auto it = object.find(key);
    return it == object.end() ? nullJson() : *it;
}

std::string pyStr(const Json& value) {
    if (value.is_array() || value.is_object()) {
        return pyRepr(value);
    }
    return scalarStr(value);
}

std::string strOrEmpty(const Json& value) {
    return truthy(value) ? pyStr(value) : std::string();
}

std::int64_t intOrZero(const Json& value) {
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
    throw py::ValueError("invalid literal for int() with base 10: " + pyRepr(value));
}

Json listOrEmpty(const Json& value) {
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
    const char* type = value.is_boolean() ? "bool" : value.is_number_float() ? "float" : "int";
    throw py::TypeError(std::string("'") + type + "' object is not iterable");
}

Json dictOrEmpty(const Json& value) {
    if (value.is_object()) {
        return value;
    }
    if (!truthy(value)) {
        return Json::object();
    }
    // dict(iterable): each element is a (key, value) pair. A number is not iterable ("'int'
    // object is not iterable"); a string iterates as its one-character strings, which are
    // sequences of length 1.
    const Json items = listOrEmpty(value);
    Json out = Json::object();
    std::size_t index = 0;
    for (const Json& item : items) {
        const bool sequence = item.is_array() || item.is_string() || item.is_object();
        const Json pair = sequence ? listOrEmpty(item) : Json();
        const std::string element = "dictionary update sequence element #" + std::to_string(index);
        if (pair.is_null()) {
            throw py::TypeError("cannot convert " + element + " to a sequence");
        }
        if (pair.size() != 2) {
            throw py::ValueError(element + " has length " + std::to_string(pair.size()) +
                                 "; 2 is required");
        }
        // A JSON object keys by text; any other key is written as Python prints it.
        out[pair[0].is_string() ? pair[0].get<std::string>() : pyStr(pair[0])] = pair[1];
        ++index;
    }
    return out;
}

std::optional<double> pythonNumber(const Json& value) {
    if (value.is_boolean()) {
        return value.get<bool>() ? 1.0 : 0.0;
    }
    if (value.is_number()) {
        return value.get<double>();
    }
    return std::nullopt;
}

Json toJson(const QJsonObject& object) {
    const QByteArray text = QJsonDocument(object).toJson(QJsonDocument::Compact);
    return Json::parse(text.constData(), text.constData() + text.size());
}

} // namespace fv::ui_meeting::detail
