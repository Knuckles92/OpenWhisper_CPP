#pragma once

#include <optional>
#include <string>

namespace fv::appcore {

// What the readiness check needs to know about the selected speech engine.
struct ReadinessInput {
    bool has_backend = false;      // an engine is selected
    bool available = false;        // it is loaded and can decode
    bool reload_in_flight = false; // EngineReloadMachine::in_flight()
    bool model_missing = false;    // its model is not downloaded
    bool closed_by_cancel = false; // a Cancel tore the loaded engine down (should_cancel)
    bool has_last_error = false;   // its last load left an error sentence
    std::string device_info;       // the engine's readout: the error sentence when not loaded
    // The selected engine's settings value: "parakeet" and "remote" are on-device speech
    // engines (Python's LocalSpeechBackend); "api" is a cloud speech endpoint. Last, so the
    // members M1 callers initialize keep their positions.
    std::string backend_id = "parakeet";
    // The Remote engine ("remote"): a background reconnect is running (RemoteRuntime.restoring),
    // and the paired host's name ("" reads "the remote engine"). has_last_error and device_info
    // are then the backend's last_error: set only when a connection attempt failed.
    bool remote_restoring = false;
    std::string remote_host;
};

// The readiness check's answer and the work it asks the caller to start.
struct ReadinessDecision {
    std::optional<std::string> message; // why capture must not start; nullopt when it may
    bool request_reload = false;        // call EngineReloadMachine::request()
    bool request_model_consent = false; // start the model download consent flow
    bool request_remote_retry = false;  // reconnect the Remote engine now (RemoteRuntime.retry_now)
};

// The actionable reason capture must not start yet (OpenWhisper's
// transcription_readiness_message with _speech_engine_readiness_message). A speech engine that
// is not loaded is reloaded or asks for its model, except the Remote engine, which is reconnected
// at once rather than reloaded (_remote_engine_readiness_message); any other unavailable engine
// is only named: the API one needs a valid key, anything else is unavailable.
ReadinessDecision transcription_readiness_message(const ReadinessInput& input);

} // namespace fv::appcore
