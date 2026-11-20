#pragma once

#include <filesystem>
#include <string>
#include <string_view>

#include "models/engine_control.h"

namespace fv {

class ActivityState;
class ApplicationController;
class SettingsStore;

namespace dictation {
class TranscriptionRuntime;
}
namespace speech {
class SpeechService;
}

// What the engine control reads. Every pointer is required and must outlive it.
struct EngineControlPorts {
    speech::SpeechService* speech = nullptr;
    ApplicationController* app = nullptr;
    dictation::TranscriptionRuntime* transcription = nullptr;
    ActivityState* activity = nullptr;
    SettingsStore* settings = nullptr;
    // Where the runtime components are installed (Paths::components_root): a loaded runtime
    // folder directly inside it is that component's.
    std::filesystem::path components_root;
};

// models::EngineControl over the controller (the ApplicationController attributes OpenWhisper's
// ModelRuntime and ComponentRuntime read): the job slot and the recorder for a speech job, the
// engine reload machine for a reload in flight, the activity state for a meeting, the saved
// engine choice, and the engine the speech service loaded for the dictation slot.
// Affinity: thread-safe, as EngineControl says; reload_engine posts to the UI executor when it
// is called elsewhere.
class ControllerEngineControl final : public models::EngineControl {
public:
    explicit ControllerEngineControl(EngineControlPorts ports);

    bool job_active() const override;
    bool meeting_active() const override;
    std::string selected_backend() const override;
    std::string loaded_model_key() const override;
    bool with_model_unused(std::string_view model_key, const std::function<void()>& work) override;
    std::filesystem::path loaded_runtime_dir() const override;
    void reload_engine(std::string_view note = {}) override;
    bool engine_settled() const override;
    void release_backends_using(std::string_view component_id) override;

private:
    EngineControlPorts ports_;
};

} // namespace fv
