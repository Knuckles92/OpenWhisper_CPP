#include "meeting_state/content.h"

#include <exception>
#include <vector>

#include "base/log.h"
#include "meeting_state/detail/py_value.h"
#include "meeting_state/patches.h"
#include "meeting_state/sql_repository.h"
#include "meeting_state/stored.h"
#include "pyport/errors.h"
#include "pyport/number.h"
#include "pyport/text.h"

namespace fv::meeting_state {

namespace {

constexpr log::Logger logger("meeting.content");

using detail::get;
using detail::truthy;

// str(value or "").strip()
std::string stripped_text(const Json& value) {
    return truthy(value) ? py::strip(detail::py_str(value)) : std::string();
}

// The row's persisted state mapping: its "state" object, else state_json.
Json meeting_state_dict(const Json& meeting) {
    const Json& state = get(meeting, "state");
    if (state.is_object()) {
        return state;
    }
    return stored_state_dict(meeting);
}

// (meeting.get(key) or {}): a truthy value that is not a mapping has no .get (AttributeError).
const Json& mapping_or_empty(const Json& meeting, std::string_view key) {
    static const Json empty = Json::object();
    const Json& value = get(meeting, key);
    if (!truthy(value)) {
        return empty;
    }
    if (!value.is_object()) {
        throw py::PyError("AttributeError", "object has no attribute 'get'");
    }
    return value;
}

// float(value or 0.0)
double duration_of(const Json& value) {
    if (!truthy(value)) {
        return 0.0;
    }
    const std::optional<double> number = py::py_float(value);
    if (!number) {
        throw py::ValueError("could not convert to float");
    }
    return *number;
}

} // namespace

std::string meeting_display_title(const Json& meeting) {
    std::string title = stripped_text(get(meeting, "title"));
    if (!title.empty()) {
        return title;
    }
    const Json state = meeting_state_dict(meeting);
    std::string state_title = stripped_text(get(state, "title"));
    if (!state_title.empty()) {
        return state_title;
    }
    const Json& topic = get(state, "topic");
    if (topic.is_object()) {
        return stripped_text(get(topic, "current"));
    }
    return "";
}

std::string fallback_meeting_title(const Json& meeting) {
    std::string display = meeting_display_title(meeting);
    if (!display.empty()) {
        return display;
    }
    const Json& status = get(meeting, "status");
    const std::string lowered = truthy(status) ? py::lower(detail::py_str(status)) : std::string();
    return lowered == "failed" ? "Failed meeting" : "Untitled meeting";
}

std::string untitled_title_from_topic(const Json& state) {
    if (!stripped_text(get(state, "title")).empty()) {
        return "";
    }
    const Json& topic = get(state, "topic");
    if (!topic.is_object()) {
        return "";
    }
    return py::slice(stripped_text(get(topic, "current")), 0,
                     static_cast<std::int64_t>(MAX_NAME_LEN));
}

Json build_untitled_title_ops(const Json& state) {
    const std::string text = untitled_title_from_topic(state);
    Json ops = Json::array();
    if (!text.empty()) {
        ops.push_back({{"op", "set_title"}, {"text", text}});
    }
    return ops;
}

std::optional<std::pair<std::string, std::string>> meeting_insights_pill(const Json& meeting) {
    const Json& status = get(meeting, "status");
    if (truthy(status) && py::lower(detail::py_str(status)) == "failed") {
        return std::make_pair(std::string("Failed start"), std::string("warning"));
    }
    if (truthy(get(mapping_or_empty(meeting, "content_summary"), "is_empty"))) {
        return std::make_pair(std::string("Empty"), std::string("warning"));
    }
    try {
        const FinalizationState fin = finalization_from_meeting_row(meeting);
        return fin.history_pill(truthy(status) ? detail::py_str(status) : std::string("ended"));
    } catch (const std::exception& error) {
        logger.debug("Could not derive insights pill for a past meeting: {}", error.what());
        return std::nullopt;
    }
}

Json summarize_meeting_content(MeetingRepository& repository, std::string_view meeting_id) {
    if (auto* sql = dynamic_cast<SqlMeetingRepository*>(&repository)) {
        return sql->get_meeting_content_summary(meeting_id);
    }
    std::vector<Json> chunks;
    std::vector<Json> segments;
    try {
        chunks = repository.get_audio_chunks(meeting_id);
    } catch (const std::exception& error) {
        logger.error_with(error, "Could not inspect audio for meeting " + std::string(meeting_id));
    }
    try {
        segments = repository.get_segments(meeting_id, -1.0, std::nullopt);
    } catch (const std::exception& error) {
        logger.error_with(error,
                          "Could not inspect transcript for meeting " + std::string(meeting_id));
    }
    std::int64_t audio_chunks = 0;
    bool has_loopback_audio = false;
    for (const auto& chunk : chunks) {
        if (duration_of(get(chunk, "duration_s")) > 0.0) {
            ++audio_chunks;
            const Json& channel = get(chunk, "channel");
            if (truthy(channel) && detail::py_str(channel) == "loopback") {
                has_loopback_audio = true;
            }
        }
    }
    std::vector<std::string> texts;
    for (const auto& segment : segments) {
        std::string text = stripped_text(get(segment, "text"));
        if (!text.empty()) {
            texts.push_back(std::move(text));
        }
    }
    std::string preview;
    std::size_t used = 0;
    for (const auto& text : texts) {
        preview += preview.empty() ? text : " " + text;
        used += py::len(text);
        if (used >= PREVIEW_LIMIT) {
            break;
        }
    }
    preview = py::strip(preview);
    if (py::len(preview) > PREVIEW_LIMIT) {
        preview = py::rstrip(py::slice(preview, 0, static_cast<std::int64_t>(PREVIEW_LIMIT))) + "…";
    }
    const std::int64_t transcript_segments = static_cast<std::int64_t>(texts.size());
    Json summary = Json::object();
    summary["has_audio"] = audio_chunks > 0;
    summary["has_loopback_audio"] = has_loopback_audio;
    summary["has_transcript"] = transcript_segments > 0;
    summary["is_empty"] = audio_chunks == 0 && transcript_segments == 0;
    summary["audio_chunks"] = audio_chunks;
    summary["transcript_segments"] = transcript_segments;
    summary["can_rerun_speakers"] = has_loopback_audio;
    summary["preview_text"] = preview;
    return summary;
}

std::string meeting_preview_text(const Json& meeting) {
    const Json& summary = get(meeting, "content_summary");
    if (summary.is_object()) {
        std::string preview = stripped_text(get(summary, "preview_text"));
        if (!preview.empty()) {
            return preview;
        }
    }
    const Json state = meeting_state_dict(meeting);
    const Json& topic = get(state, "topic");
    if (topic.is_object()) {
        return stripped_text(get(topic, "current"));
    }
    return "";
}

} // namespace fv::meeting_state
