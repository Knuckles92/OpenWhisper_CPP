#include "ui_controller/engine_control.h"

#include <optional>
#include <stdexcept>
#include <utility>

#include "appcore/activity.h"
#include "appcore/engine_reload.h"
#include "base/log.h"
#include "base/utf8.h"
#include "dictation/transcription_runtime.h"
#include "settings/store.h"
#include "speech/dictation_language.h"
#include "speech/service.h"
#include "ui_controller/application_controller.h"

namespace fv {
namespace {

constexpr log::Logger kLog{"services.application_controller"};

template <class T> T& required(T* pointer, const char* name) {
    if (pointer == nullptr) {
        throw std::invalid_argument(std::string("ControllerEngineControl needs ") + name);
    }
    return *pointer;
}

} // namespace

ControllerEngineControl::ControllerEngineControl(EngineControlPorts ports)
    : ports_(std::move(ports)) {
    required(ports_.speech, "the speech service");
    required(ports_.app, "the application controller");
    required(ports_.transcription, "the transcription runtime");
    required(ports_.activity, "the activity state");
    required(ports_.settings, "the settings");
}

// recorder.is_recording or is_transcribing() or _reload_in_flight.
bool ControllerEngineControl::job_active() const {
    return ports_.transcription->is_recording() || ports_.app->is_transcribing() ||
           ports_.app->reload_machine().in_flight();
}

bool ControllerEngineControl::meeting_active() const {
    return ports_.activity->snapshot().meeting_active;
}

std::string ControllerEngineControl::selected_backend() const {
    return speech::engine(*ports_.settings->snapshot());
}

// get_loaded_local_model: only a local engine has a model of its own on this computer.
std::string ControllerEngineControl::loaded_model_key() const {
    const std::optional<speech::BackendConfig> loaded = ports_.app->loaded_engine();
    if (!loaded || loaded->backend != "parakeet") {
        return {};
    }
    return loaded->model_key;
}

bool ControllerEngineControl::with_model_unused(std::string_view model_key,
                                                const std::function<void()>& work) {
    bool done = false;
    const bool unused = ports_.speech->with_model_unused(model_key, [&] {
        if (!meeting_active()) {
            work();
            done = true;
        }
    });
    return unused && done;
}

std::filesystem::path ControllerEngineControl::loaded_runtime_dir() const {
    return ports_.app->loaded_runtime_dir();
}

void ControllerEngineControl::reload_engine(std::string_view note) {
    ports_.app->reload_whisper_model(note);
}

bool ControllerEngineControl::engine_settled() const {
    return ports_.app->reload_machine().settled();
}

// The backends' cleanup for the engine on that runtime (components.py:75, 109): the dictation
// slot's engine when its runtime folder is that component's.
void ControllerEngineControl::release_backends_using(std::string_view component_id) {
    const std::optional<speech::BackendConfig> loaded = ports_.app->loaded_engine();
    if (!loaded || loaded->backend != "parakeet" || loaded->runtime_dir.empty()) {
        return;
    }
    const std::filesystem::path component =
        (ports_.components_root / base_utf8::path_from_utf8(component_id)).lexically_normal();
    if (loaded->runtime_dir.lexically_normal() != component) {
        return;
    }
    kLog.info("Closing the speech engine on runtime {}", component_id);
    ports_.speech->release_local(speech::Slot::Dictation);
}

} // namespace fv
