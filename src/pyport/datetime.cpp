#include "pyport/datetime.h"

#include <algorithm>
#include <format>

#include "pyport/errors.h"
#include "pyport/text.h"
#include "pyport/unicode.h"

namespace fv::py {
namespace {

// The parsers below follow CPython 3.12's Modules/_datetimemodule.c, which reads a
// NUL-terminated UTF-8 buffer and relies on the terminator to stop; `p` here always points into
// a std::string, whose terminator plays the same part.

bool is_digit(char c) {
    return c >= '0' && c <= '9';
}

const char* parse_digits(const char* p, int* var, std::size_t num_digits) {
    for (std::size_t i = 0; i < num_digits; ++i) {
        const auto d = static_cast<unsigned>(static_cast<unsigned char>(*(p++)) - '0');
        if (d > 9) {
            return nullptr;
        }
        *var = *var * 10 + static_cast<int>(d);
    }
    return p;
}

bool is_leap(int year) {
    return year % 4 == 0 && (year % 100 != 0 || year % 400 == 0);
}

int days_in_month(int year, int month) {
    static constexpr int kDays[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    return month == 2 && is_leap(year) ? 29 : kDays[month - 1];
}

// Proleptic Gregorian ordinal, 0001-01-01 being 1 (date.toordinal).
int ymd_to_ord(int year, int month, int day) {
    const int y = year - 1;
    int days = y * 365 + y / 4 - y / 100 + y / 400;
    for (int m = 1; m < month; ++m) {
        days += days_in_month(year, m);
    }
    return days + day;
}

void ord_to_ymd(int ordinal, int* year, int* month, int* day) {
    int y = (ordinal - 1) / 366 + 1; // never past the answer
    while (ymd_to_ord(y + 1, 1, 1) <= ordinal) {
        ++y;
    }
    int remaining = ordinal - ymd_to_ord(y, 1, 1) + 1;
    int m = 1;
    while (remaining > days_in_month(y, m)) {
        remaining -= days_in_month(y, m);
        ++m;
    }
    *year = y;
    *month = m;
    *day = remaining;
}

int iso_week1_monday(int year) {
    const int first_day = ymd_to_ord(year, 1, 1);
    const int first_weekday = (first_day + 6) % 7;
    int week1_monday = first_day - first_weekday;
    if (first_weekday > 3) {
        week1_monday += 7;
    }
    return week1_monday;
}

int iso_to_ymd(int iso_year, int iso_week, int iso_day, int* year, int* month, int* day) {
    if (iso_year < 1 || iso_year > 9999) {
        return -4;
    }
    if (iso_week <= 0 || iso_week >= 53) {
        bool out_of_range = true;
        if (iso_week == 53) {
            // 53 weeks when the year starts on a Thursday, or on a Wednesday in a leap year.
            const int first_weekday = (ymd_to_ord(iso_year, 1, 1) + 6) % 7;
            if (first_weekday == 3 || (first_weekday == 2 && is_leap(iso_year))) {
                out_of_range = false;
            }
        }
        if (out_of_range) {
            return -2;
        }
    }
    if (iso_day <= 0 || iso_day >= 8) {
        return -3;
    }
    ord_to_ymd(iso_week1_monday(iso_year) + (iso_week - 1) * 7 + iso_day - 1, year, month, day);
    return 0;
}

int parse_isoformat_date(const char* dtstr, std::size_t len, int* year, int* month, int* day) {
    const char* p = parse_digits(dtstr, year, 4);
    if (p == nullptr) {
        return -1;
    }
    const bool uses_separator = *p == '-';
    if (uses_separator) {
        ++p;
    }
    if (*p == 'W') {
        ++p;
        int iso_week = 0;
        int iso_day = 0;
        p = parse_digits(p, &iso_week, 2);
        if (p == nullptr) {
            return -3;
        }
        if (static_cast<std::size_t>(p - dtstr) < len) {
            if (uses_separator && *(p++) != '-') {
                return -2;
            }
            p = parse_digits(p, &iso_day, 1);
            if (p == nullptr) {
                return -4;
            }
        } else {
            iso_day = 1;
        }
        const int rv = iso_to_ymd(*year, iso_week, iso_day, year, month, day);
        return rv != 0 ? -3 + rv : 0;
    }
    p = parse_digits(p, month, 2);
    if (p == nullptr) {
        return -1;
    }
    if (uses_separator && *(p++) != '-') {
        return -2;
    }
    p = parse_digits(p, day, 2);
    return p == nullptr ? -1 : 0;
}

// HH[:?MM[:?SS[{.,}fff[fff]]]] up to p_end: 0 at the end of the string, 1 when text follows,
// negative when malformed.
int parse_hh_mm_ss_ff(const char* tstr, const char* tstr_end, int* hour, int* minute, int* second,
                      int* microsecond) {
    *hour = *minute = *second = *microsecond = 0;
    const char* p = tstr;
    int* vals[3] = {hour, minute, second};
    bool has_separator = true;
    for (std::size_t i = 0; i < 3; ++i) {
        p = parse_digits(p, vals[i], 2);
        if (p == nullptr) {
            return -3;
        }
        const char c = *(p++);
        if (i == 0) {
            has_separator = c == ':';
        }
        if (p >= tstr_end) {
            return c != '\0' ? 1 : 0;
        }
        if (has_separator && c == ':') {
            continue;
        }
        if (c == '.' || c == ',') {
            break;
        }
        if (!has_separator) {
            --p;
        } else {
            return -4;
        }
    }
    const auto len_remains = static_cast<std::size_t>(tstr_end - p);
    const std::size_t to_parse = len_remains >= 6 ? 6 : len_remains;
    p = parse_digits(p, microsecond, to_parse);
    if (p == nullptr) {
        return -3;
    }
    static constexpr int kCorrection[] = {100000, 10000, 1000, 100, 10};
    if (to_parse < 6) {
        *microsecond *= kCorrection[to_parse - 1];
    }
    while (is_digit(*p)) {
        ++p; // digits past the sixth are dropped
    }
    return *p != '\0' ? 1 : 0;
}

int parse_isoformat_time(const char* dtstr, std::size_t dtlen, int* hour, int* minute, int* second,
                         int* microsecond, int* tzoffset, int* tzmicrosecond) {
    const char* p_end = dtstr + dtlen;
    const char* tzinfo_pos = dtstr;
    do {
        if (*tzinfo_pos == 'Z' || *tzinfo_pos == '+' || *tzinfo_pos == '-') {
            break;
        }
    } while (++tzinfo_pos < p_end);

    int rv = parse_hh_mm_ss_ff(dtstr, tzinfo_pos, hour, minute, second, microsecond);
    if (rv < 0) {
        return rv;
    }
    if (tzinfo_pos == p_end) {
        return rv == 1 ? -5 : 0;
    }
    if (*tzinfo_pos == 'Z') {
        *tzoffset = 0;
        *tzmicrosecond = 0;
        return *(tzinfo_pos + 1) != '\0' ? -5 : 1;
    }
    const int tzsign = *tzinfo_pos == '-' ? -1 : 1;
    ++tzinfo_pos;
    int tzhour = 0;
    int tzminute = 0;
    int tzsecond = 0;
    rv = parse_hh_mm_ss_ff(tzinfo_pos, p_end, &tzhour, &tzminute, &tzsecond, tzmicrosecond);
    *tzoffset = tzsign * ((tzhour * 3600) + (tzminute * 60) + tzsecond);
    *tzmicrosecond *= tzsign;
    return rv != 0 ? -5 : 1;
}

std::size_t find_separator(const std::string& s) {
    const std::size_t len = s.size();
    if (len == 7) {
        return 7;
    }
    const auto at = [&](std::size_t i) {
        return i < len ? s[i] : '\0';
    };
    if (at(4) == '-') {
        if (at(5) == 'W') {
            if (len < 8) {
                return std::string::npos;
            }
            if (len > 8 && at(8) == '-') {
                if (len == 9) {
                    return std::string::npos;
                }
                // YYYY-Www-## is read as the separator at 8 (CPython's best effort).
                return len > 10 && is_digit(at(10)) ? 8 : 10;
            }
            return 8;
        }
        return 10;
    }
    if (at(4) == 'W') {
        std::size_t idx = 7;
        while (idx < len && is_digit(s[idx])) {
            ++idx;
        }
        if (idx < 9) {
            return idx;
        }
        return idx % 2 == 0 ? 7 : 8;
    }
    return 8;
}

[[noreturn]] void invalid(std::string_view text) {
    throw ValueError("Invalid isoformat string: " + repr(text));
}

void check_date(int year, int month, int day) {
    if (year < 1 || year > 9999) {
        throw ValueError(std::format("year {} is out of range", year));
    }
    if (month < 1 || month > 12) {
        throw ValueError("month must be in 1..12");
    }
    if (day < 1 || day > days_in_month(year, month)) {
        throw ValueError("day is out of range for month");
    }
}

void check_time(int hour, int minute, int second, int microsecond) {
    if (hour < 0 || hour > 23) {
        throw ValueError("hour must be in 0..23");
    }
    if (minute < 0 || minute > 59) {
        throw ValueError("minute must be in 0..59");
    }
    if (second < 0 || second > 59) {
        throw ValueError("second must be in 0..59");
    }
    if (microsecond < 0 || microsecond > 999999) {
        throw ValueError("microsecond must be in 0..999999");
    }
}

// repr(timedelta(microseconds=us)), as the offset error quotes it.
std::string timedelta_repr(std::int64_t us) {
    constexpr std::int64_t kDay = 86400LL * 1000000;
    std::int64_t days = us / kDay;
    std::int64_t rest = us % kDay;
    if (rest < 0) {
        rest += kDay;
        --days;
    }
    const std::int64_t seconds = rest / 1000000;
    const std::int64_t micro = rest % 1000000;
    std::string args;
    const auto add = [&](const char* name, std::int64_t v) {
        if (v != 0) {
            args += std::format("{}{}={}", args.empty() ? "" : ", ", name, v);
        }
    };
    add("days", days);
    add("seconds", seconds);
    add("microseconds", micro);
    return "datetime.timedelta(" + (args.empty() ? std::string("0") : args) + ")";
}

// CPython's _sanitize_isoformat_str: a surrogate is allowed only as the separator (at 7, 8 or
// 10), where it is read as 'T'; anywhere else the text cannot be UTF-8 and is invalid.
std::string sanitize(std::string_view text) {
    std::u32string cps = to_code_points(text);
    const auto surrogate = [](char32_t c) {
        return c >= 0xD800 && c <= 0xDFFF;
    };
    for (const std::size_t pos : {7u, 8u, 10u}) {
        if (pos > cps.size()) {
            break;
        }
        if (pos < cps.size() && surrogate(cps[pos])) {
            cps[pos] = U'T';
            break;
        }
    }
    for (const char32_t c : cps) {
        if (surrogate(c)) {
            invalid(text);
        }
    }
    return to_utf8(cps);
}

} // namespace

std::string Date::isoformat() const {
    return std::format("{:04}-{:02}-{:02}", year, month, day);
}

std::string DateTime::isoformat(char sep, std::string_view timespec) const {
    std::string time;
    if (timespec == "auto") {
        timespec = microsecond != 0 ? "microseconds" : "seconds";
    }
    if (timespec == "hours") {
        time = std::format("{:02}", hour);
    } else if (timespec == "minutes") {
        time = std::format("{:02}:{:02}", hour, minute);
    } else if (timespec == "seconds") {
        time = std::format("{:02}:{:02}:{:02}", hour, minute, second);
    } else if (timespec == "milliseconds") {
        time = std::format("{:02}:{:02}:{:02}.{:03}", hour, minute, second, microsecond / 1000);
    } else if (timespec == "microseconds") {
        time = std::format("{:02}:{:02}:{:02}.{:06}", hour, minute, second, microsecond);
    } else {
        throw ValueError("Unknown timespec value");
    }
    std::string out = std::format("{:04}-{:02}-{:02}{}{}", year, month, day, sep, time);
    if (utc_offset_us) {
        std::int64_t off = *utc_offset_us;
        const char sign = off < 0 ? '-' : '+';
        if (off < 0) {
            off = -off;
        }
        const std::int64_t hh = off / 3600000000LL;
        const std::int64_t mm = off / 60000000LL % 60;
        const std::int64_t ss = off / 1000000 % 60;
        const std::int64_t us = off % 1000000;
        out += std::format("{}{:02}:{:02}", sign, hh, mm);
        if (ss != 0 || us != 0) {
            out += std::format(":{:02}", ss);
            if (us != 0) {
                out += std::format(".{:06}", us);
            }
        }
    }
    return out;
}

DateTime fromisoformat(std::string_view text) {
    if (len(text) < 7) {
        invalid(text);
    }
    const std::string s = sanitize(text);
    const std::size_t separator = find_separator(s);
    if (separator == std::string::npos) {
        invalid(text);
    }
    DateTime dt;
    int year = 0, month = 0, day = 0;
    int hour = 0, minute = 0, second = 0, microsecond = 0;
    int tzoffset = 0, tzusec = 0;
    int rv = parse_isoformat_date(s.c_str(), separator, &year, &month, &day);
    if (rv == 0 && s.size() > separator) {
        // The separator is any one code point, so skip as many bytes as it is long.
        const auto lead = static_cast<unsigned char>(s[separator]);
        std::size_t skip = 1;
        if ((lead & 0x80) != 0) {
            skip = (lead & 0xF0) == 0xE0 ? 3 : (lead & 0xF0) == 0xF0 ? 4 : 2;
        }
        const std::size_t start = std::min(separator + skip, s.size());
        rv = parse_isoformat_time(s.c_str() + start, s.size() - start, &hour, &minute, &second,
                                  &microsecond, &tzoffset, &tzusec);
    }
    if (rv < 0) {
        invalid(text);
    }
    if (rv == 1) {
        // CPython returns timezone.utc whenever the whole seconds are zero.
        if (tzoffset == 0) {
            dt.utc_offset_us = 0;
        } else {
            const std::int64_t us = static_cast<std::int64_t>(tzoffset) * 1000000 + tzusec;
            constexpr std::int64_t kDay = 86400LL * 1000000;
            if (us <= -kDay || us >= kDay) {
                throw ValueError("offset must be a timedelta strictly between "
                                 "-timedelta(hours=24) and timedelta(hours=24), not " +
                                 timedelta_repr(us) + ".");
            }
            dt.utc_offset_us = us;
        }
    }
    check_date(year, month, day);
    check_time(hour, minute, second, microsecond);
    dt.year = year;
    dt.month = month;
    dt.day = day;
    dt.hour = hour;
    dt.minute = minute;
    dt.second = second;
    dt.microsecond = microsecond;
    return dt;
}

Date date_fromisoformat(std::string_view text) {
    const std::string s(text);
    int year = 0, month = 0, day = 0;
    int rv = -1;
    if (s.size() == 7 || s.size() == 8 || s.size() == 10) {
        rv = parse_isoformat_date(s.c_str(), s.size(), &year, &month, &day);
    }
    if (rv < 0) {
        invalid(text);
    }
    check_date(year, month, day);
    return {year, month, day};
}

} // namespace fv::py
