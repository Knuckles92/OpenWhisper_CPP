#include "agentapi/detail/query_params.h"

#include <chrono>
#include <cmath>
#include <cstdlib>
#include <limits>

#include "pyport/errors.h"
#include "pyport/number.h"
#include "pyport/text.h"

namespace fv::agentapi::detail {

namespace {

bool is_digit(char c) {
    return c >= '0' && c <= '9';
}

bool ascii_only(std::string_view text) {
    for (const char c : text) {
        if (static_cast<unsigned char>(c) >= 0x80) {
            return false;
        }
    }
    return true;
}

std::string lower_ascii(std::string_view text) {
    std::string out(text);
    for (char& c : out) {
        if (c >= 'A' && c <= 'Z') {
            c = static_cast<char>(c - 'A' + 'a');
        }
    }
    return out;
}

// `count` ASCII digits at `pos`, as a number.
std::optional<int> digits(std::string_view text, std::size_t pos, std::size_t count) {
    if (pos + count > text.size()) {
        return std::nullopt;
    }
    int value = 0;
    for (std::size_t i = pos; i < pos + count; ++i) {
        if (!is_digit(text[i])) {
            return std::nullopt;
        }
        value = value * 10 + (text[i] - '0');
    }
    return value;
}

constexpr std::int64_t kMicrosPerDay = 86'400'000'000LL;

std::int64_t days_from_civil(int year, int month, int day) {
    using namespace std::chrono;
    const sys_days days{std::chrono::year{year} / static_cast<unsigned>(month) /
                        static_cast<unsigned>(day)};
    return days.time_since_epoch().count();
}

// Microseconds since 1970-01-01T00:00:00 of the wall time `value` shows.
std::int64_t wall_micros(const py::DateTime& value) {
    const std::int64_t seconds = static_cast<std::int64_t>(value.hour) * 3600 +
                                 static_cast<std::int64_t>(value.minute) * 60 + value.second;
    return days_from_civil(value.year, value.month, value.day) * kMicrosPerDay +
           seconds * 1'000'000 + value.microsecond;
}

std::optional<py::DateTime> from_micros(std::int64_t micros) {
    using namespace std::chrono;
    std::int64_t days = micros / kMicrosPerDay;
    std::int64_t rest = micros % kMicrosPerDay;
    if (rest < 0) {
        rest += kMicrosPerDay;
        --days;
    }
    const year_month_day date{
        sys_days{std::chrono::days{static_cast<std::chrono::days::rep>(days)}}};
    const int year = static_cast<int>(date.year());
    if (year < 1 || year > 9999) {
        return std::nullopt;
    }
    py::DateTime out;
    out.year = year;
    out.month = static_cast<int>(static_cast<unsigned>(date.month()));
    out.day = static_cast<int>(static_cast<unsigned>(date.day()));
    out.hour = static_cast<int>(rest / 3'600'000'000LL);
    out.minute = static_cast<int>(rest / 60'000'000 % 60);
    out.second = static_cast<int>(rest / 1'000'000 % 60);
    out.microsecond = static_cast<int>(rest % 1'000'000);
    return out;
}

int days_in_month(int year, int month) {
    static constexpr int kDays[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    const bool leap = (year % 4 == 0 && year % 100 != 0) || year % 400 == 0;
    return month == 2 && leap ? 29 : kDays[month - 1];
}

// A Unix time: [+-]digits[.digits][e[+-]digits], the exponent only after a fraction point.
std::optional<py::DateTime> parse_timestamp(std::string_view text) {
    std::size_t i = 0;
    if (i < text.size() && (text[i] == '+' || text[i] == '-')) {
        ++i;
    }
    bool any_digit = false;
    bool point = false;
    while (i < text.size() && is_digit(text[i])) {
        ++i;
        any_digit = true;
    }
    if (i < text.size() && text[i] == '.') {
        point = true;
        ++i;
        while (i < text.size() && is_digit(text[i])) {
            ++i;
            any_digit = true;
        }
    }
    if (point && any_digit && i < text.size() && (text[i] == 'e' || text[i] == 'E')) {
        ++i;
        if (i < text.size() && (text[i] == '+' || text[i] == '-')) {
            ++i;
        }
        const std::size_t start = i;
        while (i < text.size() && is_digit(text[i])) {
            ++i;
        }
        if (i == start) {
            return std::nullopt;
        }
    }
    if (!any_digit || i != text.size()) {
        return std::nullopt;
    }
    std::string number(text);
    if (number.front() == '+') {
        number.erase(0, 1);
    }
    double seconds = std::strtod(number.c_str(), nullptr);
    // Beyond 2e10 (the year 2603 in seconds) the number is milliseconds.
    if (std::fabs(seconds) > 2e10) {
        seconds /= 1000.0;
    }
    if (!std::isfinite(seconds) || std::fabs(seconds) > 1e12) {
        return std::nullopt;
    }
    std::optional<py::DateTime> out =
        from_micros(static_cast<std::int64_t>(std::llround(seconds * 1e6)));
    if (out) {
        out->utc_offset_us = 0;
    }
    return out;
}

} // namespace

std::optional<std::string> last_value(const net::Query& query, std::string_view name) {
    std::optional<std::string> found;
    for (const auto& [key, value] : query) {
        if (key == name) {
            found = value;
        }
    }
    return found;
}

std::optional<bool> parse_bool(std::string_view text) {
    const std::string value = lower_ascii(text);
    for (const std::string_view yes : {"true", "1", "yes", "on", "t", "y"}) {
        if (value == yes) {
            return true;
        }
    }
    for (const std::string_view no : {"false", "0", "no", "off", "f", "n"}) {
        if (value == no) {
            return false;
        }
    }
    return std::nullopt;
}

std::optional<std::int64_t> parse_int(std::string_view text) {
    const std::string stripped = py::strip(text);
    std::string_view rest = stripped;
    std::string number;
    if (!rest.empty() && (rest.front() == '+' || rest.front() == '-')) {
        number += rest.front();
        rest.remove_prefix(1);
    }
    bool digit_before = false;
    std::size_t i = 0;
    for (; i < rest.size() && rest[i] != '.'; ++i) {
        if (is_digit(rest[i])) {
            number += rest[i];
            digit_before = true;
        } else if (rest[i] == '_' && digit_before && i + 1 < rest.size() && is_digit(rest[i + 1])) {
            digit_before = false;
        } else {
            return std::nullopt;
        }
    }
    if (number.empty() || number.back() < '0' || number.back() > '9') {
        return std::nullopt;
    }
    if (i < rest.size()) {
        // A fraction of zeros: "20.0" and "20.00", but not "20.".
        if (i + 1 == rest.size()) {
            return std::nullopt;
        }
        for (std::size_t k = i + 1; k < rest.size(); ++k) {
            if (rest[k] != '0') {
                return std::nullopt;
            }
        }
    }
    return py::py_int(number);
}

std::optional<double> parse_float(std::string_view text) {
    const std::string stripped = py::strip(text);
    if (stripped.empty() || !ascii_only(stripped)) {
        return std::nullopt;
    }
    return py::py_float(stripped);
}

std::optional<py::DateTime> parse_datetime(std::string_view text) {
    if (text.size() < 10 || text[4] != '-') {
        return parse_timestamp(text);
    }
    const auto year = digits(text, 0, 4);
    const auto month = digits(text, 5, 2);
    const auto day = digits(text, 8, 2);
    if (!year || !month || !day || text[7] != '-' || *year < 1 || *month < 1 || *month > 12 ||
        *day < 1 || *day > days_in_month(*year, *month)) {
        return std::nullopt;
    }
    py::DateTime out;
    out.year = *year;
    out.month = *month;
    out.day = *day;
    if (text.size() == 10) {
        return out;
    }
    const char separator = text[10];
    if (separator != 'T' && separator != 't' && separator != ' ' && separator != '_') {
        return std::nullopt;
    }
    std::size_t pos = 11;
    const auto hour = digits(text, pos, 2);
    if (!hour || pos + 2 >= text.size() || text[pos + 2] != ':') {
        return std::nullopt;
    }
    const auto minute = digits(text, pos + 3, 2);
    if (!minute || *hour > 23 || *minute > 59) {
        return std::nullopt;
    }
    out.hour = *hour;
    out.minute = *minute;
    pos += 5;
    if (pos < text.size() && text[pos] == ':') {
        const auto second = digits(text, pos + 1, 2);
        if (!second || *second > 59) {
            return std::nullopt;
        }
        out.second = *second;
        pos += 3;
        if (pos < text.size() && (text[pos] == '.' || text[pos] == ',')) {
            ++pos;
            const std::size_t start = pos;
            int micro = 0;
            while (pos < text.size() && is_digit(text[pos])) {
                if (pos - start < 6) {
                    micro = micro * 10 + (text[pos] - '0');
                }
                ++pos;
            }
            if (pos == start) {
                return std::nullopt;
            }
            for (std::size_t k = pos - start; k < 6; ++k) {
                micro *= 10;
            }
            out.microsecond = micro;
        }
    }
    if (pos == text.size()) {
        return out;
    }
    if ((text[pos] == 'Z' || text[pos] == 'z') && pos + 1 == text.size()) {
        out.utc_offset_us = 0;
        return out;
    }
    if (text[pos] != '+' && text[pos] != '-') {
        return std::nullopt;
    }
    const int sign = text[pos] == '-' ? -1 : 1;
    const auto offset_hours = digits(text, pos + 1, 2);
    std::size_t minute_pos = pos + 3;
    if (minute_pos < text.size() && text[minute_pos] == ':') {
        ++minute_pos;
    }
    const auto offset_minutes = digits(text, minute_pos, 2);
    if (!offset_hours || !offset_minutes || minute_pos + 2 != text.size() || *offset_hours > 23 ||
        *offset_minutes > 59) {
        return std::nullopt;
    }
    out.utc_offset_us =
        sign * (static_cast<std::int64_t>(*offset_hours) * 3600 + *offset_minutes * 60) * 1'000'000;
    return out;
}

py::DateTime to_utc(const py::DateTime& value) {
    const std::optional<py::DateTime> utc =
        from_micros(wall_micros(value) - value.utc_offset_us.value_or(0));
    if (!utc) {
        throw py::OverflowError("date value out of range");
    }
    py::DateTime out = *utc;
    out.utc_offset_us = 0;
    return out;
}

} // namespace fv::agentapi::detail
