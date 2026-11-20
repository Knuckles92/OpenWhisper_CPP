#include "models/component_runtime.h"

#include <algorithm>
#include <exception>
#include <format>
#include <stdexcept>
#include <utility>

#include "base/identity.h"
#include "base/log.h"
#include "models/catalog.h"
#include "models/progress_throttle.h"
#include "models/runtime_resolver.h"

namespace fv::models {
namespace {

constexpr log::Logger kLog("services.runtime.components");

constexpr std::string_view kReady = "Installed and ready to use — no restart needed.";
constexpr std::string_view kCanceled = "Installation canceled.";
constexpr std::string_view kInstallDuringJob =
    "Finish the current speech job before installing its runtime";
constexpr std::string_view kRemoveDuringJob =
    "Finish the current speech job before removing its runtime";

} // namespace

bool is_gpu_runtime(std::string_view component_id) {
    return component_id == component_id::kAsrNvidiaCuda ||
           component_id == component_id::kAsrNvidiaVulkan ||
           component_id == component_id::kAsrNvidiaMetal;
}

bool is_runtime_component(std::string_view component_id) {
    const auto ids = runtime_ids();
    return std::find(ids.begin(), ids.end(), component_id) != ids.end();
}

std::string install_finish_message(std::string_view component_id, bool activated) {
    if (activated) {
        return std::string(kReady);
    }
    if (is_gpu_runtime(component_id)) {
        return std::format("Restart {} to switch to the GPU runtime.", identity().product);
    }
    return std::format("Restart {} to enable this component.", identity().product);
}

ComponentRuntime::ComponentRuntime(EngineControl& engine, ComponentCoordinator& coordinator,
                                   Executor& ui, Executor& components, const Clock& clock)
    : engine_(engine), coordinator_(coordinator), ui_(ui), components_(components), clock_(clock) {}

ComponentRuntime::~ComponentRuntime() {
    alive_.reset();
}

// _speech_job_running: a meeting, a recording, a transcription or a reload in flight.
bool ComponentRuntime::speech_job_running() const {
    return engine_.meeting_active() || engine_.job_active();
}

void ComponentRuntime::request_install(const std::string& component_id) {
    if (is_runtime_component(component_id)) {
        if (speech_job_running()) {
            status.emit(std::string(kInstallDuringJob));
            return;
        }
        engine_.release_backends_using(component_id);
    }
    auto cancel = coordinator_.begin_install(component_id);
    if (!cancel) {
        return;
    }
    component_install_started.emit(component_id);
    std::weak_ptr<int> alive = alive_;
    components_.post([this, alive, component_id, token = cancel->token()] {
        if (alive.lock()) {
            install_worker(component_id, token);
        }
    });
}

void ComponentRuntime::cancel_install(const std::string& component_id) {
    coordinator_.cancel_install(component_id);
}

void ComponentRuntime::request_uninstall(const std::string& component_id) {
    if (is_runtime_component(component_id) && speech_job_running()) {
        status.emit(std::string(kRemoveDuringJob));
        return;
    }
    // OpenWhisper removes a component that is not a runtime on the calling thread; every
    // component here is a runtime, and removal blocks on the disk, so both go to the strand.
    std::weak_ptr<int> alive = alive_;
    components_.post([this, alive, component_id] {
        if (alive.lock()) {
            uninstall_worker(component_id);
        }
    });
}

void ComponentRuntime::uninstall_worker(const std::string& component_id) {
    try {
        if (is_runtime_component(component_id)) {
            if (speech_job_running()) {
                throw std::runtime_error(std::string(kRemoveDuringJob));
            }
            engine_.release_backends_using(component_id);
        }
        const ChangeOutcome outcome = coordinator_.uninstall(component_id);
        if (outcome == ChangeOutcome::StagedForRestart) {
            // The runtime is loaded in this process and cannot be unloaded: its folder goes at
            // the next start.
            status.emit(
                std::format("Restart {} to finish removing the component.", identity().product));
        } else {
            status.emit("Component removed");
        }
    } catch (const std::exception& error) {
        kLog.error_with(error, std::format("Failed to remove component '{}'", component_id));
        status.emit(std::format("Could not remove the component: {}", error.what()));
    }
    component_state_changed.emit();
}

void ComponentRuntime::install_worker(const std::string& component_id, const CancelToken& cancel) {
    ProgressThrottle throttle(clock_);
    const InstallProgress progress = [this, &throttle, &component_id](std::string_view phase,
                                                                      std::int64_t done,
                                                                      std::int64_t total) {
        if (throttle.should_emit(done, total)) {
            component_progress.emit(component_id, std::string(phase), done, total);
        }
    };

    bool success = false;
    bool activated = false;
    bool staged = false;
    std::string message;
    try {
        // Without a catalog entry this throws "Could not reach the download server. ...".
        const ChangeOutcome outcome = coordinator_.install(component_id, progress, cancel);
        success = true;
        staged = outcome == ChangeOutcome::StagedForRestart;
        // One runtime per process: a runtime installed while another is loaded, or a change
        // staged for the loaded one, is used from the next start.
        activated = outcome == ChangeOutcome::Done &&
                    !(is_runtime_component(component_id) && !engine_.loaded_runtime_dir().empty());
        if (!activated) {
            kLog.warning("Component '{}' installed but could not be activated in this session: {}",
                         component_id,
                         outcome == ChangeOutcome::StagedForRestart
                             ? "it is loaded, so the change is staged for the next start"
                             : "another speech runtime is loaded");
        }
        message = install_finish_message(component_id, activated);
    } catch (const ComponentCanceled&) {
        message = std::string(kCanceled);
        kLog.info("Install of '{}' was canceled", component_id);
    } catch (const ComponentError& error) {
        message = error.what();
        kLog.error("Install of '{}' failed: {}", component_id, error.what());
    } catch (const std::exception& error) {
        message = std::format("Installation failed: {}", error.what());
        kLog.error_with(error, std::format("Install of '{}' failed", component_id));
    }
    coordinator_.end_install(component_id);
    component_install_finished.emit(component_id, success, message);
    std::weak_ptr<int> alive = alive_;
    // A change staged for the loaded runtime left its folder in place, but request_install closed
    // the engines on it: they load again from that folder until the next start.
    const bool reload = activated || staged;
    ui_.post([this, alive, component_id, success, reload] {
        if (alive.lock()) {
            on_install_finished(component_id, success, reload);
        }
    });
    component_state_changed.emit();
}

// _on_install_finished: a runtime installed for the selected local engine is loaded at once
// when it could be activated, and an engine closed for a staged update of the loaded runtime is
// loaded again. A runtime installed beside another loaded one waits for the next start. A remote
// engine runs on its host, so nothing installed here changes it.
void ComponentRuntime::on_install_finished(const std::string& component_id, bool success,
                                           bool reload) {
    if (!success || !reload || !is_runtime_component(component_id)) {
        return;
    }
    if (engine_.selected_backend() == "parakeet") {
        engine_.reload_engine();
    }
}

} // namespace fv::models
