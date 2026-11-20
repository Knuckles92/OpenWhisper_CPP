#pragma once

#include <chrono>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "base/cancel.h"
#include "base/json.h"
#include "models/engine_resolution.h"
#include "remote/link.h"

namespace fv {
class Paths;
class SettingsStore;
namespace models {
class CachedModelsSnapshot;
class ComponentCoordinator;
class EngineControl;
class HuggingFaceAccessCoordinator;
class SpeechModelCache;
} // namespace models
namespace speech {
class SpeechService;
}
} // namespace fv

namespace fv::remote_host {

struct HostModelManagerDeps {
    const Paths& paths;
    SettingsStore& settings;
    models::SpeechModelCache& cache;
    models::ComponentCoordinator& components;
    models::HuggingFaceAccessCoordinator& access;
    models::EngineControl& engine_control;
    speech::SpeechService& speech;
    Executor& work;
    Executor& ui;
    CancelToken shutdown;
    std::function<Json()> describe;
    std::function<void(std::string)> on_changed;
    models::CachedModelsSnapshot* inventory = nullptr;
    // This computer's name in what a paired computer is told (socket.gethostname()); empty
    // reads "The host".
    std::string host_name;
    // Which runtime and model file a settings snapshot would load. Empty:
    // models::resolve_parakeet over `paths` and the component store's host.
    std::function<models::ResolvedEngine(const Json& settings)> resolve;
};

// An opt-in service for the host's bounded model and runtime catalog. A download's job
// belongs to the host, not to the client connection that started it.
//
// select() and configure() are OpenWhisper's RemoteEngineService.select_model and
// configure_runtime with RemoteRuntime._switch_engine_to: the model is saved and loaded as
// though it were picked on this computer, and the paired computer is answered once the load
// has settled, or told why not in a sentence it shows as it is.
class HostModelManager final : public std::enable_shared_from_this<HostModelManager> {
public:
    explicit HostModelManager(HostModelManagerDeps deps);

    std::vector<remote::HostModel> ready_models();
    Json handle(std::string_view op, const Json& fields);
    // `device_name`: the paired computer that asked, named in the host's reload status
    // ("Switching to <label> for <device>..."); empty for this computer ("Switching to
    // <label>...").
    Json select(std::string_view family, std::string_view model,
                const std::optional<std::string>& device, std::string_view device_name = {});
    Json configure(std::string_view family, std::string_view model, const Json& changes,
                   std::string_view device_name = {});
    // How long the UI thread has to take a switch before the paired computer is told "<host>
    // didn't respond. Try again in a moment." (RemoteRuntime.switch_engine_for_client: 10 s).
    void set_handoff_timeout(std::chrono::milliseconds timeout) { handoff_timeout_ = timeout; }

private:
    Json catalog();
    Json download(std::string_view family, std::string_view model);
    Json install(std::string_view family, std::string_view model, std::string_view device);
    // Saves `patch` and reloads on the UI executor, then waits for the load to settle; the new
    // engine's describe(). `runtime`: a runtime change of the model already served.
    Json apply(const Json& patch, std::string_view model, bool runtime,
               std::string_view device_name);
    void require_allowed() const;
    models::ResolvedEngine resolve(const Json& settings) const;
    std::string name() const;

    HostModelManagerDeps deps_;
    std::mutex jobs_mutex_;
    std::mutex change_mutex_;
    Json download_ = Json::object();
    Json installation_ = Json::object();
    std::chrono::milliseconds handoff_timeout_{10000};
};

} // namespace fv::remote_host
