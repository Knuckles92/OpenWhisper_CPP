#include "settings/enums.h"

#include <map>
#include <stdexcept>

#include "pyport/json.h"
#include "settings/data/enums.json.h"

namespace fv::enums {

namespace {

struct Record {
    Json values = Json::array();
    std::vector<Entry> labels;
    std::vector<Entry> details;
    std::vector<std::pair<std::string, std::string>> aliases;
    Json installed = Json::array();
    Json legacy = Json::array();
};

const Json& document() {
    static const Json parsed = Json::parse(embedded::settings_enums);
    return parsed;
}

std::vector<Entry> entries(const Json& pairs) {
    std::vector<Entry> out;
    for (const auto& pair : pairs) {
        out.push_back({pair.at(0), pair.at(1).get<std::string>()});
    }
    return out;
}

const Record& record(EnumId id) {
    static const auto records = [] {
        std::map<EnumId, Record> map;
        const Json& enums = document().at("enums");
        for (const EnumId each : kAllEnums) {
            const Json& source = enums.at(std::string(name(each)));
            Record r;
            r.values = source.value("values", Json::array());
            r.labels = entries(source.value("labels", Json::array()));
            r.details = entries(source.value("details", Json::array()));
            for (const auto& pair : source.value("aliases", Json::array())) {
                r.aliases.emplace_back(pair.at(0).get<std::string>(),
                                       pair.at(1).get<std::string>());
            }
            r.installed = source.value("installed", Json::array());
            r.legacy = source.value("legacy", Json::array());
            map.emplace(each, std::move(r));
        }
        return map;
    }();
    return records.at(id);
}

} // namespace

std::string_view name(EnumId id) {
    switch (id) {
    case EnumId::RecordingRetentionMode:
        return "RecordingRetentionMode";
    case EnumId::RecordingTriggerMode:
        return "RecordingTriggerMode";
    case EnumId::UiFontScale:
        return "UiFontScale";
    case EnumId::UiTheme:
        return "UiTheme";
    case EnumId::WalkthroughCopyFormat:
        return "WalkthroughCopyFormat";
    case EnumId::WalkthroughSnapMode:
        return "WalkthroughSnapMode";
    case EnumId::WalkthroughPageSize:
        return "WalkthroughPageSize";
    case EnumId::RecordingOverlayLook:
        return "RecordingOverlayLook";
    case EnumId::StreamingOverlayLayout:
        return "StreamingOverlayLayout";
    case EnumId::TranscriptCleanupProvider:
        return "TranscriptCleanupProvider";
    case EnumId::TranscriptCleanupLevel:
        return "TranscriptCleanupLevel";
    case EnumId::TranscriptCleanupModelSort:
        return "TranscriptCleanupModelSort";
    case EnumId::TranscriptCleanupReasoning:
        return "TranscriptCleanupReasoning";
    case EnumId::MeetingAgentCore:
        return "MeetingAgentCore";
    case EnumId::TypeSafeProvider:
        return "TypeSafeProvider";
    case EnumId::TypeSafeTextModel:
        return "TypeSafeTextModel";
    case EnumId::MeetingSpeakerIdBackend:
        return "MeetingSpeakerIdBackend";
    case EnumId::MeetingLanguage:
        return "MeetingLanguage";
    case EnumId::MeetingServerBind:
        return "MeetingServerBind";
    case EnumId::HuggingFaceAccessPolicy:
        return "HuggingFaceAccessPolicy";
    }
    throw std::invalid_argument("unknown settings enum");
}

const Json& values(EnumId id) {
    return record(id).values;
}

const std::vector<Entry>& labels(EnumId id) {
    return record(id).labels;
}

const std::vector<Entry>& details(EnumId id) {
    return record(id).details;
}

const std::vector<std::pair<std::string, std::string>>& aliases(EnumId id) {
    return record(id).aliases;
}

const Json& installed(EnumId id) {
    return record(id).installed;
}

const Json& legacy(EnumId id) {
    return record(id).legacy;
}

std::optional<std::string> label(EnumId id, const Json& value) {
    for (const Entry& entry : labels(id)) {
        if (py::py_equal(entry.value, value)) {
            return entry.text;
        }
    }
    return std::nullopt;
}

const Json& constants() {
    return document().at("constants");
}

const std::vector<std::string>& default_voice_command_names() {
    static const auto names =
        constants().at("DEFAULT_VOICE_COMMAND_NAMES").get<std::vector<std::string>>();
    return names;
}

} // namespace fv::enums
