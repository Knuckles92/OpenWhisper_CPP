#include "update/schedule.h"

#include <algorithm>
#include <exception>

#include "base/log.h"
#include "pyport/errors.h"
#include "pyport/text.h"
#include "settings/keys.h"
#include "settings/store.h"

namespace fv::update {

namespace {

constexpr log::Logger kLog("services.app_update");

using Micros = std::chrono::microseconds;

} // namespace

ReleaseTrack release_track(const Json& settings) {
    return keys::update_beta_track_enabled.read(settings) ? ReleaseTrack::Beta
                                                          : ReleaseTrack::Stable;
}

ReleaseTrack release_track(const SettingsStore& settings) {
    return release_track(*settings.snapshot());
}

void release_track_changed(SettingsStore& settings) {
    try {
        settings.update(Json::object(), {keys::update_last_check_at.key()});
    } catch (const std::exception& error) {
        kLog.warning("Could not reset the update check time: {}", error.what());
    }
}

std::optional<py::DateTime> parse_last_check_at(const Json& raw) {
    if (!raw.is_string()) {
        return std::nullopt;
    }
    const auto text = py::strip(raw.get_ref<const std::string&>());
    if (text.empty()) {
        return std::nullopt;
    }
    py::DateTime parsed;
    try {
        parsed = py::fromisoformat(text);
    } catch (const py::ValueError&) {
        return std::nullopt;
    }
    if (!parsed.utc_offset_us) {
        parsed.utc_offset_us = 0;
    }
    return parsed;
}

std::chrono::sys_time<Micros> to_sys_time(const py::DateTime& value) {
    using namespace std::chrono;
    const sys_days day{year{value.year} / month{static_cast<unsigned>(value.month)} /
                       std::chrono::day{static_cast<unsigned>(value.day)}};
    const auto wall = sys_time<Micros>(day) + hours{value.hour} + minutes{value.minute} +
                      seconds{value.second} + Micros{value.microsecond};
    return wall - Micros{value.utc_offset_us.value_or(0)};
}

py::DateTime utc_datetime_micros(std::chrono::sys_time<std::chrono::microseconds> time) {
    using namespace std::chrono;
    const auto micros = time;
    const auto day = floor<days>(micros);
    const year_month_day date{day};
    const hh_mm_ss clock{micros - day};
    py::DateTime value;
    value.year = static_cast<int>(date.year());
    value.month = static_cast<int>(static_cast<unsigned>(date.month()));
    value.day = static_cast<int>(static_cast<unsigned>(date.day()));
    value.hour = static_cast<int>(clock.hours().count());
    value.minute = static_cast<int>(clock.minutes().count());
    value.second = static_cast<int>(clock.seconds().count());
    value.microsecond = static_cast<int>(clock.subseconds().count());
    value.utc_offset_us = 0;
    return value;
}

void save_check_timestamp_micros(SettingsStore& settings, std::string_view key,
                                 std::chrono::sys_time<std::chrono::microseconds> time) {
    try {
        settings.save_setting(key, utc_datetime_micros(time).isoformat());
    } catch (const std::exception& error) {
        kLog.warning("Could not persist update check timestamp: {}", error.what());
    }
}

std::optional<double> seconds_until_auto_check(const Json& settings,
                                               std::chrono::system_clock::time_point now,
                                               std::chrono::seconds interval, InstallChannel,
                                               bool source_configured) {
    if (!source_configured) {
        return std::nullopt;
    }
    if (!keys::update_check_enabled.read(settings)) {
        return std::nullopt;
    }
    const auto current = std::chrono::floor<Micros>(now);
    auto due = current;
    const std::pair<std::string_view, std::chrono::seconds> waits[] = {
        {keys::update_last_check_at.key(), interval},
        {keys::update_last_failed_check_at.key(), kFailedCheckInterval},
    };
    for (const auto& [key, wait] : waits) {
        const auto found = settings.find(std::string(key));
        if (found == settings.end()) {
            continue;
        }
        if (const auto at = parse_last_check_at(*found)) {
            const auto when = to_sys_time(*at);
            if (when <= current) {
                due = std::max(due, when + wait);
            }
        }
    }
    return static_cast<double>((due - current).count()) / 1e6;
}

std::optional<double> seconds_until_auto_check(const Json& settings,
                                               std::chrono::system_clock::time_point now,
                                               InstallChannel channel, bool launch,
                                               bool source_configured) {
    return seconds_until_auto_check(settings, now,
                                    launch ? std::chrono::seconds{0} : kUpdateCheckInterval,
                                    channel, source_configured);
}

bool shows_update_notice(UpdateStatus status, const Json& settings) {
    if (status != UpdateStatus::UpdateAvailable) {
        return false;
    }
    return keys::update_check_enabled.read(settings) && keys::update_notify_enabled.read(settings);
}

} // namespace fv::update
