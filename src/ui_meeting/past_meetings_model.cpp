#include "ui_meeting/past_meetings_model.h"

#include <algorithm>

#include "meeting_state/content.h"
#include "meeting_state/time_utils.h"
#include "pyport/text.h"
#include "ui_meeting/detail/payload.h"

namespace fv::past_meetings {

namespace {

using ui_meeting::detail::dictOrEmpty;
using ui_meeting::detail::get;
using ui_meeting::detail::strOrEmpty;
using ui_meeting::detail::truthy;

std::string status_of(const Json& meeting) {
    return py::lower(strOrEmpty(get(meeting, "status")));
}

// dict(meeting.get("content_summary") or {}).
Json content_summary_of(const Json& meeting) {
    return dictOrEmpty(get(meeting, "content_summary"));
}

} // namespace

bool is_historical(const Json& meeting) {
    const std::string status = status_of(meeting);
    return status != "active" && status != "paused" && status != "ending";
}

std::vector<Json> historical_meetings(const std::vector<Json>& rows,
                                      const std::function<Json(const std::string& id)>& summarize) {
    std::vector<Json> meetings;
    for (const Json& row : rows) {
        if (!is_historical(row)) {
            continue;
        }
        Json meeting = row.is_object() ? row : dictOrEmpty(row);
        if (!meeting.contains("content_summary") && summarize) {
            meeting["content_summary"] = summarize(strOrEmpty(get(meeting, "id")));
        }
        meetings.push_back(std::move(meeting));
    }
    return meetings;
}

std::string filter_haystack(const Json& meeting, const Clock& clock) {
    const auto pill = meeting_state::meeting_insights_pill(meeting);
    const std::string parts[] = {
        meeting_state::fallback_meeting_title(meeting),
        meeting_state::format_meeting_started_at(get(meeting, "started_at"), clock),
        meeting_state::format_meeting_duration(meeting, clock),
        meeting_state::meeting_preview_text(meeting),
        pill ? pill->first : std::string(),
        strOrEmpty(get(meeting, "status")),
    };
    std::string haystack;
    for (const std::string& part : parts) {
        if (part.empty()) {
            continue;
        }
        if (!haystack.empty()) {
            haystack += ' ';
        }
        haystack += part;
    }
    return py::lower(haystack);
}

std::vector<Json> filter_meetings(const std::vector<Json>& meetings, std::string_view query,
                                  const Clock& clock) {
    const std::string needle = py::lower(py::strip(query));
    if (needle.empty()) {
        return meetings;
    }
    std::vector<Json> matches;
    for (const Json& meeting : meetings) {
        if (filter_haystack(meeting, clock).find(needle) != std::string::npos) {
            matches.push_back(meeting);
        }
    }
    return matches;
}

std::string count_text(int shown, bool has_more) {
    const bool plus = has_more && shown == kMaxMeetings;
    return std::to_string(shown) + (plus ? "+" : "") +
           (shown == 1 && !plus ? " meeting" : " meetings");
}

ListView list_view(const std::vector<Json>& meetings, bool source_filtered, bool has_more,
                   std::string_view query, const Clock& clock) {
    ListView view;
    const std::vector<Json> shown_meetings =
        source_filtered ? meetings : filter_meetings(meetings, query, clock);
    const std::string typed = py::strip(query);
    const std::size_t total = meetings.size();
    const int shown = static_cast<int>(std::min<std::size_t>(shown_meetings.size(), kMaxMeetings));
    view.count = count_text(shown, has_more);
    view.count_visible = shown > 0;

    if (meetings.empty()) {
        view.placeholder = "No past meetings yet";
        return view;
    }
    if (!typed.empty() && shown_meetings.empty()) {
        view.placeholder = "No matching meetings";
        return view;
    }
    view.cards.assign(shown_meetings.begin(),
                      shown_meetings.begin() + static_cast<std::ptrdiff_t>(shown));
    if (has_more || shown_meetings.size() > static_cast<std::size_t>(kMaxMeetings)) {
        view.footer = "Showing the first 100 matches — refine your search to find older meetings";
    } else if (typed.empty() && total > static_cast<std::size_t>(kMaxMeetings)) {
        view.footer =
            "Showing 100 of " + std::to_string(total) + " — search to find older meetings";
    }
    return view;
}

std::string content_note(const Json& meeting) {
    const std::string status = status_of(meeting);
    const Json content = content_summary_of(meeting);
    const auto flag = [&content](const char* key) -> const Json& {
        return get(content, key);
    };
    if (status == "failed") {
        return "Meeting failed to start";
    }
    if (flag("is_empty").is_boolean() && flag("is_empty").get<bool>()) {
        return "No audio or transcript captured";
    }
    if (flag("has_transcript").is_boolean() && !flag("has_transcript").get<bool>()) {
        return "No transcript captured";
    }
    if (flag("has_audio").is_boolean() && !flag("has_audio").get<bool>()) {
        return "No audio captured";
    }
    return {};
}

std::string detail_text(const Json& meeting, const Clock& clock) {
    const std::string status = status_of(meeting);
    std::string duration = meeting_state::format_meeting_duration(meeting, clock);
    if (status.empty() || status == "ended") {
        return duration;
    }
    std::string spaced = status;
    std::replace(spaced.begin(), spaced.end(), '_', ' ');
    const std::string lifecycle = status == "failed" ? "Failed" : py::title(spaced);
    return duration.empty() ? lifecycle : duration + " · " + lifecycle;
}

bool has_transcript(const Json& meeting) {
    const Json summary = content_summary_of(meeting);
    if (summary.contains("has_transcript")) {
        return truthy(summary["has_transcript"]);
    }
    return !meeting_state::meeting_preview_text(meeting).empty();
}

bool has_audio(const Json& meeting) {
    if (!truthy(meeting)) {
        return false;
    }
    const Json summary = content_summary_of(meeting);
    if (summary.contains("has_audio")) {
        return truthy(summary["has_audio"]);
    }
    return !strOrEmpty(get(meeting, "spool_dir")).empty();
}

std::pair<std::string, std::string> meeting_location(const Json& meeting) {
    const std::string stored_on = strOrEmpty(get(meeting, "stored_on"));
    if (!stored_on.empty()) {
        return {"On " + stored_on, "Kept on " + stored_on + "; opening it downloads a copy"};
    }
    const std::string also_on = strOrEmpty(get(meeting, "also_on"));
    if (!also_on.empty()) {
        return {"Also on " + also_on, "Kept here and on " + also_on};
    }
    const std::string origin = strOrEmpty(get(meeting, "origin_device_name"));
    if (!origin.empty()) {
        return {"From " + origin, "Kept here for " + origin + ", a paired computer"};
    }
    return {};
}

} // namespace fv::past_meetings
