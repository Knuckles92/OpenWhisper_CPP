#pragma once

#include <chrono>
#include <cstdint>

#include "history/detail/time.h"
#include "pyport/datetime.h"

namespace fv::history_detail {

// The moment an aware datetime names (a naive one read as UTC), summed in 64-bit microseconds:
// chrono's int-based hours and minutes overflow past about year 6000 when added to days.
inline Micros utc_moment(const py::DateTime& moment) {
    using namespace std::chrono;
    const year_month_day date{year{moment.year}, month{static_cast<unsigned>(moment.month)},
                              day{static_cast<unsigned>(moment.day)}};
    const std::int64_t days_since_epoch = sys_days{date}.time_since_epoch().count();
    const std::int64_t seconds =
        ((days_since_epoch * 24 + moment.hour) * 60 + moment.minute) * 60 + moment.second;
    return Micros{
        microseconds{seconds * 1'000'000 + moment.microsecond - moment.utc_offset_us.value_or(0)}};
}

// Whether a moment lies in years 1..9999 in UTC, as a Python datetime must after an aware
// value is moved to UTC (OverflowError "date value out of range" otherwise).
inline bool utc_in_range(Micros instant) {
    using namespace std::chrono;
    constexpr sys_days first{year{1} / January / 1};
    constexpr sys_days end{year{10000} / January / 1};
    return instant >= first && instant < end;
}

} // namespace fv::history_detail
