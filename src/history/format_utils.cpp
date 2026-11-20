#include "history/format_utils.h"

#include <array>
#include <chrono>
#include <cmath>
#include <format>

#include "history/detail/time.h"
#include "pyport/datetime.h"
#include "pyport/errors.h"
#include "pyport/number.h"

namespace fv {

namespace {

constexpr std::array<std::string_view, 12> month_abbreviations = {
    "Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};

// strftime("%b %d, %Y %I:%M %p") in the C locale.
std::string display(const py::DateTime& moment) {
    const int hour12 = moment.hour % 12 == 0 ? 12 : moment.hour % 12;
    return std::format("{} {:02}, {:04} {:02}:{:02} {}", month_abbreviations[moment.month - 1],
                       moment.day, moment.year, hour12, moment.minute,
                       moment.hour < 12 ? "AM" : "PM");
}

} // namespace

std::string format_timestamp(std::string_view iso_timestamp, const Clock& clock) {
    py::DateTime moment;
    try {
        moment = py::fromisoformat(iso_timestamp);
    } catch (const py::PyError&) {
        return std::string(iso_timestamp);
    }
    if (moment.utc_offset_us) {
        // astimezone() with no argument goes through the C runtime's localtime, which on
        // Windows refuses moments before 1970 and after 3001-01-19 (OSError, caught there).
        const auto instant = history_detail::utc_instant(moment);
        if (!history_detail::windows_localtime_accepts(instant)) {
            return std::string(iso_timestamp);
        }
        const auto local = history_detail::to_zone(instant, *clock.zone());
        if (!local) {
            return std::string(iso_timestamp);
        }
        moment = *local;
    }
    return display(moment);
}

std::optional<std::string> format_timestamp(const std::optional<std::string>& iso_timestamp,
                                            const Clock& clock) {
    if (!iso_timestamp) {
        return std::nullopt;
    }
    return format_timestamp(std::string_view(*iso_timestamp), clock);
}

std::string format_audio_duration(double seconds) {
    double value = seconds;
    if (value < 0) {
        value = 0.0;
    }
    if (value < 60) {
        return py::format_fixed(value, 1) + "s";
    }
    if (std::isnan(value)) {
        throw py::ValueError("cannot convert float NaN to integer");
    }
    if (std::isinf(value)) {
        throw py::OverflowError("cannot convert float infinity to integer");
    }
    const double rounded = py::round_half_even(value);
    if (rounded >= 9.223372036854775808e18) {
        // Past int64 Python's int keeps every digit: divide the exact decimal digits of the
        // (already whole) double by 3600, as divmod does.
        std::string hours;
        int rem = 0;
        for (const char digit : py::format_fixed(rounded, 0)) {
            rem = rem * 10 + (digit - '0');
            if (!hours.empty() || rem / 3600 != 0) {
                hours += static_cast<char>('0' + rem / 3600);
            }
            rem %= 3600;
        }
        const int minutes = rem / 60;
        return minutes != 0 ? std::format("{}h {}m", hours, minutes) : hours + "h";
    }
    const auto total_seconds = static_cast<std::int64_t>(rounded);
    const std::int64_t hours = total_seconds / 3600;
    const std::int64_t rem = total_seconds % 3600;
    const std::int64_t minutes = rem / 60;
    const std::int64_t secs = rem % 60;
    if (hours > 0) {
        if (minutes != 0) {
            return std::format("{}h {}m", hours, minutes);
        }
        return std::format("{}h", hours);
    }
    return std::format("{}m {}s", minutes, secs);
}

std::string format_audio_duration(std::optional<double> seconds) {
    if (!seconds) {
        return "0.0s";
    }
    return format_audio_duration(*seconds);
}

std::string format_audio_duration(const Json& seconds) {
    if (seconds.is_null()) {
        return "0.0s";
    }
    const std::optional<double> value = py::py_float(seconds);
    if (!value) {
        return "0.0s";
    }
    return format_audio_duration(*value);
}

std::string format_short_duration(double seconds) {
    // max(0.0, x) keeps 0.0 for NaN, which compares false.
    const double value = seconds > 0.0 ? seconds : 0.0;
    if (value < 1) {
        return py::format_fixed(value * 1000, 0) + " ms";
    }
    return format_audio_duration(value);
}

std::string format_short_duration(const Json& seconds) {
    const std::optional<double> value = py::py_float(seconds);
    return format_short_duration(value.value_or(0.0));
}

std::string format_size_bytes(std::int64_t size_bytes) {
    const auto size = static_cast<double>(size_bytes);
    if (size_bytes >= 1'000'000'000) {
        return py::format_fixed(size / 1'000'000'000, 2) + " GB";
    }
    if (size_bytes >= 1'000'000) {
        return py::format_fixed(size / 1'000'000, 0) + " MB";
    }
    if (size_bytes >= 1'000) {
        return py::format_fixed(size / 1'000, 0) + " KB";
    }
    return std::format("{} B", size_bytes);
}

std::string format_sample_rate(std::int64_t hertz) {
    if (hertz <= 0) {
        return "";
    }
    if (hertz % 1000 == 0) {
        return std::format("{} kHz", hertz / 1000);
    }
    return py::format_fixed(static_cast<double>(hertz) / 1000, 1) + " kHz";
}

std::string format_sample_rate(const Json& hertz) {
    const std::optional<std::int64_t> value = py::py_int(hertz);
    if (!value) {
        return "";
    }
    return format_sample_rate(*value);
}

std::string format_file_size(double size_bytes) {
    if (size_bytes < 1024) {
        // int() truncates toward zero.
        return std::format("{} B", static_cast<std::int64_t>(std::trunc(size_bytes)));
    }
    double size = size_bytes / 1024;
    for (const std::string_view unit : {"KB", "MB"}) {
        if (size < 1024) {
            return py::format_fixed(size, 1) + " " + std::string(unit);
        }
        size /= 1024;
    }
    return py::format_fixed(size, 1) + " GB";
}

} // namespace fv
