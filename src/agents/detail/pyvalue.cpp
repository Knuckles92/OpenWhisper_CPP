#include "agents/detail/pyvalue.h"

#include <cmath>
#include <memory>
#include <utility>

#include "pyport/errors.h"
#include "pyport/number.h"
#include "pyport/text.h"
#include "pyport/unicode.h"

namespace fv::agents::detail {

namespace {

const Json& null_json() {
    static const Json null;
    return null;
}

std::string py_repr(const Json& value) {
    if (value.is_string()) {
        return py::repr(value.get_ref<const std::string&>());
    }
    return py_str(value);
}

std::string type_name(const Json& value) {
    if (value.is_boolean()) {
        return "bool";
    }
    if (value.is_number_float()) {
        return "float";
    }
    return "int";
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

const Json& get(const Json& object, std::string_view key) {
    if (!object.is_object()) {
        return null_json();
    }
    const auto it = object.find(std::string(key));
    return it == object.end() ? null_json() : *it;
}

std::vector<Json> iterate(const Json& value) {
    std::vector<Json> items;
    if (value.is_array()) {
        items.assign(value.begin(), value.end());
    } else if (value.is_object()) {
        for (const auto& [key, item] : value.items()) {
            items.emplace_back(key);
        }
    } else if (value.is_string()) {
        const auto& text = value.get_ref<const std::string&>();
        for (const char32_t cp : py::to_code_points(text)) {
            std::string one;
            py::append_utf8(one, cp);
            items.emplace_back(std::move(one));
        }
    } else if (truthy(value)) {
        throw py::TypeError("'" + type_name(value) + "' object is not iterable");
    }
    return items;
}

bool contains(const Json& value, std::string_view needle) {
    if (value.is_object()) {
        return value.contains(std::string(needle));
    }
    if (value.is_array()) {
        for (const auto& item : value) {
            if (item.is_string() && item.get_ref<const std::string&>() == needle) {
                return true;
            }
        }
        return false;
    }
    if (value.is_string()) {
        return value.get_ref<const std::string&>().find(needle) != std::string::npos;
    }
    throw py::TypeError("argument of type '" +
                        (value.is_null() ? std::string("NoneType") : type_name(value)) +
                        "' is not iterable");
}

bool is_py_int(const Json& value) {
    return value.is_number_integer() || value.is_number_unsigned() || value.is_boolean();
}

std::string decode_utf8_replace(std::string_view bytes) {
    std::string out;
    out.reserve(bytes.size());
    std::size_t i = 0;
    const auto at = [&](std::size_t index) {
        return static_cast<unsigned char>(bytes[index]);
    };
    while (i < bytes.size()) {
        const unsigned char lead = at(i);
        if (lead < 0x80) {
            out += static_cast<char>(lead);
            ++i;
            continue;
        }
        std::size_t length = 0;
        unsigned char low = 0x80;
        unsigned char high = 0xBF;
        if (lead >= 0xC2 && lead <= 0xDF) {
            length = 2;
        } else if (lead == 0xE0) {
            length = 3;
            low = 0xA0;
        } else if ((lead >= 0xE1 && lead <= 0xEC) || lead == 0xEE || lead == 0xEF) {
            length = 3;
        } else if (lead == 0xED) {
            length = 3;
            high = 0x9F;
        } else if (lead == 0xF0) {
            length = 4;
            low = 0x90;
        } else if (lead >= 0xF1 && lead <= 0xF3) {
            length = 4;
        } else if (lead == 0xF4) {
            length = 4;
            high = 0x8F;
        } else {
            out += "\xEF\xBF\xBD";
            ++i;
            continue;
        }
        std::size_t valid = 1;
        while (valid < length && i + valid < bytes.size()) {
            const unsigned char next = at(i + valid);
            const unsigned char min = valid == 1 ? low : 0x80;
            const unsigned char max = valid == 1 ? high : 0xBF;
            if (next < min || next > max) {
                break;
            }
            ++valid;
        }
        if (valid == length) {
            out.append(bytes.substr(i, length));
        } else {
            out += "\xEF\xBF\xBD";
        }
        i += valid;
    }
    return out;
}

bool is_valid_utf8(std::string_view bytes) {
    return decode_utf8_replace(bytes) == bytes;
}

std::string universal_newlines(std::string_view text) {
    std::string out;
    out.reserve(text.size());
    for (std::size_t i = 0; i < text.size(); ++i) {
        if (text[i] == '\r') {
            out += '\n';
            if (i + 1 < text.size() && text[i + 1] == '\n') {
                ++i;
            }
        } else {
            out += text[i];
        }
    }
    return out;
}

std::string text_mode(std::string_view bytes) {
    return universal_newlines(decode_utf8_replace(bytes));
}

std::vector<std::string> splitlines(std::string_view utf8) {
    std::vector<std::string> out;
    std::size_t start = 0;
    std::size_t i = 0;
    while (i < utf8.size()) {
        const auto c = static_cast<unsigned char>(utf8[i]);
        std::size_t width = 0; // bytes of the line break at i
        if (c == '\r') {
            width = i + 1 < utf8.size() && utf8[i + 1] == '\n' ? 2 : 1;
        } else if (c == '\n' || c == 0x0B || c == 0x0C || (c >= 0x1C && c <= 0x1E)) {
            width = 1;
        } else if (c == 0xC2 && i + 1 < utf8.size() &&
                   static_cast<unsigned char>(utf8[i + 1]) == 0x85) {
            width = 2; // U+0085
        } else if (c == 0xE2 && i + 2 < utf8.size() &&
                   static_cast<unsigned char>(utf8[i + 1]) == 0x80 &&
                   (static_cast<unsigned char>(utf8[i + 2]) == 0xA8 ||
                    static_cast<unsigned char>(utf8[i + 2]) == 0xA9)) {
            width = 3; // U+2028, U+2029
        }
        if (width == 0) {
            ++i;
            continue;
        }
        out.emplace_back(utf8.substr(start, i - start));
        i += width;
        start = i;
    }
    if (start < utf8.size()) {
        out.emplace_back(utf8.substr(start));
    }
    return out;
}

ByteSink lines(std::function<void(std::string line)> sink, std::function<void()> on_end) {
    auto buffer = std::make_shared<std::string>();
    return [buffer, sink = std::move(sink),
            on_end = std::move(on_end)](std::span<const char> bytes) {
        if (bytes.empty()) {
            if (!buffer->empty() && sink) {
                // A "\r" held back for a "\n" that never came ends the last line.
                std::string_view rest(*buffer);
                if (rest.ends_with('\r')) {
                    rest.remove_suffix(1);
                }
                sink(decode_utf8_replace(rest));
            }
            buffer->clear();
            if (on_end) {
                on_end();
            }
            return;
        }
        buffer->append(bytes.data(), bytes.size());
        // Universal newlines, as a text-mode pipe reads: "\n", "\r\n" and a lone "\r" each
        // end a line.
        std::size_t start = 0;
        for (;;) {
            const auto end = buffer->find_first_of("\r\n", start);
            if (end == std::string::npos) {
                break;
            }
            std::size_t next = end + 1;
            if ((*buffer)[end] == '\r') {
                if (next == buffer->size()) {
                    break; // the "\n" of a "\r\n" may be in the next chunk
                }
                if ((*buffer)[next] == '\n') {
                    ++next;
                }
            }
            if (sink) {
                sink(decode_utf8_replace(std::string_view(buffer->data() + start, end - start)));
            }
            start = next;
        }
        buffer->erase(0, start);
    };
}

} // namespace fv::agents::detail
