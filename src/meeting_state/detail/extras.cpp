#include "meeting_state/detail/extras.h"

#include "meeting_state/detail/py_value.h"
#include "meeting_state/detail/time.h"
#include "meeting_state/time_utils.h"
#include "pyport/errors.h"
#include "pyport/unicode.h"

namespace fv::meeting_state::detail {

namespace {

bool line_break(char32_t cp) {
    switch (cp) {
    case U'\n':
    case U'\r':
    case U'\v':
    case U'\f':
    case U'\x1c':
    case U'\x1d':
    case U'\x1e':
    case U'\x85':
    case U'\u2028':
    case U'\u2029':
        return true;
    default:
        return false;
    }
}

} // namespace

std::string text_or_empty(const Json& value) {
    if (value.is_string()) {
        return value.get<std::string>();
    }
    if (!truthy(value)) {
        return "";
    }
    throw py::PyError("AttributeError", "object has no attribute 'strip'");
}

std::string replace_all(std::string text, std::string_view from, std::string_view to) {
    if (from.empty()) {
        return text;
    }
    std::size_t at = 0;
    while ((at = text.find(from, at)) != std::string::npos) {
        text.replace(at, from.size(), to);
        at += to.size();
    }
    return text;
}

std::vector<std::string> splitlines(std::string_view text) {
    std::vector<std::string> lines;
    std::string current;
    std::size_t pos = 0;
    bool open = false;
    while (pos < text.size()) {
        const std::size_t start = pos;
        const char32_t cp = py::next_code_point(text, pos);
        if (!line_break(cp)) {
            current.append(text.substr(start, pos - start));
            open = true;
            continue;
        }
        if (cp == U'\r' && pos < text.size() && text[pos] == '\n') {
            ++pos;
        }
        lines.push_back(std::move(current));
        current.clear();
        open = false;
    }
    if (open) {
        lines.push_back(std::move(current));
    }
    return lines;
}

std::string join(const std::vector<std::string>& parts, std::string_view separator) {
    std::string out;
    for (std::size_t i = 0; i < parts.size(); ++i) {
        if (i > 0) {
            out += separator;
        }
        out += parts[i];
    }
    return out;
}

const Json& list_member(const Json& object, std::string_view key) {
    static const Json empty = Json::array();
    if (!has(object, key)) {
        return empty;
    }
    const Json& value = get(object, key);
    if (!value.is_array()) {
        throw py::TypeError("object is not iterable");
    }
    return value;
}

const Json& mapping(const Json& value) {
    if (!value.is_object()) {
        throw py::PyError("AttributeError", "object has no attribute 'get'");
    }
    return value;
}

Json& mapping(Json& value) {
    if (!value.is_object()) {
        throw py::PyError("AttributeError", "object has no attribute 'get'");
    }
    return value;
}

bool is_real_number(const Json& value) {
    return value.is_number();
}

std::optional<double> meeting_timestamp(const Json& value, const std::chrono::time_zone& zone) {
    const auto parsed = parse_meeting_time(value);
    if (!parsed) {
        return std::nullopt;
    }
    const Micros instant =
        parsed->utc_offset_us ? utc_instant(*parsed) : local_instant(*parsed, zone);
    return static_cast<double>(instant.time_since_epoch().count()) / 1e6;
}

} // namespace fv::meeting_state::detail
