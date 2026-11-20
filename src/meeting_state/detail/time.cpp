#include "meeting_state/detail/time.h"

#include <array>
#include <format>
#include <string_view>

#include "pyport/errors.h"

namespace fv::meeting_state::detail {

namespace {

using std::chrono::days;
using std::chrono::hours;
using std::chrono::microseconds;
using std::chrono::minutes;
using std::chrono::seconds;

constexpr std::array<std::string_view, 12> kMonthAbbreviations = {
    "Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};

py::DateTime checked(py::DateTime moment) {
    if (moment.year < 1 || moment.year > 9999) {
        throw py::OverflowError("date value out of range");
    }
    return moment;
}

} // namespace

LocalMicros wall(const py::DateTime& moment) {
    const std::chrono::year_month_day date{std::chrono::year{moment.year},
                                           std::chrono::month{static_cast<unsigned>(moment.month)},
                                           std::chrono::day{static_cast<unsigned>(moment.day)}};
    // Summed in 64-bit microseconds: chrono's int-based minutes overflow past about year 6053.
    const LocalMicros midnight{std::chrono::local_days{date}};
    return midnight + hours{moment.hour} + minutes{moment.minute} + seconds{moment.second} +
           microseconds{moment.microsecond};
}

py::DateTime naive_from(LocalMicros local) {
    const auto day = std::chrono::floor<days>(local);
    const std::chrono::year_month_day date{day};
    const std::chrono::hh_mm_ss<microseconds> time{local - day};
    py::DateTime moment;
    moment.year = static_cast<int>(date.year());
    moment.month = static_cast<int>(static_cast<unsigned>(date.month()));
    moment.day = static_cast<int>(static_cast<unsigned>(date.day()));
    moment.hour = static_cast<int>(time.hours().count());
    moment.minute = static_cast<int>(time.minutes().count());
    moment.second = static_cast<int>(time.seconds().count());
    moment.microsecond = static_cast<int>(time.subseconds().count());
    return moment;
}

Micros utc_instant(const py::DateTime& aware) {
    return Micros{wall(aware).time_since_epoch()} - microseconds{aware.utc_offset_us.value_or(0)};
}

Micros local_instant(const py::DateTime& naive, const std::chrono::time_zone& zone) {
    const LocalMicros local = wall(naive);
    const auto info = zone.get_info(std::chrono::floor<seconds>(local));
    // For a unique time `first` is the only offset; for an ambiguous or nonexistent one it is
    // the offset in force before the change, which is CPython's fold=0 answer either way.
    return Micros{local.time_since_epoch()} -
           std::chrono::duration_cast<microseconds>(info.first.offset);
}

py::DateTime in_zone(Micros instant, const std::chrono::time_zone& zone) {
    const auto info = zone.get_info(instant);
    py::DateTime moment =
        checked(naive_from(LocalMicros{instant.time_since_epoch()} +
                           std::chrono::duration_cast<microseconds>(info.offset)));
    moment.utc_offset_us = std::chrono::duration_cast<microseconds>(info.offset).count();
    return moment;
}

py::DateTime at_offset(Micros instant, seconds offset) {
    py::DateTime moment = checked(naive_from(LocalMicros{instant.time_since_epoch()} + offset));
    moment.utc_offset_us = std::chrono::duration_cast<microseconds>(offset).count();
    return moment;
}

py::DateTime utc_now(const Clock& clock) {
    const auto now = std::chrono::floor<microseconds>(clock.utc());
    py::DateTime moment = naive_from(LocalMicros{now.time_since_epoch()});
    moment.utc_offset_us = 0;
    return moment;
}

py::DateTime local_now(const Clock& clock) {
    const auto now = std::chrono::floor<microseconds>(clock.utc());
    return naive_from(clock.zone()->to_local(now));
}

double total_seconds(microseconds difference) {
    // Python divides the exact integer count; a double holds counts below 2**53 exactly, so
    // only larger ones are split into whole seconds and a remainder first.
    const std::int64_t count = difference.count();
    constexpr std::int64_t exact = std::int64_t{1} << 53;
    if (count > -exact && count < exact) {
        return static_cast<double>(count) / 1e6;
    }
    return static_cast<double>(count / 1'000'000) + static_cast<double>(count % 1'000'000) / 1e6;
}

std::string format_started(const py::DateTime& moment) {
    const int hour12 = moment.hour % 12 == 0 ? 12 : moment.hour % 12;
    return std::format("{} {:02}, {:04} \xC2\xB7 {:02}:{:02} {}",
                       kMonthAbbreviations[static_cast<std::size_t>(moment.month - 1)], moment.day,
                       moment.year, hour12, moment.minute, moment.hour < 12 ? "AM" : "PM");
}

} // namespace fv::meeting_state::detail
