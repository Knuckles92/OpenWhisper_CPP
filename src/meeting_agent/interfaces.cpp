#include "meeting_agent/interfaces.h"

#include <algorithm>
#include <chrono>
#include <cstdint>

#include "pyport/datetime.h"

namespace fv::meeting_agent {

bool is_activity_kind(std::string_view kind) {
    return std::ranges::find(ACTIVITY_KINDS, kind) != ACTIVITY_KINDS.end();
}

Json AgentActivity::to_json() const {
    return Json{
        {"kind", kind}, {"label", label}, {"tool", tool}, {"pass_kind", pass_kind}, {"ts", ts}};
}

std::string utc_now_iso(const Clock& clock) {
    using namespace std::chrono;
    const auto since_epoch = duration_cast<microseconds>(clock.utc().time_since_epoch());
    const auto day_start = floor<days>(sys_time<microseconds>(since_epoch));
    const year_month_day date{day_start};
    const hh_mm_ss<microseconds> time{sys_time<microseconds>(since_epoch) - day_start};
    py::DateTime value;
    value.year = static_cast<int>(date.year());
    value.month = static_cast<int>(static_cast<unsigned>(date.month()));
    value.day = static_cast<int>(static_cast<unsigned>(date.day()));
    value.hour = static_cast<int>(time.hours().count());
    value.minute = static_cast<int>(time.minutes().count());
    value.second = static_cast<int>(time.seconds().count());
    value.microsecond = static_cast<int>(time.subseconds().count());
    value.utc_offset_us = std::int64_t{0};
    return value.isoformat();
}

} // namespace fv::meeting_agent
