#include "appcore/readiness.h"

#include <string_view>

namespace fv::appcore {

namespace {

// The engines OpenWhisper builds on LocalSpeechBackend.
bool is_speech_engine(std::string_view backend_id) {
    return backend_id == "parakeet" || backend_id == "remote";
}

} // namespace

ReadinessDecision transcription_readiness_message(const ReadinessInput& input) {
    ReadinessDecision decision;
    if (!input.has_backend) {
        decision.message = "No transcription engine is selected";
        return decision;
    }
    if (input.available) {
        return decision;
    }
    if (!is_speech_engine(input.backend_id)) {
        decision.message = input.backend_id == "api"
                               ? "The selected API transcription engine needs a valid API key"
                               : "The selected transcription engine is unavailable";
        return decision;
    }
    if (input.reload_in_flight) {
        decision.message = "Speech engine is still loading...";
        return decision;
    }
    if (input.backend_id == "remote") {
        // The host may be back (woke up, came into range): try now rather than at the next
        // scheduled retry.
        decision.request_remote_retry = true;
        const std::string host =
            input.remote_host.empty() ? std::string("the remote engine") : input.remote_host;
        if (input.remote_restoring || !input.has_last_error) {
            decision.message = "Connecting to " + host + "...";
        } else {
            decision.message = input.device_info + " Trying again...";
        }
        return decision;
    }
    if (input.model_missing) {
        // The consent flow runs, and the engine's own sentence says what is missing.
        decision.request_model_consent = true;
    } else if (input.closed_by_cancel || !input.has_last_error) {
        // Closed by a Cancel, or never loaded: loading it again is the fix.
        decision.request_reload = true;
        decision.message = "Reloading speech engine...";
        return decision;
    }
    decision.message = input.device_info;
    return decision;
}

} // namespace fv::appcore
