#include "meeting_state/time_utils.h"

#include <cmath>
#include <cstdint>
#include <string>

#include "meeting_state/detail/py_value.h"
#include "meeting_state/detail/time.h"
#include "pyport/errors.h"
#include "pyport/number.h"

namespace fv::meeting_state {

namespace {

using std::chrono::microseconds;

std::string replace_all(std::string text, std::string_view from, std::string_view to) {
    std::size_t at = 0;
    while ((at = text.find(from, at)) != std::string::npos) {
        text.replace(at, from.size(), to);
        at += to.size();
    }
    return text;
}

} // namespace

std::string utc_now_iso(const Clock& clock) {
    return replace_all(detail::utc_now(clock).isoformat(), "+00:00", "Z");
}

std::optional<py::DateTime> parse_meeting_time(const Json& value) {
    if (!detail::truthy(value)) {
        return std::nullopt;
    }
    // str(value): an integer such as 20260820 is a basic-format date.
    const std::string text = value.is_string() ? value.get<std::string>() : detail::py_str(value);
    try {
        return py::fromisoformat(replace_all(text, "Z", "+00:00"));
    } catch (const py::PyError&) {
        return std::nullopt;
    }
}

std::optional<py::DateTime> as_local_time(const Json& value, const Clock& clock) {
    return as_local_time(value, *clock.zone());
}

std::optional<py::DateTime> as_local_time(const Json& value,
                                          const std::chrono::time_zone& local_tz) {
    auto parsed = parse_meeting_time(value);
    if (!parsed || !parsed->utc_offset_us) {
        return parsed;
    }
    return detail::in_zone(detail::utc_instant(*parsed), local_tz);
}

std::optional<py::DateTime> as_local_time(const Json& value, std::chrono::seconds local_offset) {
    auto parsed = parse_meeting_time(value);
    if (!parsed || !parsed->utc_offset_us) {
        return parsed;
    }
    return detail::at_offset(detail::utc_instant(*parsed), local_offset);
}

std::optional<double> elapsed_seconds(const Json& started_at, const Json& ended_at,
                                      const Clock& clock) {
    const auto started = parse_meeting_time(started_at);
    const auto ended = parse_meeting_time(ended_at);
    if (!started || !ended) {
        return std::nullopt;
    }
    if (!started->utc_offset_us && !ended->utc_offset_us) {
        return detail::total_seconds(detail::wall(*ended) - detail::wall(*started));
    }
    // A mixed pair occurs across an upgrade: the naive side is local wall time, as
    // astimezone() reads it.
    const auto instant = [&](const py::DateTime& moment) {
        return moment.utc_offset_us ? detail::utc_instant(moment)
                                    : detail::local_instant(moment, *clock.zone());
    };
    return detail::total_seconds(instant(*ended) - instant(*started));
}

std::optional<double> seconds_since(const Json& value, const Clock& clock) {
    const auto parsed = parse_meeting_time(value);
    if (!parsed) {
        return std::nullopt;
    }
    if (!parsed->utc_offset_us) {
        return detail::total_seconds(detail::wall(detail::local_now(clock)) -
                                     detail::wall(*parsed));
    }
    return detail::total_seconds(detail::utc_instant(detail::utc_now(clock)) -
                                 detail::utc_instant(*parsed));
}

std::string format_meeting_started_at(const Json& value, const Clock& clock) {
    const auto started = as_local_time(value, clock);
    return started ? detail::format_started(*started) : std::string(UNKNOWN_DATE);
}

std::optional<double> meeting_duration_s(const Json& meeting, bool running, const Clock& clock) {
    std::optional<double> elapsed;
    if (running && !detail::truthy(detail::get(meeting, "ended_at"))) {
        elapsed = seconds_since(detail::get(meeting, "started_at"), clock);
    } else {
        elapsed = elapsed_seconds(detail::get(meeting, "started_at"),
                                  detail::get(meeting, "ended_at"), clock);
    }
    if (!elapsed) {
        return std::nullopt;
    }
    const Json& raw_paused = detail::get(meeting, "paused_total_s");
    double paused = 0.0;
    if (detail::truthy(raw_paused)) {
        paused = py::py_float(raw_paused).value_or(0.0);
    }
    const double remaining = *elapsed - paused;
    // max(0.0, x) keeps 0.0 unless x is greater, so NaN gives 0.0.
    return remaining > 0.0 ? remaining : 0.0;
}

std::string format_meeting_duration(const Json& meeting, const Clock& clock) {
    const auto duration = meeting_duration_s(meeting, false, clock);
    if (!duration) {
        return "";
    }
    if (std::isinf(*duration)) {
        throw py::OverflowError("cannot convert float infinity to integer");
    }
    const auto total = static_cast<std::int64_t>(*duration);
    const std::int64_t hours = total / 3600;
    const std::int64_t minutes = total % 3600 / 60;
    const std::int64_t remaining_seconds = total % 60;
    if (hours == 0 && minutes == 0) {
        return std::to_string(remaining_seconds) + " sec";
    }
    if (hours != 0) {
        return std::to_string(hours) + "h " + std::to_string(minutes) + "m";
    }
    return std::to_string(minutes) + " min";
}

std::string format_meeting_identity_meta(const Json& meeting, const Clock& clock) {
    std::string started = format_meeting_started_at(detail::get(meeting, "started_at"), clock);
    const std::string duration = format_meeting_duration(meeting, clock);
    if (duration.empty()) {
        return started;
    }
    return started + " \xC2\xB7 " + duration;
}

} // namespace fv::meeting_state
