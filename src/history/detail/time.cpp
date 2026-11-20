#include "history/detail/time.h"

namespace fv::history_detail {

namespace {

using std::chrono::days;
using std::chrono::hours;
using std::chrono::microseconds;
using std::chrono::minutes;
using std::chrono::seconds;

std::chrono::local_time<microseconds> wall(const py::DateTime& moment) {
    const std::chrono::year_month_day date{std::chrono::year{moment.year},
                                           std::chrono::month{static_cast<unsigned>(moment.month)},
                                           std::chrono::day{static_cast<unsigned>(moment.day)}};
    // Summed in 64-bit microseconds from the start: days plus chrono's int-based minutes
    // overflow int past about year 6053.
    const std::chrono::local_time<microseconds> midnight{std::chrono::local_days{date}};
    return midnight + hours{moment.hour} + minutes{moment.minute} + seconds{moment.second} +
           microseconds{moment.microsecond};
}

} // namespace

Micros utc_instant(const py::DateTime& moment) {
    const auto local = wall(moment);
    return Micros{local.time_since_epoch()} - microseconds{moment.utc_offset_us.value_or(0)};
}

py::DateTime naive_from(std::chrono::local_time<microseconds> local) {
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

std::optional<py::DateTime> to_zone(Micros instant, const std::chrono::time_zone& zone) {
    const auto local = zone.to_local(instant);
    py::DateTime moment = naive_from(local);
    if (moment.year < 1 || moment.year > 9999) {
        return std::nullopt;
    }
    return moment;
}

bool windows_localtime_accepts(Micros instant) {
    // 3001-01-19T07:59:59Z: measured with OpenWhisper's Python 3.12 on Windows 11, which
    // converts that second and refuses the next with OSError.
    constexpr std::int64_t max_time64 = 32'536'799'999;
    const std::int64_t whole = std::chrono::floor<seconds>(instant.time_since_epoch()).count();
    return whole >= 0 && whole <= max_time64;
}

py::DateTime utc_now(const Clock& clock) {
    const auto now = std::chrono::floor<microseconds>(clock.utc());
    py::DateTime moment = naive_from(std::chrono::local_time<microseconds>{now.time_since_epoch()});
    moment.utc_offset_us = 0;
    return moment;
}

py::DateTime local_now(const Clock& clock) {
    const auto now = std::chrono::floor<microseconds>(clock.utc());
    return naive_from(clock.zone()->to_local(now));
}

std::int64_t epoch_micros(const py::DateTime& moment) {
    return utc_instant(moment).time_since_epoch().count();
}

} // namespace fv::history_detail
