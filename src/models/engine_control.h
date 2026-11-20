#pragma once

#include <filesystem>
#include <functional>
#include <string>
#include <string_view>

// What the model and component runtimes ask of the dictation engine (OpenWhisper reads these off
// its ApplicationController: current_backend, _reload_in_flight, is_meeting_active,
// is_transcribing, recorder.is_recording, _engine_lock, reload_whisper_model, and the cleanup of
// the backends that use a runtime). The controller implements it over the speech service, the
// reload machine and the activity state.
namespace fv::models {

// Affinity: thread-safe, except reload_engine, which runs on the UI executor only. The queries
// and release_backends_using are also called on the downloads and components strands.
class EngineControl {
public:
    virtual ~EngineControl() = default;

    // A recording, a transcription or an engine reload is running.
    virtual bool job_active() const = 0;
    // Meeting Mode owns the engine (is_meeting_active).
    virtual bool meeting_active() const = 0;
    // The selected engine: "parakeet", "api" or "remote" (selected_model).
    virtual std::string selected_backend() const = 0;
    // The model the dictation engine has loaded ("parakeet-v3"); "" when none is loaded.
    virtual std::string loaded_model_key() const = 0;
    // Runs a cache mutation only when neither speech slot can be using this local model.
    virtual bool with_model_unused(std::string_view model_key,
                                   const std::function<void()>& work) = 0;
    // The runtime folder loaded into this process; empty while none is. A loaded runtime stays
    // loaded until exit, so another runtime can only be used after a restart.
    virtual std::filesystem::path loaded_runtime_dir() const = 0;
    // reload_whisper_model: the debounced reload of the selected engine. A non-empty `note`
    // replaces "Reloading speech engine..." as that reload's status (the controller's
    // _reload_note: "Switching to <label> for <device>...").
    virtual void reload_engine(std::string_view note = {}) = 0;
    // No reload running or queued (ApplicationController._engine_settled): a paired computer is
    // told "The host is loading its speech engine." until it is.
    virtual bool engine_settled() const { return true; }
    // Closes every engine that runs on `component_id` (the backends' cleanup), before that
    // runtime is installed or removed.
    virtual void release_backends_using(std::string_view component_id) = 0;
};

} // namespace fv::models
