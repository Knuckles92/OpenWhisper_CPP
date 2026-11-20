#pragma once

#include <chrono>
#include <optional>
#include <string>

#include "base/clock.h"
#include "base/json.h"
#include "pyport/datetime.h"
#include "update/check.h"

namespace fv {
class SettingsStore;
}

// When automatic update checks are due, and which releases the copy follows
// (services/app_update.py: release_track ... shows_update_notice).
namespace fv::update {

// Minimum time between successful automatic checks (config.UPDATE_CHECK_INTERVAL_S).
inline constexpr std::chrono::seconds kUpdateCheckInterval{4 * 60 * 60};
// Wait after a failed check, also at launch.
inline constexpr std::chrono::seconds kFailedCheckInterval{60 * 60};

// Beta when update_beta_track_enabled is true (a bool; anything else is stable).
ReleaseTrack release_track(const Json& settings);
// From the store; a file that cannot be read reads as {} and so as stable.
ReleaseTrack release_track(const SettingsStore&);

// After the user switches tracks: drops update_last_check_at so the next automatic check is due
// now. The failure and rate-limit times stay. A write error is logged, not thrown.
void release_track_changed(SettingsStore&);

// A stored check time: a non-blank string datetime.fromisoformat reads (after strip), a naive
// one taken as UTC; anything else nullopt.
std::optional<py::DateTime> parse_last_check_at(const Json& raw);

// Microseconds since the Unix epoch of an aware datetime (naive counts as UTC).
std::chrono::sys_time<std::chrono::microseconds> to_sys_time(const py::DateTime&);
// An aware UTC datetime of `time`, floored to the microsecond as datetime.now(timezone.utc) is.
py::DateTime utc_datetime_micros(std::chrono::sys_time<std::chrono::microseconds> time);
// Any system time, floored first. The microsecond form exists because a system_clock time point
// cannot hold a year past 2262 where the clock ticks in nanoseconds (libstdc++), and
// X-RateLimit-Reset may name one.
template <class Duration> py::DateTime utc_datetime(std::chrono::sys_time<Duration> time) {
    return utc_datetime_micros(std::chrono::floor<std::chrono::microseconds>(time));
}

// Writes `time` under `key` as datetime.isoformat() of an aware UTC time
// ("2026-08-21T12:00:00.250000+00:00"). A write error is logged ("Could not persist update check
// timestamp: ...") and swallowed.
void save_check_timestamp_micros(SettingsStore&, std::string_view key,
                                 std::chrono::sys_time<std::chrono::microseconds> time);
template <class Duration>
void save_check_timestamp(SettingsStore& settings, std::string_view key,
                          std::chrono::sys_time<Duration> time) {
    save_check_timestamp_micros(settings, key, std::chrono::floor<std::chrono::microseconds>(time));
}

// Seconds until the next automatic check is due: `interval` after the last success and
// kFailedCheckInterval after the last failure, 0 when that is now. Stored times in the future
// (the clock moved back) are ignored. nullopt when automatic checks are off: the user disabled
// them, or the build has no update source (source_configured false). The channel does not
// matter: a development build with a source checks too.
std::optional<double> seconds_until_auto_check(const Json& settings,
                                               std::chrono::system_clock::time_point now,
                                               std::chrono::seconds interval, InstallChannel,
                                               bool source_configured);
// A launch asks at once (interval 0) unless a check failed within the hour; otherwise the
// regular kUpdateCheckInterval applies.
std::optional<double> seconds_until_auto_check(const Json& settings,
                                               std::chrono::system_clock::time_point now,
                                               InstallChannel, bool launch, bool source_configured);

// Whether the title bar and tray offer the release: only UpdateAvailable, and only while both
// update_check_enabled and update_notify_enabled are on.
bool shows_update_notice(UpdateStatus, const Json& settings);

} // namespace fv::update
