#include "remote/meeting_route.h"
#include "engine/error.h"
#include "models/speech_catalog.h"
#include "pyport/text.h"
#include "remote/pairing_settings.h"
#include "settings/keys.h"
namespace fv::remote {
namespace {
std::string field(const Json& object, const char* key) {
    const auto at = object.find(key);
    return at != object.end() && at->is_string() ? at->get<std::string>() : std::string();
}
} // namespace
Json meeting_route(const Json& settings) {
    const auto paired = load_client_pairing(settings);
    if (!paired) {
        throw EngineError("Pair a computer in Settings → Dictation → Remote engine first.");
    }
    if (!keys::meeting_speech_separate.read(settings) ||
        keys::meeting_asr_source.read(settings) != "remote" ||
        keys::fv_meeting_remote_fingerprint.read(settings) != paired->fingerprint) {
        throw EngineError("Choose this paired host in Meeting Voice before sending meeting audio.");
    }
    return {{"fingerprint", paired->fingerprint}, {"host_name", paired->host_name}};
}
std::optional<Json> saved_meeting_route(const Json& meeting, std::string_view own_fingerprint) {
    const auto raw = meeting.find("asr_remote_json");
    if (raw == meeting.end() || raw->is_null())
        return {};
    Json route =
        raw->is_string() ? Json::parse(raw->get_ref<const std::string&>(), nullptr, false) : Json();
    if (!route.is_object() || field(route, "fingerprint").empty() ||
        field(route, "family").empty() || field(route, "model").empty()) {
        throw EngineError("This meeting's remote speech configuration is invalid.");
    }
    if (!own_fingerprint.empty() &&
        py::lower(field(route, "fingerprint")) == py::lower(own_fingerprint))
        return {};
    return route;
}
Json checked_meeting_engine(const Json& route, const Json& ready) {
    const Json engine = ready.is_object() ? ready.value("engine", Json()) : Json();
    const std::string host =
        field(route, "host_name").empty() ? "the paired computer" : field(route, "host_name");
    if (!engine.is_object() || !engine.value("available", false)) {
        const auto status = field(engine, "status");
        throw EngineError(host + ": " + (status.empty() ? "speech engine is not ready." : status));
    }
    const auto family = field(engine, "family"), model = field(engine, "model");
    const auto* descriptor = models::model(model);
    if (family != "parakeet" || descriptor == nullptr || descriptor->backend != family ||
        !descriptor->meeting) {
        throw EngineError(
            host + "'s engine does not support meetings. Select Parakeet in Manage host models.");
    }
    if (!field(route, "model").empty() &&
        (model != field(route, "model") || family != field(route, "family"))) {
        throw EngineError("Restore " + field(route, "model") + " on " + host +
                          " to resume this meeting.");
    }
    Json resolved = route;
    resolved["family"] = family;
    resolved["model"] = model;
    return resolved;
}
} // namespace fv::remote
