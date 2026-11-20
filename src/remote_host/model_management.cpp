#include "remote_host/model_management.h"

#include <algorithm>
#include <format>
#include <future>
#include <thread>
#include <utility>

#include "base/identity.h"
#include "models/coordinator.h"
#include "models/engine_control.h"
#include "models/engine_resolution.h"
#include "models/hf_access.h"
#include "models/hf_access_policy.h"
#include "models/model_details.h"
#include "models/runtime_resolver.h"
#include "models/speech_catalog.h"
#include "pyport/errors.h"
#include "remote/errors.h"
#include "remote/pairing_settings.h"
#include "remote_host/runtime_state.h"
#include "settings/keys.h"
#include "speech/service.h"

namespace fv::remote_host {
namespace {

using namespace std::chrono_literals;

constexpr std::string_view kFamily = "parakeet";

std::string field(const Json& object, std::string_view key) {
    const auto at = object.find(std::string(key));
    return at != object.end() && at->is_string() ? at->get<std::string>() : std::string();
}

const models::SpeechModel& check_model(std::string_view family, std::string_view model) {
    const auto* choice = models::model(model);
    if (family != kFamily || !choice || choice->backend != family) {
        throw py::ValueError("Choose a speech model from the host's catalog.");
    }
    return *choice;
}

Json model_fields(const models::SpeechModel& model) {
    return {{"family", model.backend}, {"model", model.key}, {"label", model.label}};
}

Json device_option(const models::ComponentCoordinator& components, std::string_view device) {
    const auto options = runtime_dependencies(components.store().host());
    const auto found = std::ranges::find_if(
        options, [&](const Json& value) { return field(value, "device") == device; });
    if (found == options.end()) {
        throw py::ValueError("Choose CPU or NVIDIA GPU from the host's runtime choices.");
    }
    return *found;
}

bool active(const Json& job, std::string_view state) {
    return field(job, "state") == state;
}

} // namespace

HostModelManager::HostModelManager(HostModelManagerDeps deps) : deps_(std::move(deps)) {}

void HostModelManager::require_allowed() const {
    if (deps_.shutdown.cancelled() || !remote::host_model_management(*deps_.settings.snapshot())) {
        throw remote::RemoteEngineError("Model management is disabled on the host.");
    }
}

models::ResolvedEngine HostModelManager::resolve(const Json& settings) const {
    return deps_.resolve
               ? deps_.resolve(settings)
               : models::resolve_parakeet(settings, deps_.paths, deps_.components.store().host());
}

std::string HostModelManager::name() const {
    return deps_.host_name.empty() ? std::string("The host") : deps_.host_name;
}

std::vector<remote::HostModel> HostModelManager::ready_models() {
    // Only models that load without asking anything of the person at this computer:
    // downloaded, with the runtime installed for the device set here.
    std::vector<remote::HostModel> result;
    const auto settings = deps_.settings.snapshot();
    if (!remote::host_model_management(*settings)) {
        return result;
    }
    for (const auto& model : models::models()) {
        Json candidate = *settings;
        auto selected = keys::local_asr_models.read(candidate);
        selected[std::string(kFamily)] = model.key;
        candidate[std::string(keys::local_asr_models.key())] = std::move(selected);
        if (resolve(candidate).usable()) {
            result.push_back(
                {std::string(model.backend), std::string(model.key), std::string(model.label)});
        }
    }
    return result;
}

Json HostModelManager::catalog() {
    const auto settings = deps_.settings.snapshot();
    const auto engine = deps_.describe();
    const auto& host = deps_.components.store().host();
    const auto devices = runtime_dependencies(host);
    const auto device = models::selected_device(*settings, kFamily);
    const auto runtime = resolve(*settings);
    Json choices = Json::array();
    for (const auto& model : models::models()) {
        auto entry = model_fields(model);
        entry["cached"] = deps_.cache.is_cached(model.key);
        entry["runtime_ready"] = runtime.runtime_dir.has_value();
        entry["selected_device"] = device;
        entry["download_size"] = models::get_model_details(model.key).download_size();
        entry["dependencies"] = devices;
        choices.push_back(std::move(entry));
    }
    std::lock_guard lock(jobs_mutex_);
    return {{"models", std::move(choices)},
            {"engine", engine},
            {"can_select", true},
            {"can_install_runtime", true},
            {"runtime", runtime_state(engine, *settings, host)},
            {"download", download_},
            {"installation", installation_}};
}

Json HostModelManager::download(std::string_view family, std::string_view key) {
    const auto& model = check_model(family, key);
    std::unique_lock lock(jobs_mutex_);
    if (active(download_, "downloading")) {
        if (field(download_, "model") == key) {
            return {{"download", download_}};
        }
        throw remote::RemoteEngineError(
            "Another remote model download is in progress. Wait for it to finish.");
    }

    if (!deps_.access.begin_request(key)) {
        throw remote::RemoteEngineError(
            "This model is already being downloaded or managed on the host.");
    }
    const std::string name(key);
    const auto owner = shared_from_this();
    const std::shared_ptr<void> claim(
        nullptr, [owner, name](void*) { owner->deps_.access.end_request(name); });
    const auto access = deps_.access.evaluate_access(key, true);
    if (access == models::AccessDecision::BlockedByEnv) {
        throw remote::RemoteEngineError(
            "Model downloads are disabled by HF_HUB_OFFLINE on the host.");
    }
    const bool cached = access == models::AccessDecision::LoadCached;
    if (!cached && deps_.access.policy() != models::hf_access_policy::kAlways) {
        throw remote::RemoteEngineError(
            "The host requires download consent. Choose ‘Always allow downloads’ in "
            "Settings → Downloads on the host, or download this model there first.");
    }
    download_ = model_fields(model);
    download_.update(Json{
        {"state", cached ? "complete" : "downloading"},
        {"done", 0},
        {"total", 0},
        {"error", ""},
    });
    Json response{{"download", download_}};
    lock.unlock();
    if (!cached) {
        // A task the executor drops (posted after shutdown) never runs: its destructor fails the
        // job, so it does not stay "downloading" for good (model_management.py's start failure:
        // "Couldn't start the download on the host.").
        struct Started {
            // Constructed in place: a temporary's destructor would fail the job at once.
            explicit Started(std::shared_ptr<HostModelManager> by) : owner(std::move(by)) {}
            Started(const Started&) = delete;
            Started& operator=(const Started&) = delete;
            std::shared_ptr<HostModelManager> owner;
            bool ran = false;
            ~Started() {
                if (!ran) {
                    std::lock_guard held(owner->jobs_mutex_);
                    if (owner->download_.value("state", "") == "downloading") {
                        owner->download_["state"] = "failed";
                        owner->download_["error"] = "Couldn't start the download on the host.";
                    }
                }
            }
        };
        auto started = std::make_shared<Started>(owner);
        if (deps_.shutdown.cancelled()) {
            started.reset(); // fails the job now
            throw remote::RemoteEngineError("Couldn't start the download on the host.");
        }
        deps_.work.post([owner, name, claim, started] {
            started->ran = true;
            (void)claim;
            std::string error;
            try {
                owner->deps_.cache.download(
                    name,
                    [&](std::int64_t done, std::int64_t total) {
                        std::lock_guard held(owner->jobs_mutex_);
                        owner->download_["done"] = done;
                        owner->download_["total"] = total;
                    },
                    owner->deps_.shutdown);
                if (!owner->deps_.cache.is_cached(name)) {
                    throw std::runtime_error("Incomplete model");
                }
                if (owner->deps_.inventory) {
                    owner->deps_.inventory->invalidate();
                }
            } catch (const std::exception&) {
                error = "Download failed on the host. Check its connection, free disk space and "
                        "logs, then retry.";
            }
            {
                std::lock_guard held(owner->jobs_mutex_);
                owner->download_["state"] = error.empty() ? "complete" : "failed";
                owner->download_["error"] = error;
            }
            owner->deps_.on_changed("models");
        });
    }
    return response;
}

Json HostModelManager::install(std::string_view family, std::string_view key,
                               std::string_view device) {
    const auto& model = check_model(family, key);
    auto option = device_option(deps_.components, device);
    const auto component = field(option, "component");
    std::unique_lock lock(jobs_mutex_);
    if (active(installation_, "installing")) {
        if (field(installation_, "component") == component) {
            return {{"installation", installation_}};
        }
        throw remote::RemoteEngineError(
            "Another remote runtime installation is in progress. Wait for it to finish.");
    }
    if (option["ready"] == true) {
        option["state"] = "complete";
        return {{"installation", std::move(option)}};
    }
    if (option["installable"] != true) {
        throw remote::RemoteEngineError(field(option, "reason"));
    }
    const auto cancel = deps_.components.begin_install(component, CancelSource(deps_.shutdown));
    if (!cancel) {
        throw remote::RemoteEngineError(
            "This runtime is already being installed on the host. Refresh when it finishes.");
    }
    const auto owner = shared_from_this();
    const std::shared_ptr<void> claim(
        nullptr, [owner, component](void*) { owner->deps_.components.end_install(component); });
    installation_ = model_fields(model);
    installation_.update(option);
    installation_.update(Json{
        {"state", "installing"}, {"phase", "Starting"}, {"done", 0}, {"total", 0}, {"error", ""}});
    Json response{{"installation", installation_}};
    lock.unlock();
    deps_.work.post([owner, component, cancel = cancel->token(), claim] {
        (void)claim;
        std::string state = "complete";
        std::string error;
        try {
            const auto outcome = owner->deps_.components.install(
                component,
                [&](std::string_view phase, std::int64_t done, std::int64_t total) {
                    std::lock_guard held(owner->jobs_mutex_);
                    owner->installation_["phase"] = phase;
                    owner->installation_["done"] = done;
                    owner->installation_["total"] = total;
                },
                cancel);
            const auto loaded = owner->deps_.engine_control.loaded_runtime_dir();
            const auto installed = owner->deps_.components.store().component_dir(component);
            if (outcome == models::ChangeOutcome::StagedForRestart ||
                (!loaded.empty() && loaded.lexically_normal() != installed.lexically_normal())) {
                state = "restart_required";
                error = std::format("Installed. Restart {} on the host to enable this runtime.",
                                    identity().product);
            }
        } catch (const models::ComponentCanceled&) {
            state = "failed";
            error = "Runtime installation was canceled on the host. You can retry.";
        } catch (const std::exception&) {
            state = "failed";
            error = "Runtime installation failed on the host. Check its connection, free disk "
                    "space and logs, then retry.";
        }
        {
            std::lock_guard held(owner->jobs_mutex_);
            owner->installation_["state"] = state;
            owner->installation_["error"] = error;
        }
        owner->deps_.on_changed("components");
    });
    return response;
}

Json HostModelManager::handle(std::string_view op, const Json& fields) {
    require_allowed();
    if (op == "model_catalog") {
        return catalog();
    }
    if (op == "download_model") {
        return download(field(fields, "family"), field(fields, "model"));
    }
    if (op == "install_runtime") {
        return install(field(fields, "family"), field(fields, "model"), field(fields, "device"));
    }
    throw py::ValueError("Unknown model management operation.");
}

Json HostModelManager::apply(const Json& patch, std::string_view model, bool runtime,
                             std::string_view device_name) {
    using Generation = std::uint64_t;
    const auto* listed = models::model(model);
    const std::string label = listed ? std::string(listed->label) : std::string(model);
    auto accepted = std::make_shared<std::promise<Generation>>();
    auto received = accepted->get_future();
    // _switch_engine_to's _reload_note, shown on this computer while the reload runs.
    std::string note = device_name.empty()
                           ? std::format("Switching to {}...", label)
                           : std::format("Switching to {} for {}...", label, device_name);
    deps_.ui.post([owner = weak_from_this(), accepted, patch, note = std::move(note)] {
        try {
            const auto self = owner.lock();
            if (!self) {
                throw remote::RemoteEngineError("The host is closing.");
            }
            self->require_allowed();
            auto& deps = self->deps_;
            // _engine_switch_refusal and _apply_engine_overrides: why it can't happen now.
            const auto host = self->name();
            if (deps.engine_control.meeting_active()) {
                throw remote::RemoteEngineError(std::format(
                    "{} is running a meeting. Change its model after the meeting ends.", host));
            }
            if (deps.speech.readiness(speech::Slot::Dictation).state ==
                speech::ReadyState::Loading) {
                throw remote::RemoteEngineError(
                    std::format("{} is loading its engine. Try again in a moment.", host));
            }
            if (deps.engine_control.job_active()) {
                throw remote::RemoteEngineError(
                    std::format("{} is transcribing right now. Try again in a moment.", host));
            }
            const auto generation = deps.speech.generation(speech::Slot::Dictation);
            deps.settings.update(patch);
            deps.engine_control.reload_engine(note);
            accepted->set_value(generation);
        } catch (...) {
            accepted->set_exception(std::current_exception());
        }
    });
    const auto deadline = std::chrono::steady_clock::now() + 240s;
    // The UI thread takes the switch, or says why not, within RemoteRuntime's 10 s; only then is
    // the load waited for.
    const auto handoff = std::chrono::steady_clock::now() + handoff_timeout_;
    while (received.wait_for(50ms) != std::future_status::ready) {
        if (deps_.shutdown.cancelled()) {
            throw remote::RemoteEngineError("The host is closing or still loading its engine.");
        }
        if (std::chrono::steady_clock::now() >= handoff) {
            throw remote::RemoteEngineError(
                std::format("{} didn't respond. Try again in a moment.", name()));
        }
    }
    // Unsettled from the moment the reload is queued until the load it starts has ended: a new
    // generation that is past Loading.
    const auto before = received.get();
    while (std::chrono::steady_clock::now() < deadline) {
        const auto generation = deps_.speech.generation(speech::Slot::Dictation);
        const auto state = deps_.speech.readiness(speech::Slot::Dictation);
        if (generation > before && state.state != speech::ReadyState::Loading &&
            state.state != speech::ReadyState::Warming && !deps_.engine_control.job_active()) {
            const auto engine = deps_.describe();
            if (state.state != speech::ReadyState::Ready) {
                const auto status = field(engine, "status");
                if (!status.empty() &&
                    !(engine.contains("available") && engine["available"] == true)) {
                    throw remote::RemoteEngineError(status);
                }
                throw remote::RemoteEngineError(
                    runtime ? std::string("The host couldn't load this runtime.")
                            : std::format("{} couldn't load {}.", name(), label));
            }
            if (field(engine, "model") != model || field(engine, "family") != kFamily) {
                throw remote::RemoteEngineError(
                    runtime ? std::string("The host switched models while changing its runtime.")
                            : std::format("{} switched to another model while loading {}.", name(),
                                          label));
            }
            return engine;
        }
        if (deps_.shutdown.wait_for(50ms)) {
            throw remote::RemoteEngineError("The host is closing.");
        }
    }
    throw remote::RemoteEngineError(
        runtime ? std::string("The host is still loading its runtime. Reconnect in a moment.")
                : std::format("{} is still loading {}. Try again in a moment.", name(), label));
}

Json HostModelManager::select(std::string_view family, std::string_view key,
                              const std::optional<std::string>& device,
                              std::string_view device_name) {
    require_allowed();
    std::unique_lock changing(change_mutex_, std::try_to_lock);
    if (!changing) {
        throw remote::RemoteEngineError("The host is changing its engine. Try again in a moment.");
    }
    if (!device) {
        // What paired computers may switch to is what ready_models() lists.
        const auto ready = ready_models();
        if (std::ranges::none_of(ready, [&](const remote::HostModel& entry) {
                return entry.family == family && entry.model == key;
            })) {
            throw remote::RemoteEngineError(
                std::format("{} isn't ready on {}. Download it there first.", key, name()));
        }
    }
    const auto* listed = models::model(key);
    if (family != kFamily || !listed || listed->backend != family) {
        throw remote::RemoteEngineError(std::format("{} can't run {}.", name(), key));
    }
    const auto& model = *listed;
    const auto settings = deps_.settings.snapshot();
    const auto current = deps_.describe();
    if (!device && field(current, "family") == family && field(current, "model") == key &&
        current.contains("available") && current["available"] == true) {
        return current;
    }
    if (!deps_.cache.is_cached(key)) {
        throw remote::RemoteEngineError("Download the model on the host first.");
    }
    auto saved_models = keys::local_asr_models.read(*settings);
    auto saved_devices = keys::local_asr_devices.read(*settings);
    saved_models[std::string(family)] = model.key;
    if (device) {
        const auto option = device_option(deps_.components, *device);
        if (option["ready"] != true) {
            // dependencies.py validate_model_device: the device's own reason first.
            const auto reason = field(option, "reason");
            throw remote::RemoteEngineError(
                reason.empty()
                    ? std::string("Install the selected device's runtime on the host first.")
                    : reason);
        }
        saved_devices[std::string(family)] = *device;
    }
    Json patch{{std::string(keys::selected_model.key()), family},
               {std::string(keys::local_asr_models.key()), std::move(saved_models)},
               {std::string(keys::local_asr_devices.key()), std::move(saved_devices)}};
    Json proposed = *settings;
    proposed.update(patch);
    if (!resolve(proposed).usable()) {
        throw remote::RemoteEngineError("The selected model or runtime isn't ready on the host.");
    }
    return apply(patch, key, false, device_name);
}

Json HostModelManager::configure(std::string_view family, std::string_view model,
                                 const Json& changes, std::string_view device_name) {
    require_allowed();
    std::unique_lock changing(change_mutex_, std::try_to_lock);
    if (!changing) {
        throw remote::RemoteEngineError("The host is changing its runtime. Try again in a moment.");
    }
    const auto patch = validate_runtime(deps_.describe(), *deps_.settings.snapshot(),
                                        deps_.components.store().host(), family, model, changes);
    return apply(patch, model, true, device_name);
}

} // namespace fv::remote_host
