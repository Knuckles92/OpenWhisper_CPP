#include "automation/detail/pyvalue.h"

#include <cstdint>
#include <typeinfo>

#include "pyport/errors.h"
#include "pyport/json.h"
#include "pyport/number.h"

namespace fv::automation::detail {

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
    case Json::value_t::number_float:
        return value.get<double>() != 0.0;
    case Json::value_t::string:
        return !value.get_ref<const std::string&>().empty();
    case Json::value_t::array:
    case Json::value_t::object:
        return !value.empty();
    default:
        return true;
    }
}

std::string py_str(const Json& value) {
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
        return value.get<std::string>();
    default:
        return py::dumps(value, {.ensure_ascii = false});
    }
}

bool encodes_as_utf8(std::string_view text) {
    std::size_t i = 0;
    while (i < text.size()) {
        const auto lead = static_cast<unsigned char>(text[i]);
        if (lead < 0x80) {
            ++i;
            continue;
        }
        std::size_t length = 0;
        char32_t cp = 0;
        char32_t minimum = 0;
        if ((lead & 0xe0) == 0xc0) {
            length = 2;
            cp = lead & 0x1f;
            minimum = 0x80;
        } else if ((lead & 0xf0) == 0xe0) {
            length = 3;
            cp = lead & 0x0f;
            minimum = 0x800;
        } else if ((lead & 0xf8) == 0xf0) {
            length = 4;
            cp = lead & 0x07;
            minimum = 0x10000;
        } else {
            return false;
        }
        if (i + length > text.size()) {
            return false;
        }
        for (std::size_t k = 1; k < length; ++k) {
            const auto next = static_cast<unsigned char>(text[i + k]);
            if ((next & 0xc0) != 0x80) {
                return false;
            }
            cp = (cp << 6) | (next & 0x3f);
        }
        if (cp < minimum || cp > 0x10ffff || (cp >= 0xd800 && cp <= 0xdfff)) {
            return false;
        }
        i += length;
    }
    return true;
}

bool encodes_as_utf8(const Json& value) {
    if (value.is_string()) {
        return encodes_as_utf8(std::string_view(value.get_ref<const std::string&>()));
    }
    if (value.is_array()) {
        for (const Json& item : value) {
            if (!encodes_as_utf8(item)) {
                return false;
            }
        }
        return true;
    }
    if (value.is_object()) {
        for (const auto& [key, item] : value.items()) {
            if (!encodes_as_utf8(std::string_view(key)) || !encodes_as_utf8(item)) {
                return false;
            }
        }
    }
    return true;
}

std::string error_type(const std::exception& error) {
    if (const auto* python = dynamic_cast<const py::PyError*>(&error)) {
        return std::string(python->type_name());
    }
    std::string name = typeid(error).name();
    for (std::string_view prefix : {"class ", "struct "}) {
        if (name.starts_with(prefix)) {
            name.erase(0, prefix.size());
        }
    }
    return name;
}

const Json& get(const Json& item, std::string_view key) {
    static const Json kNull;
    if (!item.is_object()) {
        return kNull;
    }
    const auto found = item.find(std::string(key));
    return found == item.end() ? kNull : *found;
}

} // namespace fv::automation::detail
