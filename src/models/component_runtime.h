#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

#include "base/cancel.h"
#include "base/clock.h"
#include "base/executor.h"
#include "base/signal.h"
#include "models/coordinator.h"
#include "models/engine_control.h"

// Component installs and removals and their notifications (services/runtime/components.py).
// Installs and removals run on the components strand and report through the signals. A speech
// runtime is never replaced in the running process (architecture 5.7): a runtime installed while
// another is loaded, or a change to the loaded one, is used from the next start, and the finish
// message says to restart; an engine closed for a staged change to the loaded runtime reloads
// from the folder still in place. OpenWhisper's GPU Acceleration branch is not ported.
namespace fv::models {

// Affinity: UI actor on `ui`; the workers run on `components`. The signals are emitted on the
// components strand (progress, install finished, state changed) or on the UI executor.
// The owner shuts the components strand down (after ComponentCoordinator::cancel_all) before
// destroying this object.
class ComponentRuntime {
public:
    ComponentRuntime(EngineControl& engine, ComponentCoordinator& coordinator, Executor& ui,
                     Executor& components, const Clock& clock = system_clock());
    ~ComponentRuntime();
    ComponentRuntime(const ComponentRuntime&) = delete;
    ComponentRuntime& operator=(const ComponentRuntime&) = delete;

    // Starts an install unless the same component is already in flight. A speech runtime is
    // refused during a speech job ("Finish the current speech job before installing its
    // runtime"), and the engines that use it are closed first.
    void request_install(const std::string& component_id);
    void cancel_install(const std::string& component_id);
    // Removes a component on the components strand; a speech runtime is refused during a speech
    // job ("Finish the current speech job before removing its runtime").
    void request_uninstall(const std::string& component_id);

    // (component, phase, done bytes, total bytes); throttled at the source.
    Signal<std::string, std::string, std::int64_t, std::int64_t> component_progress;
    Signal<std::string> component_install_started;
    // (component, success, message): "Installed and ready to use — no restart needed.",
    // "Restart {product} to switch to the GPU runtime.", "Restart {product} to enable this
    // component.", "Installation canceled.", a ComponentError's text, or "Installation failed:
    // {error}".
    Signal<std::string, bool, std::string> component_install_finished;
    Signal<> component_state_changed;
    // The status line (the controller's status_update).
    Signal<std::string> status;

private:
    void install_worker(const std::string& component_id, const CancelToken& cancel);
    void uninstall_worker(const std::string& component_id);
    void on_install_finished(const std::string& component_id, bool success, bool reload);
    bool speech_job_running() const;

    EngineControl& engine_;
    ComponentCoordinator& coordinator_;
    Executor& ui_;
    Executor& components_;
    const Clock& clock_;
    std::shared_ptr<int> alive_ = std::make_shared<int>(0);
};

// Whether `component_id` is one of the speech runtimes (RUNTIME_IDS).
bool is_runtime_component(std::string_view component_id);
// One of the speech runtimes built for a GPU (CUDA, Vulkan, Metal).
bool is_gpu_runtime(std::string_view component_id);

// The finish message of an install that left the component on disk: "Installed and ready to
// use — no restart needed." when it can be used now, else the restart message (the GPU runtimes
// "Restart {product} to switch to the GPU runtime.", everything else "Restart {product} to
// enable this component.").
std::string install_finish_message(std::string_view component_id, bool activated);

} // namespace fv::models
