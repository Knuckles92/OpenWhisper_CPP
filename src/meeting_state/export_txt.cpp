#include "meeting_state/export_txt.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <format>

#include "meeting_state/detail/extras.h"
#include "meeting_state/detail/py_value.h"
#include "meeting_state/time_utils.h"
#include "pyport/errors.h"
#include "pyport/number.h"
#include "pyport/text.h"

namespace fv::meeting_state {

namespace {

using detail::get;
using detail::truthy;

// max(0, int(seconds)).
std::int64_t whole_seconds(double seconds) {
    if (std::isnan(seconds)) {
        throw py::ValueError("cannot convert float NaN to integer");
    }
    if (std::isinf(seconds)) {
        throw py::OverflowError("cannot convert float infinity to integer");
    }
    return std::max<std::int64_t>(0, static_cast<std::int64_t>(std::trunc(seconds)));
}

// float(value or 0.0), as the segment readers take start_s.
double seconds_of(const Json& value) {
    if (!truthy(value)) {
        return 0.0;
    }
    if (const auto parsed = py::py_float(value)) {
        return *parsed;
    }
    if (value.is_string()) {
        throw py::ValueError("could not convert string to float");
    }
    throw py::TypeError("float() argument must be a string or a real number");
}

} // namespace

std::string format_clock(double seconds) {
    const std::int64_t total = whole_seconds(seconds);
    return std::format("{:02}:{:02}:{:02}", total / 3600, total % 3600 / 60, total % 60);
}

std::string format_mmss(double seconds) {
    const std::int64_t total = whole_seconds(seconds);
    return std::format("{:02}:{:02}", total / 60, total % 60);
}

std::string resolve_title(const Json& meeting, const Json& state, const Clock& clock) {
    std::string title = py::strip(detail::text_or_empty(get(meeting, "title")));
    if (title.empty()) {
        title = py::strip(detail::text_or_empty(get(state, "title")));
    }
    if (!title.empty()) {
        return title;
    }
    if (const auto started = as_local_time(get(meeting, "started_at"), clock)) {
        return std::format("Meeting {:04}-{:02}-{:02}", started->year, started->month,
                           started->day);
    }
    return "Meeting";
}

std::optional<std::string> format_meeting_date(const Json& meeting, const Clock& clock) {
    if (const auto started = as_local_time(get(meeting, "started_at"), clock)) {
        return std::format("{:04}-{:02}-{:02} {:02}:{:02}", started->year, started->month,
                           started->day, started->hour, started->minute);
    }
    const Json& raw = get(meeting, "started_at");
    if (truthy(raw)) {
        return detail::py_str(raw);
    }
    return std::nullopt;
}

std::vector<std::string> participant_names(const Json& state) {
    const Json& participants = get(state, "participants");
    std::vector<const Json*> people;
    if (truthy(participants)) {
        for (const auto& [id, person] : detail::mapping(participants).items()) {
            people.push_back(&person);
        }
    }
    std::stable_sort(people.begin(), people.end(), [](const Json* a, const Json* b) {
        const auto rank = [](const Json* p) {
            const Json& kind = get(detail::mapping(*p), "kind");
            return kind.is_string() && kind.get_ref<const std::string&>() == "me" ? 0 : 1;
        };
        return rank(a) < rank(b);
    });
    std::vector<std::string> names;
    for (const Json* person : people) {
        std::string name = py::strip(detail::text_or_empty(get(*person, "display_name")));
        if (!name.empty()) {
            names.push_back(std::move(name));
        }
    }
    return names;
}

std::string speaker_name(const Json& segment, const Json& participants) {
    const Json& pid = get(segment, "speaker_participant_id");
    if (truthy(pid)) {
        if (pid.is_array() || pid.is_object()) {
            throw py::TypeError("unhashable type");
        }
        const Json& participant =
            pid.is_string() ? get(participants, pid.get_ref<const std::string&>()) : Json();
        if (truthy(participant)) {
            std::string name =
                py::strip(detail::text_or_empty(get(detail::mapping(participant), "display_name")));
            if (!name.empty()) {
                return name;
            }
        }
    }
    const Json& channel = get(segment, "channel");
    if (channel.is_string() && channel.get_ref<const std::string&>() == "mic") {
        return std::string(FALLBACK_SPEAKER_MIC);
    }
    return std::string(FALLBACK_SPEAKER_LOOPBACK);
}

std::vector<std::string> transcript_lines(const std::vector<Json>& segments,
                                          const Json& participants) {
    std::vector<std::string> lines;
    for (const Json& segment : segments) {
        const std::string text =
            py::strip(detail::text_or_empty(get(detail::mapping(segment), "text")));
        if (text.empty()) {
            continue;
        }
        const std::string stamp = format_clock(seconds_of(get(segment, "start_s")));
        lines.push_back("[" + stamp + "] " + speaker_name(segment, participants) + ": " + text);
    }
    return lines;
}

std::string export_transcript_txt(const Json& meeting, const Json& state,
                                  const std::vector<Json>& segments, const Clock& clock) {
    std::vector<std::string> header{resolve_title(meeting, state, clock)};
    if (auto date = format_meeting_date(meeting, clock)) {
        header.push_back(std::move(*date));
    }
    const auto names = participant_names(state);
    if (!names.empty()) {
        header.push_back("Participants: " + detail::join(names, ", "));
    }
    const Json& participants = get(state, "participants");
    const auto lines =
        transcript_lines(segments, truthy(participants) ? participants : Json::object());
    const std::string body =
        lines.empty() ? std::string("(no transcript)") : detail::join(lines, "\n");
    return detail::join(header, "\n") + "\n\n" + body + "\n";
}

} // namespace fv::meeting_state
