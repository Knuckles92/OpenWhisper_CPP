#include "meeting/rerun_options.h"

#include "meeting/detail/py_value.h"
#include "models/meeting_speech.h"
#include "settings/keys.h"

namespace fv::meeting {

SpeakerPassGate resolve_speaker_pass(const Json& settings, const Clock& clock) {
    // A stored "openai" backend reads as "local", so the gate refuses before any key would be
    // needed; no key lookup is passed for the same reason.
    return speaker_pass_gate(
        models::resolve_meeting_speaker_id_backend(settings, clock),
        keys::meeting_audio_upload_consent_given.read(settings), [] { return false; }, clock);
}

RerunOptions rerun_options(const Json& meeting, const Json& settings, const Clock& clock,
                           const SavedRemoteRoute& remote) {
    static const Json empty_meeting = Json::object();
    const Json& row = detail::truthy(meeting) ? meeting : empty_meeting;
    const std::optional<Json> route = remote ? remote(row) : std::nullopt;

    RerunOptions options;
    const Json& asr_model = detail::get(row, "asr_model");
    options.asr_model_name = detail::truthy(asr_model)
                                 ? detail::py_str(asr_model)
                                 : models::resolve_meeting_whisper_model(settings);
    if (route) {
        const Json& language = detail::get(*route, "language");
        options.language = route->is_object() && route->contains("language")
                               ? detail::py_str(language)
                               : std::string("auto");
    } else {
        options.language = keys::meeting_language.read(settings);
    }
    options.speaker_id_backend = models::resolve_meeting_speaker_id_backend(settings, clock);
    options.speaker_audio_consent = keys::meeting_audio_upload_consent_given.read(settings);
    options.redecode_coverage_guard = keys::meeting_redecode_coverage_guard.read(settings);
    return options;
}

} // namespace fv::meeting
