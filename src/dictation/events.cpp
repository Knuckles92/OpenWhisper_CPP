#include "dictation/events.h"

#include <array>

#include "pyport/number.h"
#include "pyport/text.h"

namespace fv::dictation::events {

namespace {

// Modes whose raw_text is the words before AI cleanup. A command's or transform's original is
// the user's selected text, which events never carry.
constexpr std::array<std::string_view, 4> kRawTextModes = {"dictation", "action", "walkthrough",
                                                           FILE_MODE};

Json string_or_null(std::string_view value) {
    return value.empty() ? Json(nullptr) : Json(std::string(value));
}

std::string text_field(const Json& fields, const char* key) {
    if (!fields.is_object()) {
        return {};
    }
    const auto found = fields.find(key);
    if (found == fields.end() || !found->is_string()) {
        return {};
    }
    return found->get<std::string>();
}

} // namespace

Json job_fields(const DictationJob* job) {
    Json fields = Json::object();
    if (job == nullptr) {
        fields["job_id"] = "";
        fields["mode"] = std::string(FILE_MODE);
        fields["action_id"] = nullptr;
        fields["source"] = "";
        fields["route"] = "";
        return fields;
    }
    fields["job_id"] = job->id;
    fields["mode"] = std::string(to_string(job->mode));
    fields["action_id"] = string_or_null(job->action_id);
    fields["source"] = job->source;
    fields["route"] = job->route;
    return fields;
}

Json app_from_fields(const Json& fields) {
    const std::string app_id = text_field(fields, "app_id");
    if (app_id.empty()) {
        return nullptr;
    }
    Json app = Json::object();
    app["id"] = app_id;
    app["name"] = text_field(fields, "app_name");
    app["category"] = text_field(fields, "app_category");
    return app;
}

Json recording_started(const DictationJob* job) {
    return job_fields(job);
}

Json recording_stopped(const DictationJob* job, std::optional<double> duration_s) {
    Json data = Json::object();
    data["job_id"] = job_fields(job)["job_id"];
    data["duration_s"] = duration_s ? Json(py::round_ndigits(*duration_s, 3)) : Json(nullptr);
    return data;
}

Json recording_canceled(const DictationJob* job) {
    Json data = Json::object();
    data["job_id"] = job_fields(job)["job_id"];
    return data;
}

Json transcription_preview(const DictationJob* job, std::string_view text, bool final) {
    Json data = Json::object();
    data["job_id"] = job_fields(job)["job_id"];
    data["text"] = std::string(text);
    data["final"] = final;
    return data;
}

Json transcription_completed(const DictationJob* job, std::string_view text,
                             const std::optional<std::string>& raw_text, const Json& fields,
                             const std::optional<CleanupInfo>& cleanup,
                             std::optional<double> audio_s) {
    const Json base = job_fields(job);
    const auto mode = base["mode"].get<std::string>();
    bool raw_mode = false;
    for (const auto candidate : kRawTextModes) {
        raw_mode = raw_mode || candidate == mode;
    }
    Json data = Json::object();
    for (const char* key : {"job_id", "mode", "action_id", "source"}) {
        data[key] = base[key];
    }
    data["routed"] = job != nullptr && job->routed;
    data["text"] = std::string(text);
    data["raw_text"] = raw_mode && raw_text && !raw_text->empty() && *raw_text != text
                           ? Json(*raw_text)
                           : Json(nullptr);
    data["language"] = text_field(fields, "language");
    data["app"] = app_from_fields(fields);
    if (cleanup) {
        Json info = Json::object();
        info["provider"] = cleanup->provider;
        info["model"] = cleanup->model;
        info["level"] = cleanup->level;
        data["cleanup"] = std::move(info);
    } else {
        data["cleanup"] = nullptr;
    }
    // Python's `if audio_s`: zero is no duration either.
    data["audio_s"] =
        audio_s && *audio_s != 0.0 ? Json(py::round_ndigits(*audio_s, 3)) : Json(nullptr);
    return data;
}

Json transcription_failed(const DictationJob* job, std::string_view error,
                          std::optional<std::string_view> message) {
    std::string_view category = FAILURE_FAILED;
    for (const auto known : {FAILURE_CANCELED, FAILURE_NO_SPEECH, FAILURE_REFUSED}) {
        if (error == known) {
            category = known;
        }
    }
    Json data = Json::object();
    data["job_id"] = job_fields(job)["job_id"];
    data["error"] = std::string(category);
    data["message"] = category == FAILURE_REFUSED && message && !message->empty()
                          ? Json(py::slice(*message, std::nullopt, 500))
                          : Json(nullptr);
    return data;
}

Json history_saved(std::string_view job_id, std::string_view record_id, std::string_view kind) {
    Json data = Json::object();
    data["job_id"] = std::string(job_id);
    data["record_id"] = std::string(record_id);
    data["kind"] = std::string(kind);
    return data;
}

} // namespace fv::dictation::events
