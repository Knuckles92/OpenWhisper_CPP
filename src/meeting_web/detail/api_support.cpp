#include "meeting_web/detail/api_support.h"

#include <cmath>
#include <limits>
#include <string>

#include "meeting_web/dashboard_api.h"
#include "pyport/errors.h"
#include "pyport/json.h"

namespace fv::meeting_web::detail {

namespace {

// pydantic-core refuses longer integer strings (Python's int max_str_digits).
constexpr std::size_t kMaxIntDigits = 4300;

bool is_digit(char c) {
    return c >= '0' && c <= '9';
}

std::string ascii_strip(std::string_view text) {
    const auto space = [](char c) {
        return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v';
    };
    std::size_t begin = 0;
    std::size_t end = text.size();
    while (begin < end && space(text[begin])) {
        ++begin;
    }
    while (end > begin && space(text[end - 1])) {
        --end;
    }
    return std::string(text.substr(begin, end - begin));
}

enum class IntOutcome { Ok, Parsing, Size };

// pydantic's lax str -> int: surrounding whitespace, a sign, digits with single underscores
// between them, and a fraction of zeros only ("5.0"). Python's int has no bound; the routes only
// clamp or compare, so a value beyond int64 saturates.
IntOutcome int_from_text(std::string_view raw, std::int64_t& value) {
    const std::string text = ascii_strip(raw);
    std::size_t pos = 0;
    bool negative = false;
    if (pos < text.size() && (text[pos] == '+' || text[pos] == '-')) {
        negative = text[pos] == '-';
        ++pos;
    }
    std::string digits;
    bool last_digit = false;
    for (; pos < text.size() && text[pos] != '.'; ++pos) {
        const char c = text[pos];
        if (is_digit(c)) {
            digits += c;
            last_digit = true;
        } else if (c == '_' && last_digit && pos + 1 < text.size() && is_digit(text[pos + 1])) {
            last_digit = false;
        } else {
            return IntOutcome::Parsing;
        }
    }
    if (digits.empty()) {
        return IntOutcome::Parsing;
    }
    if (pos < text.size()) {
        ++pos;
        if (pos == text.size()) {
            return IntOutcome::Parsing;
        }
        for (; pos < text.size(); ++pos) {
            if (text[pos] != '0') {
                return IntOutcome::Parsing;
            }
        }
    }
    if (digits.size() > kMaxIntDigits) {
        return IntOutcome::Size;
    }
    std::uint64_t magnitude = 0;
    bool overflow = false;
    for (const char c : digits) {
        const auto digit = static_cast<std::uint64_t>(c - '0');
        if (magnitude > (std::numeric_limits<std::uint64_t>::max() - digit) / 10) {
            overflow = true;
            break;
        }
        magnitude = magnitude * 10 + digit;
    }
    constexpr auto kMax = static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max());
    if (negative) {
        value = overflow || magnitude > kMax ? std::numeric_limits<std::int64_t>::min()
                                             : -static_cast<std::int64_t>(magnitude);
    } else {
        value = overflow || magnitude > kMax ? std::numeric_limits<std::int64_t>::max()
                                             : static_cast<std::int64_t>(magnitude);
    }
    return IntOutcome::Ok;
}

void check_finite(const Json& value) {
    if (value.is_number_float()) {
        if (!std::isfinite(value.get<double>())) {
            throw py::ValueError("Out of range float values are not JSON compliant");
        }
    } else if (value.is_structured()) {
        for (const auto& item : value) {
            check_finite(item);
        }
    }
}

} // namespace

std::optional<std::string> QueryReader::last(std::string_view name) const {
    std::optional<std::string> found;
    for (const auto& [key, value] : query_) {
        if (key == name) {
            found = value;
        }
    }
    return found;
}

void QueryReader::fail(std::string_view name, std::string_view type, std::string_view message,
                       const std::string& input) {
    errors_.push_back(Json{{"type", std::string(type)},
                           {"loc", Json::array({"query", std::string(name)})},
                           {"msg", std::string(message)},
                           {"input", input}});
}

std::string QueryReader::text(std::string_view name, std::string_view fallback) {
    return last(name).value_or(std::string(fallback));
}

std::int64_t QueryReader::integer(std::string_view name, std::int64_t fallback) {
    return optional_integer(name).value_or(fallback);
}

std::optional<std::int64_t> QueryReader::optional_integer(std::string_view name) {
    const auto raw = last(name);
    if (!raw) {
        return std::nullopt;
    }
    std::int64_t value = 0;
    switch (int_from_text(*raw, value)) {
    case IntOutcome::Ok:
        return value;
    case IntOutcome::Parsing:
        fail(name, "int_parsing",
             "Input should be a valid integer, unable to parse string as an integer", *raw);
        break;
    case IntOutcome::Size:
        fail(name, "int_parsing_size",
             "Unable to parse input string as an integer, exceeded maximum size", *raw);
        break;
    }
    return std::nullopt;
}

bool QueryReader::boolean(std::string_view name, bool fallback) {
    const auto raw = last(name);
    if (!raw) {
        return fallback;
    }
    std::string lowered = *raw;
    for (char& c : lowered) {
        if (c >= 'A' && c <= 'Z') {
            c = static_cast<char>(c - 'A' + 'a');
        }
    }
    for (const std::string_view yes : {"1", "on", "t", "true", "y", "yes"}) {
        if (lowered == yes) {
            return true;
        }
    }
    for (const std::string_view no : {"0", "off", "f", "false", "n", "no"}) {
        if (lowered == no) {
            return false;
        }
    }
    fail(name, "bool_parsing", "Input should be a valid boolean, unable to interpret input", *raw);
    return fallback;
}

void QueryReader::finish() const {
    if (!errors_.empty()) {
        throw HttpError(422, errors_);
    }
}

std::string path_param(const net::HttpRequest& request, std::string_view name) {
    for (const auto& [key, value] : request.params) {
        if (key == name) {
            return value;
        }
    }
    return {};
}

std::string render_json(const Json& value) {
    check_finite(value);
    py::DumpOptions options;
    options.ensure_ascii = false;
    options.item_sep = ",";
    options.key_sep = ":";
    return py::dumps(value, options);
}

std::string media_type_header(std::string_view media_type) {
    std::string header(media_type);
    if (header.starts_with("text/")) {
        header += "; charset=utf-8";
    }
    return header;
}

} // namespace fv::meeting_web::detail
