#pragma once

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>

#include "base/clock.h"
#include "pyport/datetime.h"

// Conversions between pyport's datetime and std::chrono for the history library.
namespace fv::history_detail {

using Micros = std::chrono::sys_time<std::chrono::microseconds>;

// The moment an aware datetime names; a naive one is read as UTC.
Micros utc_instant(const py::DateTime& moment);

// A naive datetime from wall-clock fields, to the microsecond.
py::DateTime naive_from(std::chrono::local_time<std::chrono::microseconds> local);

// datetime.astimezone(zone).replace(tzinfo=None); nullopt when the result leaves years
// 1..9999 (Python's OverflowError).
std::optional<py::DateTime> to_zone(Micros instant, const std::chrono::time_zone& zone);

// Whether the Windows C runtime's localtime takes the moment: whole seconds since 1970 from 0
// to 3001-01-19 07:59:59 UTC. Python's astimezone() with no zone fails
// outside it with OSError on Windows.
bool windows_localtime_accepts(Micros instant);

// datetime.now(timezone.utc) from the clock, aware.
py::DateTime utc_now(const Clock& clock);
// datetime.now(): naive wall time in the clock's zone.
py::DateTime local_now(const Clock& clock);

// Aware moments as microseconds since the epoch, for ordering and differences; naive moments
// count as UTC (dictation_stats._utc).
std::int64_t epoch_micros(const py::DateTime& moment);

} // namespace fv::history_detail
