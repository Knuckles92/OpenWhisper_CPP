#pragma once

#include <chrono>
#include <optional>
#include <string>

#include "base/clock.h"
#include "base/json.h"
#include "history/format_utils.h"
#include "pyport/datetime.h"

// meeting/time_utils.py: Meeting Mode stores new timestamps as aware UTC ("...Z") and reads
// legacy naive ones as local wall time. "Local" is the clock's zone; "now" is the clock's.
// Values come in as JSON because rows hold strings, nulls or anything a corrupt file left.
// Affinity: thread-safe (pure functions).
namespace fv::meeting_state {

using Json = fv::Json;

// UNKNOWN_DATE (services/format_utils.py) is fv::UNKNOWN_DATE from history/format_utils.h.

// The current instant as UTC ISO text with "Z": "2026-08-20T16:55:00.123456Z".
std::string utc_now_iso(const Clock& clock = system_clock());

// fromisoformat of str(value) with every "Z" read as "+00:00"; nullopt for a falsy value or
// text Python refuses.
std::optional<py::DateTime> parse_meeting_time(const Json& value);

// An aware value converted to the local zone (aware, with the offset in force); a legacy naive
// value is returned unchanged. Throws py::OverflowError when the conversion leaves years
// 1..9999.
std::optional<py::DateTime> as_local_time(const Json& value, const Clock& clock = system_clock());
std::optional<py::DateTime> as_local_time(const Json& value,
                                          const std::chrono::time_zone& local_tz);
// local_tz=timezone(timedelta(seconds=offset)).
std::optional<py::DateTime> as_local_time(const Json& value, std::chrono::seconds local_offset);

// Wall seconds from start to end; a mixed aware/naive pair reads the naive side as local time.
std::optional<double> elapsed_seconds(const Json& started_at, const Json& ended_at,
                                      const Clock& clock = system_clock());

// Age in seconds, against local wall time for a naive value and UTC for an aware one.
std::optional<double> seconds_since(const Json& value, const Clock& clock = system_clock());

// "Aug 20, 2026 · 09:55 AM" in the local zone, or UNKNOWN_DATE.
std::string format_meeting_started_at(const Json& value, const Clock& clock = system_clock());

// Recorded seconds of a meeting row (started_at, ended_at, paused_total_s): wall time minus
// pauses, at least 0. `running` counts a meeting without ended_at up to now. nullopt when a
// needed timestamp is missing.
std::optional<double> meeting_duration_s(const Json& meeting, bool running = false,
                                         const Clock& clock = system_clock());

// "42 sec", "12 min", "1h 5m"; "" when the duration is unknown.
std::string format_meeting_duration(const Json& meeting, const Clock& clock = system_clock());

// "<started> · <duration>", or the start alone without a duration.
std::string format_meeting_identity_meta(const Json& meeting, const Clock& clock = system_clock());

} // namespace fv::meeting_state
