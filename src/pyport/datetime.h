#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace fv::py {

// datetime.date: year 1..9999.
struct Date {
    int year = 1;
    int month = 1;
    int day = 1;

    std::string isoformat() const; // "YYYY-MM-DD"
    friend bool operator==(const Date&, const Date&) = default;
};

// datetime.datetime, naive or with a fixed UTC offset (timezone(timedelta)), to the
// microsecond. fold and named zones are not modelled.
struct DateTime {
    int year = 1;
    int month = 1;
    int day = 1;
    int hour = 0;
    int minute = 0;
    int second = 0;
    int microsecond = 0;
    std::optional<std::int64_t> utc_offset_us; // set when aware; 0 is timezone.utc

    // isoformat(sep, timespec): timespec is "auto", "hours", "minutes", "seconds",
    // "milliseconds" or "microseconds"; another throws ValueError("Unknown timespec value").
    std::string isoformat(char sep = 'T', std::string_view timespec = "auto") const;
    Date date() const { return {year, month, day}; }
    friend bool operator==(const DateTime&, const DateTime&) = default;
};

// datetime.fromisoformat of CPython 3.12's C module: YYYY-MM-DD, YYYYMMDD and ISO week dates,
// any one character as the separator, HH[:MM[:SS[.f]]] or the basic forms with up to six
// fraction digits kept, "Z" or a +HH[:MM[:SS[.ffffff]]] offset. Throws ValueError with
// Python's message ("Invalid isoformat string: '...'", "month must be in 1..12", ...).
DateTime fromisoformat(std::string_view text);

// date.fromisoformat: YYYY-MM-DD, YYYYMMDD, YYYY-Www[-D], YYYYWww[D].
Date date_fromisoformat(std::string_view text);

} // namespace fv::py
