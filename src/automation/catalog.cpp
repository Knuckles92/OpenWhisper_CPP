#include "automation/catalog.h"

#include <algorithm>
#include <utility>

#include "automation/action_model.h"
#include "automation/detail/pyvalue.h"
#include "pyport/number.h"
#include "pyport/text.h"

namespace fv::automation::catalog {

namespace {

Json text_or_null(std::string_view value) {
    return value.empty() ? Json(nullptr) : Json(std::string(value));
}

std::string job_id_of(const dictation::DictationJob* job) {
    return job_fields(job)["job_id"].get<std::string>();
}

} // namespace

const std::vector<std::pair<std::string_view, std::string_view>>& EVENT_TYPES() {
    static const std::vector<std::pair<std::string_view, std::string_view>> types = {
        {RECORDING_STARTED, "A recording started (dictation, Command Mode or an action)."},
        {RECORDING_STOPPED, "A recording stopped and is being transcribed."},
        {RECORDING_CANCELED, "A recording or transcription was canceled."},
        {TRANSCRIPTION_PREVIEW, "Live preview text while recording, at most four a second."},
        {TRANSCRIPTION_COMPLETED, "Finished text of a dictation, command, action or file."},
        {TRANSCRIPTION_FAILED, "A transcription or rewrite failed."},
        {HISTORY_SAVED, "A finished transcription was saved to history."},
        {ACTION_RAN, "An action ran, with whether it worked."},
        {CHANNEL_MESSAGE, "Words sent to Claude Code sessions listening on the channel."},
        {ROUTE_DECIDED, "Route by voice chose an action, or fell back to pasting."},
        {FILTER_FAILED, "A text filter timed out or answered badly; the text was kept."},
        {MEETING_STARTED, "A meeting started."},
        {MEETING_ENDED, "A meeting ended."},
        {MEETING_SEGMENTS, "New finished lines of a meeting transcript."},
        {MEETING_VOICE_COMMAND, "Someone gave the meeting note-taker a spoken command."},
        {EVENTS_GAP, "This stream missed events; fetch history to catch up."},
    };
    return types;
}

std::optional<std::string_view> describe_type(std::string_view type) {
    for (const auto& [name, description] : EVENT_TYPES()) {
        if (name == type) {
            return description;
        }
    }
    return std::nullopt;
}

const std::vector<std::string_view>& PUBLIC_TYPES() {
    static const std::vector<std::string_view> types = [] {
        std::vector<std::string_view> names;
        for (const auto& [name, description] : EVENT_TYPES()) {
            if (name != EVENTS_GAP) {
                names.push_back(name);
            }
        }
        return names;
    }();
    return types;
}

std::vector<std::string> known_types(const std::vector<std::string>& types) {
    std::vector<std::string> seen;
    for (const std::string& item : types) {
        if (describe_type(item) && item != EVENTS_GAP &&
            std::ranges::find(seen, item) == seen.end()) {
            seen.push_back(item);
        }
    }
    return seen;
}

Json action_ran(const dictation::DictationJob* job, const Action& action, bool ok,
                std::string_view message, std::string_view output,
                const std::optional<std::string>& text) {
    const Json fields = job_fields(job);
    Json data = Json::object();
    data["job_id"] = fields["job_id"];
    data["source"] = fields["source"];
    data["action_id"] = action.id;
    data["name"] = action.name;
    data["kind"] = action.kind;
    data["ok"] = ok;
    data["message"] = py::slice(message, std::nullopt, 500);
    data["output"] = output.empty() ? std::string("none") : std::string(output);
    if (text) {
        data["text"] = *text;
    }
    return data;
}

Json channel_message(const dictation::DictationJob* job, const Action& action,
                     std::string_view text, const Json& app) {
    const Json fields = job_fields(job);
    // A job that can't say counts as a program's.
    const bool users = job != nullptr && !job->program_words();
    Json data = Json::object();
    data["job_id"] = fields["job_id"];
    data["source"] = fields["source"];
    data["origin"] = std::string(users ? ORIGIN_DICTATED : ORIGIN_PROGRAM);
    data["action_id"] = action.id;
    data["text"] = std::string(text);
    data["app"] = app.is_object() && !app.empty() ? app : Json(nullptr);
    return data;
}

Json route_decided(const dictation::DictationJob* job, std::string_view action_id,
                   std::optional<double> confidence, bool fallback) {
    Json data = Json::object();
    data["job_id"] = job_id_of(job);
    data["action_id"] = text_or_null(action_id);
    data["confidence"] = confidence ? Json(py::round_ndigits(*confidence, 4)) : Json(nullptr);
    data["fallback"] = fallback;
    return data;
}

Json filter_failed(std::string_view filter_id, std::string_view name, std::string_view stage,
                   std::string_view reason) {
    Json data = Json::object();
    data["filter_id"] = std::string(filter_id);
    data["name"] = std::string(name);
    data["stage"] = std::string(stage);
    data["reason"] = py::slice(reason, std::nullopt, 200);
    return data;
}

Json meeting_started(std::string_view meeting_id, std::string_view title) {
    Json data = Json::object();
    data["meeting_id"] = std::string(meeting_id);
    data["title"] = std::string(title);
    return data;
}

Json meeting_ended(std::string_view meeting_id, std::string_view status) {
    Json data = Json::object();
    data["meeting_id"] = std::string(meeting_id);
    data["status"] = std::string(status);
    return data;
}

Json meeting_segments(std::string_view meeting_id, const Json& items) {
    Json rows = Json::array();
    if (items.is_array()) {
        for (const Json& item : items) {
            const auto get = [&](std::string_view key) -> const Json& {
                return detail::get(item, key);
            };
            // str(item.get("segment_id") or item.get("id") or "")
            const Json& segment_id =
                detail::truthy(get("segment_id")) ? get("segment_id") : get("id");
            Json row = Json::object();
            row["segment_id"] =
                detail::truthy(segment_id) ? detail::py_str(segment_id) : std::string();
            row["speaker"] = detail::truthy(get("speaker")) ? get("speaker") : Json("");
            row["text"] = detail::truthy(get("text")) ? get("text") : Json("");
            row["start_s"] = get("start_s");
            row["end_s"] = get("end_s");
            rows.push_back(std::move(row));
        }
    }
    Json data = Json::object();
    data["meeting_id"] = std::string(meeting_id);
    data["items"] = std::move(rows);
    return data;
}

Json meeting_voice_command(std::string_view meeting_id, std::string_view command) {
    Json data = Json::object();
    data["meeting_id"] = std::string(meeting_id);
    data["command"] = std::string(command);
    return data;
}

Json events_gap(std::int64_t missed_from_seq, std::string_view reason) {
    Json data = Json::object();
    data["missed_from_seq"] = missed_from_seq;
    data["reason"] = std::string(reason);
    return data;
}

} // namespace fv::automation::catalog
