#pragma once

#include <chrono>
#include <cstdint>
#include <string>

#include "base/clock.h"
#include "pyport/datetime.h"

// pyport datetimes against std::chrono for the meeting-state library.
namespace fv::meeting_state::detail {

using Micros = std::chrono::sys_time<std::chrono::microseconds>;
using LocalMicros = std::chrono::local_time<std::chrono::microseconds>;

// The wall-clock fields of a datetime, ignoring its offset.
LocalMicros wall(const py::DateTime& moment);
// A naive datetime from wall-clock fields.
py::DateTime naive_from(LocalMicros local);

// The instant an aware datetime names.
Micros utc_instant(const py::DateTime& aware);
// A naive local wall time read in `zone` as Python's astimezone() reads it (fold=0): an
// ambiguous time takes the offset before the change, and so does a time in a gap.
Micros local_instant(const py::DateTime& naive, const std::chrono::time_zone& zone);

// datetime.astimezone(zone) and astimezone(timezone(offset)): aware, with the offset in force.
// Throws py::OverflowError("date value out of range") outside years 1..9999.
py::DateTime in_zone(Micros instant, const std::chrono::time_zone& zone);
py::DateTime at_offset(Micros instant, std::chrono::seconds offset);

// datetime.now(timezone.utc) and datetime.now() from the clock.
py::DateTime utc_now(const Clock& clock);
py::DateTime local_now(const Clock& clock);

// timedelta.total_seconds() of a difference in microseconds.
double total_seconds(std::chrono::microseconds difference);

// strftime("%b %d, %Y · %I:%M %p") with English month names.
std::string format_started(const py::DateTime& moment);

} // namespace fv::meeting_state::detail
