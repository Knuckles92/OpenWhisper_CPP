#include "meeting_web/cursors.h"

#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

#include "base/base64.h"
#include "meeting_web/detail/py_value.h"
#include "meeting_web/detail/text_codecs.h"
#include "pyport/errors.h"
#include "pyport/json.h"
#include "pyport/number.h"
#include "pyport/text.h"

namespace fv::meeting_web {

namespace {

py::DumpOptions compact_ascii() {
    py::DumpOptions options;
    options.ensure_ascii = true;
    options.item_sep = ",";
    options.key_sep = ":";
    return options;
}

std::string urlsafe_b64(std::string_view text) {
    return b64encode(text, /*urlsafe=*/true, /*pad=*/false);
}

// "=" * (-len(cursor) % 4), with len() in code points.
std::string padded(std::string_view cursor) {
    const std::size_t length = py::len(cursor);
    return std::string(cursor) + std::string((4 - length % 4) % 4, '=');
}

// float(value) over a JSON value, with Python's exception types.
double to_float(const Json& value) {
    if (value.is_null() || value.is_array() || value.is_object()) {
        throw py::TypeError("float() argument must be a string or a real number, not '" +
                            std::string(value.is_null()    ? "NoneType"
                                        : value.is_array() ? "list"
                                                           : "dict") +
                            "'");
    }
    if (value.is_string()) {
        const auto& text = value.get_ref<const std::string&>();
        if (const auto parsed = py::py_float(std::string_view(text))) {
            return *parsed;
        }
        throw py::ValueError("could not convert string to float: " + py::repr(text));
    }
    if (const auto parsed = py::py_float(value)) {
        return *parsed;
    }
    throw py::TypeError("float() argument must be a string or a real number");
}

// Whether the first item of a JSON list text is an integer literal: json.loads keeps it a
// Python int, and float() of an int beyond the double range raises OverflowError where the
// parsed C++ value is already infinite.
bool first_item_is_integer_literal(std::string_view text) {
    auto skip_space = [&](std::size_t at) {
        while (at < text.size() &&
               (text[at] == ' ' || text[at] == '\t' || text[at] == '\n' || text[at] == '\r')) {
            ++at;
        }
        return at;
    };
    std::size_t at = skip_space(0);
    if (at >= text.size() || text[at] != '[') {
        return false;
    }
    at = skip_space(at + 1);
    if (at < text.size() && text[at] == '-') {
        ++at;
    }
    const std::size_t digits = at;
    while (at < text.size() && text[at] >= '0' && text[at] <= '9') {
        ++at;
    }
    if (at == digits) {
        return false;
    }
    return at >= text.size() || (text[at] != '.' && text[at] != 'e' && text[at] != 'E');
}

// Python's `a, b = value` over a parsed JSON value.
std::optional<std::pair<Json, Json>> unpack_two(const Json& value) {
    if (value.is_array()) {
        if (value.size() != 2) {
            return std::nullopt;
        }
        return std::pair<Json, Json>(value[0], value[1]);
    }
    if (value.is_object()) {
        if (value.size() != 2) {
            return std::nullopt;
        }
        auto it = value.items().begin();
        Json first = it.key();
        ++it;
        Json second = it.key();
        return std::pair<Json, Json>(std::move(first), std::move(second));
    }
    if (value.is_string()) {
        const auto& text = value.get_ref<const std::string&>();
        if (py::len(text) != 2) {
            return std::nullopt;
        }
        return std::pair<Json, Json>(py::slice(text, 0, 1), py::slice(text, 1, 2));
    }
    return std::nullopt;
}

Json parse_json_text(const std::string& text) {
    try {
        return py::loads(text);
    } catch (const py::PyError&) {
        throw;
    } catch (const std::exception& error) {
        // Anything the parser reports outside Python's exception types is still a bad cursor.
        throw py::ValueError(error.what());
    }
}

} // namespace

std::string encode_cursor(const Json& segment) {
    if (!segment.is_object()) {
        throw py::TypeError("segment is not a mapping");
    }
    const auto start = segment.find("start_s");
    const double start_s = start == segment.end() ? 0.0 : to_float(*start);
    const auto id = segment.find("id");
    const std::string segment_id = id == segment.end() ? std::string() : detail::py_str(*id);
    const Json pair = Json::array({start_s, segment_id});
    return urlsafe_b64(py::dumps(pair, compact_ascii()));
}

std::optional<TranscriptCursor> decode_cursor(std::string_view cursor) {
    if (cursor.empty()) {
        return std::nullopt;
    }
    try {
        const auto bytes = detail::b64decode_lenient(padded(cursor));
        if (!bytes || !detail::valid_utf8(*bytes)) {
            throw py::ValueError("bad base64 or UTF-8");
        }
        const Json parsed = parse_json_text(*bytes);
        const auto pair = unpack_two(parsed);
        if (!pair) {
            throw py::ValueError("cannot unpack");
        }
        const double start_s = to_float(pair->first);
        if (std::isinf(start_s) && pair->first.is_number_float() &&
            first_item_is_integer_literal(*bytes)) {
            throw py::OverflowError("int too large to convert to float");
        }
        return TranscriptCursor{start_s, detail::py_str(pair->second)};
    } catch (const py::PyError&) {
        throw CursorError(std::string(kInvalidTranscriptCursor));
    }
}

std::string history_cursor(const Json& meeting) {
    if (!meeting.is_object() || !meeting.contains("started_at")) {
        throw py::PyError("KeyError", "'started_at'");
    }
    if (!meeting.contains("id")) {
        throw py::PyError("KeyError", "'id'");
    }
    const Json pair = Json::array({meeting.at("started_at"), meeting.at("id")});
    return urlsafe_b64(py::dumps(pair, compact_ascii()));
}

std::optional<HistoryCursor> decode_history_cursor(std::string_view cursor) {
    if (cursor.empty()) {
        return std::nullopt;
    }
    try {
        if (py::len(cursor) > kHistoryCursorMaxChars) {
            throw py::ValueError("cursor too long");
        }
        const auto bytes = detail::b64decode_lenient(padded(cursor));
        if (!bytes) {
            throw py::ValueError("bad base64");
        }
        // json.loads of bytes: the encoding is detected, then decoded with surrogatepass.
        const auto text = detail::json_bytes_to_text(*bytes);
        if (!text) {
            throw py::ValueError("bad text encoding");
        }
        const Json values = parse_json_text(*text);
        if (!values.is_array() || values.size() != 2) {
            throw py::ValueError("invalid cursor shape");
        }
        if (!values[0].is_string() || !values[1].is_string()) {
            throw py::ValueError("invalid cursor fields");
        }
        return HistoryCursor{values[0].get<std::string>(), values[1].get<std::string>()};
    } catch (const py::PyError&) {
        throw CursorError(std::string(kInvalidMeetingCursor));
    }
}

} // namespace fv::meeting_web
