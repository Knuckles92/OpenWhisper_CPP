#include "agentapi/history_models.h"

#include <cmath>
#include <string_view>
#include <utility>

#include "pyport/number.h"
#include "pyport/text.h"

namespace fv::agentapi {

namespace {

Json text_or_null(const std::optional<std::string>& value) {
    return value ? Json(*value) : Json(nullptr);
}

// pydantic's JSON mode writes inf and nan as null (ser_json_inf_nan="null").
Json number_or_null(const std::optional<double>& value) {
    if (!value || !std::isfinite(*value)) {
        return nullptr;
    }
    return *value;
}

Json number(double value) {
    return number_or_null(value);
}

Json strings(const std::vector<std::string>& values) {
    Json out = Json::array();
    for (const auto& value : values) {
        out.push_back(value);
    }
    return out;
}

template <class T> Json models(const std::vector<T>& values) {
    Json out = Json::array();
    for (const auto& value : values) {
        out.push_back(value.to_json());
    }
    return out;
}

[[noreturn]] void invalid(std::string_view field) {
    throw ModelError("invalid value for " + std::string(field));
}

const Json* field(const Json& object, std::string_view name) {
    const auto it = object.find(std::string(name));
    return it == object.end() ? nullptr : &*it;
}

std::string required_text(const Json& object, std::string_view name) {
    const Json* value = field(object, name);
    if (value == nullptr || !value->is_string()) {
        invalid(name);
    }
    return value->get<std::string>();
}

std::string text_or(const Json& object, std::string_view name, std::string fallback) {
    const Json* value = field(object, name);
    if (value == nullptr) {
        return fallback;
    }
    if (!value->is_string()) {
        invalid(name);
    }
    return value->get<std::string>();
}

std::optional<std::string> optional_text(const Json& object, std::string_view name) {
    const Json* value = field(object, name);
    if (value == nullptr || value->is_null()) {
        return std::nullopt;
    }
    if (!value->is_string()) {
        invalid(name);
    }
    return value->get<std::string>();
}

bool ascii_only(std::string_view text) {
    for (const char c : text) {
        if (static_cast<unsigned char>(c) >= 0x80) {
            return false;
        }
    }
    return true;
}

// pydantic's float in lax mode.
std::optional<double> lax_float(const Json& value, std::string_view name) {
    if (value.is_null()) {
        return std::nullopt;
    }
    if (value.is_boolean()) {
        return value.get<bool>() ? 1.0 : 0.0;
    }
    if (value.is_number()) {
        return value.get<double>();
    }
    if (value.is_string()) {
        const std::string stripped = py::strip(value.get<std::string>());
        if (!stripped.empty() && ascii_only(stripped)) {
            if (const auto parsed = py::py_float(stripped)) {
                return parsed;
            }
        }
    }
    invalid(name);
}

// pydantic's bool in lax mode.
bool lax_bool(const Json& value, std::string_view name) {
    if (value.is_boolean()) {
        return value.get<bool>();
    }
    if (value.is_number_integer()) {
        if (value == 0) {
            return false;
        }
        if (value == 1) {
            return true;
        }
    } else if (value.is_number_float()) {
        const double number = value.get<double>();
        if (number == 0.0) {
            return false;
        }
        if (number == 1.0) {
            return true;
        }
    } else if (value.is_string()) {
        std::string text = value.get<std::string>();
        for (char& c : text) {
            if (c >= 'A' && c <= 'Z') {
                c = static_cast<char>(c - 'A' + 'a');
            }
        }
        for (const std::string_view yes : {"true", "1", "yes", "on", "t", "y"}) {
            if (text == yes) {
                return true;
            }
        }
        for (const std::string_view no : {"false", "0", "no", "off", "f", "n"}) {
            if (text == no) {
                return false;
            }
        }
    }
    invalid(name);
}

std::vector<std::string> string_list(const Json& object, std::string_view name) {
    const Json* value = field(object, name);
    if (value == nullptr) {
        return {};
    }
    if (!value->is_array()) {
        invalid(name);
    }
    std::vector<std::string> out;
    for (const Json& item : *value) {
        if (!item.is_string()) {
            invalid(name);
        }
        out.push_back(item.get<std::string>());
    }
    return out;
}

void require_object(const Json& value) {
    if (!value.is_object()) {
        throw ModelError("expected an object");
    }
}

} // namespace

Json ClientStatus::to_json() const {
    Json out = Json::object();
    out["device_id"] = device_id;
    out["name"] = name;
    out["status"] = status;
    return out;
}

Json ApiStatus::to_json() const {
    Json out = Json::object();
    out["api_version"] = "1";
    out["read_only"] = true;
    out["clients"] = models(clients);
    return out;
}

Json TranscriptionSummary::to_json() const {
    Json out = Json::object();
    out["id"] = id;
    out["device_id"] = text_or_null(device_id);
    out["title"] = text_or_null(title);
    out["timestamp"] = timestamp;
    out["model"] = model;
    out["source_name"] = text_or_null(source_name);
    out["audio_duration"] = number_or_null(audio_duration);
    out["origin_device_id"] = text_or_null(origin_device_id);
    out["origin_device_name"] = text_or_null(origin_device_name);
    out["preview"] = preview;
    return out;
}

Json Transcription::to_json() const {
    Json out = TranscriptionSummary::to_json();
    out["text"] = text;
    out["raw_text"] = text_or_null(raw_text);
    out["cleanup_provider"] = text_or_null(cleanup_provider);
    out["cleanup_model"] = text_or_null(cleanup_model);
    return out;
}

Json Meeting::to_json() const {
    Json out = Json::object();
    out["id"] = id;
    out["device_id"] = text_or_null(device_id);
    out["title"] = title;
    out["status"] = status;
    out["started_at"] = started_at;
    out["ended_at"] = text_or_null(ended_at);
    out["state_seq"] = state_seq;
    out["origin_device_id"] = text_or_null(origin_device_id);
    out["origin_device_name"] = text_or_null(origin_device_name);
    return out;
}

Json Segment::to_json() const {
    Json out = Json::object();
    out["id"] = id;
    out["device_id"] = text_or_null(device_id);
    out["meeting_id"] = meeting_id;
    out["start_s"] = number(start_s);
    out["end_s"] = number(end_s);
    out["channel"] = channel;
    out["text"] = text;
    out["speaker_participant_id"] = text_or_null(speaker_participant_id);
    out["speaker_source"] = speaker_source;
    return out;
}

Json Participant::to_json() const {
    Json out = Json::object();
    out["id"] = id;
    out["display_name"] = display_name;
    out["kind"] = kind;
    return out;
}

Json InsightData::to_json() const {
    Json out = Json::object();
    out["heading"] = text_or_null(heading);
    out["start_s"] = number_or_null(start_s);
    out["owner_participant_id"] = text_or_null(owner_participant_id);
    out["due_date"] = text_or_null(due_date);
    out["severity"] = text_or_null(severity);
    return out;
}

Json Insight::to_json() const {
    Json out = Json::object();
    out["id"] = id;
    out["card"] = card;
    out["text"] = text;
    out["status"] = status;
    out["pinned"] = pinned;
    out["data"] = data.to_json();
    out["evidence"] = strings(evidence);
    return out;
}

Json Question::to_json() const {
    Json out = Json::object();
    out["id"] = id;
    out["text"] = text;
    out["status"] = status;
    out["answer"] = text_or_null(answer);
    out["answer_source"] = text_or_null(answer_source);
    out["evidence"] = strings(evidence);
    return out;
}

Json Report::to_json() const {
    Json out = Json::object();
    out["id"] = id;
    out["title"] = title;
    out["request"] = request;
    out["status"] = status;
    out["markdown"] = markdown;
    return out;
}

Json MeetingInsights::to_json() const {
    Json out = Json::object();
    out["meeting_id"] = meeting_id;
    out["device_id"] = text_or_null(device_id);
    out["state_seq"] = state_seq;
    out["snapshot_available"] = snapshot_available;
    out["rolling_summary"] = rolling_summary;
    out["rolling_summary_evidence"] = strings(rolling_summary_evidence);
    out["participants"] = models(participants);
    out["cards"] = models(cards);
    out["questions"] = models(questions);
    out["custom_reports"] = models(custom_reports);
    return out;
}

Json SearchHit::to_json() const {
    Json out = Json::object();
    out["kind"] = kind;
    out["id"] = id;
    out["device_id"] = text_or_null(device_id);
    out["origin_device_name"] = text_or_null(origin_device_name);
    out["meeting_id"] = text_or_null(meeting_id);
    out["title"] = title;
    out["timestamp"] = timestamp;
    out["excerpt"] = excerpt;
    out["matched_field"] = matched_field;
    out["start_s"] = number_or_null(start_s);
    out["end_s"] = number_or_null(end_s);
    out["origin_device_id"] = text_or_null(origin_device_id);
    out["resource"] = resource;
    return out;
}

Json page_json(Json items, const std::optional<std::string>& next_cursor,
               const std::vector<ClientStatus>& clients) {
    Json out = Json::object();
    out["items"] = std::move(items);
    out["next_cursor"] = text_or_null(next_cursor);
    out["clients"] = models(clients);
    return out;
}

Participant participant_from(const Json& value) {
    require_object(value);
    Participant out;
    out.id = required_text(value, "id");
    out.display_name = required_text(value, "display_name");
    out.kind = text_or(value, "kind", out.kind);
    return out;
}

InsightData insight_data_from(const Json& value) {
    require_object(value);
    InsightData out;
    out.heading = optional_text(value, "heading");
    if (const Json* start = field(value, "start_s")) {
        out.start_s = lax_float(*start, "start_s");
    }
    out.owner_participant_id = optional_text(value, "owner_participant_id");
    out.due_date = optional_text(value, "due_date");
    out.severity = optional_text(value, "severity");
    return out;
}

Insight insight_from(const Json& value) {
    require_object(value);
    Insight out;
    out.id = required_text(value, "id");
    out.card = required_text(value, "card");
    out.text = text_or(value, "text", out.text);
    out.status = text_or(value, "status", out.status);
    if (const Json* pinned = field(value, "pinned")) {
        out.pinned = lax_bool(*pinned, "pinned");
    }
    if (const Json* data = field(value, "data")) {
        out.data = insight_data_from(*data);
    }
    out.evidence = string_list(value, "evidence");
    return out;
}

Question question_from(const Json& value) {
    require_object(value);
    Question out;
    out.id = required_text(value, "id");
    out.text = text_or(value, "text", out.text);
    out.status = text_or(value, "status", out.status);
    out.answer = optional_text(value, "answer");
    out.answer_source = optional_text(value, "answer_source");
    out.evidence = string_list(value, "evidence");
    return out;
}

Report report_from(const Json& value) {
    require_object(value);
    Report out;
    out.id = required_text(value, "id");
    out.title = text_or(value, "title", out.title);
    out.request = text_or(value, "request", out.request);
    out.status = text_or(value, "status", out.status);
    out.markdown = text_or(value, "markdown", out.markdown);
    return out;
}

} // namespace fv::agentapi
