#include "pyport/email_date.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <string>
#include <utility>
#include <vector>

#include "pyport/number.h"
#include "pyport/text.h"
#include "pyport/unicode.h"

namespace fv::py {

namespace {

constexpr std::array<std::string_view, 24> kMonthNames = {
    "jan", "feb",  "mar",  "apr",    "may",       "jun",      "jul",      "aug",
    "sep", "oct",  "nov",  "dec",    "january",   "february", "march",    "april",
    "may", "june", "july", "august", "september", "october",  "november", "december"};
constexpr std::array<std::string_view, 7> kDayNames = {"mon", "tue", "wed", "thu",
                                                       "fri", "sat", "sun"};

// _timezones: hours and minutes as hhmm.
constexpr std::array<std::pair<std::string_view, int>, 14> kZones = {{{"UT", 0},
                                                                      {"UTC", 0},
                                                                      {"GMT", 0},
                                                                      {"Z", 0},
                                                                      {"AST", -400},
                                                                      {"ADT", -300},
                                                                      {"EST", -500},
                                                                      {"EDT", -400},
                                                                      {"CST", -600},
                                                                      {"CDT", -500},
                                                                      {"MST", -700},
                                                                      {"MDT", -600},
                                                                      {"PST", -800},
                                                                      {"PDT", -700}}};

std::optional<std::size_t> month_index(std::string_view name) {
    const auto found = std::ranges::find(kMonthNames, name);
    if (found == kMonthNames.end()) {
        return std::nullopt;
    }
    return static_cast<std::size_t>(found - kMonthNames.begin());
}

void drop_last(std::string& text) {
    // Only ever an ASCII comma, so one byte.
    text.pop_back();
}

bool first_is_digit(std::string_view text) {
    std::size_t at = 0;
    return !text.empty() && py::is_digit(py::next_code_point(text, at));
}

// days_from_civil (Howard Hinnant): days since 1970-01-01 of a proleptic Gregorian date.
std::int64_t days_from_civil(std::int64_t y, std::int64_t m, std::int64_t d) {
    y -= m <= 2 ? 1 : 0;
    const std::int64_t era = (y >= 0 ? y : y - 399) / 400;
    const std::int64_t yoe = y - era * 400;
    const std::int64_t doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    const std::int64_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + doe - 719468;
}

bool is_leap(std::int64_t year) {
    return (year % 4 == 0 && year % 100 != 0) || year % 400 == 0;
}

std::int64_t days_in_month(std::int64_t year, std::int64_t month) {
    static constexpr std::array<int, 12> kDays = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    return month == 2 && is_leap(year) ? 29 : kDays[static_cast<std::size_t>(month - 1)];
}

struct Parsed {
    std::int64_t year, month, day, hour, minute, second;
    std::optional<std::int64_t> offset_s;
};

// _parsedate_tz; nullopt where it returns None or raises.
std::optional<Parsed> parsedate_tz(std::string_view text) {
    if (text.empty()) {
        return std::nullopt;
    }
    std::vector<std::string> data = py::split(text);
    if (data.empty()) {
        return std::nullopt;
    }
    if (data[0].ends_with(',') ||
        std::ranges::find(kDayNames, py::lower(data[0])) != kDayNames.end()) {
        data.erase(data.begin());
    } else if (const auto comma = data[0].rfind(','); comma != std::string::npos) {
        data[0] = data[0].substr(comma + 1);
    }
    if (data.size() == 3) {
        std::vector<std::string> stuff = py::split_on(data[0], "-");
        if (stuff.size() == 3) {
            stuff.insert(stuff.end(), data.begin() + 1, data.end());
            data = std::move(stuff);
        }
    }
    if (data.size() == 4) {
        const std::string s = data[3];
        auto at = s.find('+');
        if (at == std::string::npos) {
            at = s.find('-');
        }
        if (at != std::string::npos && at > 0) {
            data[3] = s.substr(0, at);
            data.push_back(s.substr(at));
        } else {
            data.emplace_back();
        }
    }
    if (data.size() < 5) {
        return std::nullopt;
    }
    std::string dd = data[0];
    std::string mm = data[1];
    std::string yy = data[2];
    std::string tm = data[3];
    std::string tz = data[4];
    if (dd.empty() || mm.empty() || yy.empty()) {
        return std::nullopt;
    }
    mm = py::lower(mm);
    auto month = month_index(mm);
    if (!month) {
        std::string swapped = py::lower(dd);
        dd = mm;
        mm = swapped;
        month = month_index(mm);
        if (!month) {
            return std::nullopt;
        }
    }
    std::int64_t month_number = static_cast<std::int64_t>(*month) + 1;
    if (month_number > 12) {
        month_number -= 12;
    }
    if (dd.ends_with(',')) {
        drop_last(dd);
    }
    if (const auto colon = yy.find(':'); colon != std::string::npos && colon > 0) {
        std::swap(yy, tm);
    }
    if (yy.empty()) {
        return std::nullopt; // yy[-1] raises IndexError
    }
    if (yy.ends_with(',')) {
        drop_last(yy);
        if (yy.empty()) {
            return std::nullopt;
        }
    }
    if (!first_is_digit(yy)) {
        std::swap(yy, tz);
    }
    if (tm.empty()) {
        return std::nullopt; // tm[-1] raises IndexError
    }
    if (tm.ends_with(',')) {
        drop_last(tm);
    }
    std::vector<std::string> fields = py::split_on(tm, ":");
    std::string thh;
    std::string tmm;
    std::string tss = "0";
    if (fields.size() == 2) {
        thh = fields[0];
        tmm = fields[1];
    } else if (fields.size() == 3) {
        thh = fields[0];
        tmm = fields[1];
        tss = fields[2];
    } else if (fields.size() == 1 && fields[0].find('.') != std::string::npos) {
        fields = py::split_on(fields[0], ".");
        if (fields.size() == 2) {
            thh = fields[0];
            tmm = fields[1];
        } else if (fields.size() == 3) {
            thh = fields[0];
            tmm = fields[1];
            tss = fields[2];
        } else {
            return std::nullopt;
        }
    } else {
        return std::nullopt;
    }
    const auto year = py::py_int(yy);
    const auto day = py::py_int(dd);
    const auto hour = py::py_int(thh);
    const auto minute = py::py_int(tmm);
    const auto second = py::py_int(tss);
    if (!year || !day || !hour || !minute || !second) {
        return std::nullopt;
    }
    Parsed parsed{*year, month_number, *day, *hour, *minute, *second, std::nullopt};
    if (parsed.year < 100) {
        parsed.year += parsed.year > 68 ? 1900 : 2000;
    }
    std::optional<std::int64_t> offset;
    const std::string zone = py::upper(tz);
    const auto named =
        std::ranges::find(kZones, std::string_view(zone), &std::pair<std::string_view, int>::first);
    if (named != kZones.end()) {
        offset = named->second;
    } else {
        const auto number = py::py_int_checked(zone);
        if (number) {
            offset = *number;
            if (*offset == 0 && zone.starts_with('-')) {
                offset.reset();
            }
        } else if (number.error() == py::IntError::Overflow) {
            return std::nullopt; // a zone that large makes timezone() raise
        }
    }
    if (offset && *offset != 0) {
        std::int64_t value = *offset;
        const std::int64_t sign = value < 0 ? -1 : 1;
        value = value < 0 ? -value : value;
        // A zone beyond a day is refused below; keep the arithmetic in range on the way there.
        if (value > 100'000'000'000) {
            return std::nullopt;
        }
        offset = sign * ((value / 100) * 3600 + (value % 100) * 60);
    }
    parsed.offset_s = offset;
    return parsed;
}

} // namespace

std::optional<std::int64_t> parse_http_date(std::string_view text) {
    const auto parsed = parsedate_tz(text);
    if (!parsed) {
        return std::nullopt;
    }
    // datetime(*dtuple[:6]) and timezone(timedelta(seconds=tz)).
    if (parsed->year < 1 || parsed->year > 9999 || parsed->day < 1 ||
        parsed->day > days_in_month(parsed->year, parsed->month) || parsed->hour < 0 ||
        parsed->hour > 23 || parsed->minute < 0 || parsed->minute > 59 || parsed->second < 0 ||
        parsed->second > 59) {
        return std::nullopt;
    }
    const std::int64_t offset = parsed->offset_s.value_or(0);
    if (offset <= -86400 || offset >= 86400) {
        return std::nullopt;
    }
    return days_from_civil(parsed->year, parsed->month, parsed->day) * 86400 + parsed->hour * 3600 +
           parsed->minute * 60 + parsed->second - offset;
}

} // namespace fv::py
