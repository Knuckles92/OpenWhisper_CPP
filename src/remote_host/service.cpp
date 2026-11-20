#include "remote_host/service.h"
#include "remote/meeting_route.h"
#include "remote_host/mcp_session.h"

#include <algorithm>
#include <chrono>
#include <format>
#include <tuple>
#include <utility>

#include "base/executor.h"
#include "base/log.h"
#include "models/coordinator.h"
#include "models/engine_control.h"
#include "net/lan_address.h"
#include "pyport/errors.h"
#include "remote/connection.h"
#include "remote/remote_backend.h"
#include "remote/scans.h"
#include "remote_host/dispatch.h"
#include "remote_host/host_settings.h"
#include "remote_host/runtime_state.h"
#include "settings/keys.h"

namespace fv::remote_host {

namespace {
constexpr log::Logger kLog{"services.remote_asr.service"};
} // namespace
namespace {

using namespace std::chrono_literals;

const Json& capabilities(const Json& ready) {
    static const Json empty = Json::object();
    const auto found = ready.find("capabilities");
    return found != ready.end() && found->is_object() ? *found : empty;
}

} // namespace

RemoteEngineService::RemoteEngineService(RemoteEngineServiceDeps deps)
    : deps_(std::move(deps)), dotenv_(deps_.paths.env_file(), deps_.env),
      registry_(settings_device_registry(deps_.settings, deps_.clock)), history_(*registry_) {
    if (!deps_.tailscale) {
        deps_.tailscale = remote::make_tailscale_cli(deps_.env);
    }
    if (deps_.components && deps_.model_cache && deps_.model_access && deps_.engine_control &&
        deps_.speech_service && deps_.ui) {
        models_ = std::make_shared<HostModelManager>(HostModelManagerDeps{
            .paths = deps_.paths,
            .settings = deps_.settings,
            .cache = *deps_.model_cache,
            .components = *deps_.components,
            .access = *deps_.model_access,
            .engine_control = *deps_.engine_control,
            .speech = *deps_.speech_service,
            .work = deps_.io,
            .ui = *deps_.ui,
            .shutdown = shutdown_.token(),
            .describe = deps_.engine ? [engine = deps_.engine] {
                if (auto current = engine()) {
                    return current->describe();
                }
                return Json();
            } : std::function<Json()>([] { return Json(); }),
            .on_changed = [guard = background_work_, this](std::string event) {
                guard->run([this, &event] { changed.emit(event); });
            },
            .inventory = deps_.inventory,
            .host_name = deps_.computer_name,
        });
    }
    deps_.client_env.remember_addresses = [this](std::string_view fingerprint,
                                                 std::string_view reached, int port,
                                                 std::span<const std::string> listed) {
        if (remote::remember_host_addresses(deps_.settings, fingerprint, reached, port, listed)) {
            changed.emit("client");
        }
    };
}

std::unique_ptr<speech::Backend> RemoteEngineService::make_backend() {
    const auto service = shared_from_this();
    auto backend = std::make_unique<remote::RemoteSpeechBackend>(
        [service] { return service->client_pairing(); },
        [service](const remote::ClientPairing& pairing) {
            // reports_recording stays false: OpenWhisper's client reserves each recording on the
            // host (recording_begin / recording_end) only so that a host's update hold
            // (prepare_host_update, called by the Linux Omarchy controls alone) waits for it.
            // OpenWhisper_CPP does not send reservations, so it does not advertise them: such a
            // host counts this computer as untracked and holds its update while it is connected.
            return service->make_connection(
                pairing, {}, false,
                std::format("This computer's pairing with {} is missing its token. Pair again "
                            "in Settings → Remote engine.",
                            pairing.host_name),
                "The paired host changed.");
        },
        deps_.clock, nullptr, true, &deps_.io);
    {
        std::lock_guard lock(dictation_mutex_);
        dictation_link_ = backend->link_handle();
        dictation_link_watch_ =
            backend->link_changed.connect(deps_.io, [this] { dictation_link_changed.emit(); });
    }
    dictation_link_changed.emit();
    // _on_host_renamed: dictation calls the paired host its new name, without reconnecting.
    backend->keep(changed.connect(
        deps_.io, [weak = weak_from_this(), target = backend.get()](const std::string& event) {
            if (event != "host_renamed") {
                return;
            }
            if (const auto self = weak.lock()) {
                if (const auto pairing = self->client_pairing()) {
                    target->host_renamed(*pairing);
                }
            }
        }));
    // A model chosen for the host rides on the next load only.
    std::optional<remote::HostModel> requested;
    std::optional<Json> runtime;
    {
        std::lock_guard lock(requested_mutex_);
        requested = std::exchange(requested_model_, std::nullopt);
        runtime = std::exchange(requested_runtime_, std::nullopt);
    }
    if (requested) {
        backend->request_model(requested->family, requested->model, std::move(runtime));
    }
    return backend;
}

remote::RemoteSpeechBackend::LinkHandle RemoteEngineService::dictation_link() const {
    std::lock_guard lock(dictation_mutex_);
    return dictation_link_;
}

remote::HostModel RemoteEngineService::request_model(std::string_view family,
                                                     std::string_view model,
                                                     std::optional<Json> runtime) {
    remote::HostModel choice{std::string(family), std::string(model), std::string(model)};
    std::lock_guard lock(requested_mutex_);
    requested_model_ = choice;
    requested_runtime_ = std::move(runtime);
    return choice;
}

std::unique_ptr<speech::Backend>
RemoteEngineService::make_meeting_backend(std::shared_ptr<Json> route) {
    const auto service = shared_from_this();
    const std::string fingerprint = route->at("fingerprint").get<std::string>();
    std::string host = route->value("host_name", std::string());
    if (host.empty()) {
        host = "the paired computer";
    }
    return std::make_unique<remote::RemoteSpeechBackend>(
        [service, fingerprint, host]() -> std::optional<remote::ClientPairing> {
            const auto settings = service->deps_.settings.snapshot();
            const auto paired = remote::load_client_pairing(*settings);
            if (!paired || paired->fingerprint != fingerprint) {
                // meeting/asr/remote.py _check_pairing: a forgotten or replaced pairing.
                throw remote::RemoteDecodeNotStarted(
                    std::format("Pair with {} again to transcribe this meeting. Its audio will "
                                "not be sent to a different computer.",
                                host));
            }
            if (!keys::meeting_speech_separate.read(*settings) ||
                keys::meeting_asr_source.read(*settings) != "remote" ||
                keys::fv_meeting_remote_fingerprint.read(*settings) != fingerprint) {
                throw remote::RemoteDecodeNotStarted(
                    "The original host is no longer selected for meeting speech.");
            }
            return paired;
        },
        [service](const remote::ClientPairing& pairing) {
            // OpenWhisper meeting/asr/remote.py reports_recording=False.
            return service->client_connection(pairing, {}, false);
        },
        deps_.clock, std::move(route));
}

RemoteEngineService::~RemoteEngineService() {
    shutdown();
}

std::shared_ptr<SpeechHost> RemoteEngineService::host() const {
    std::lock_guard lock(host_mutex_);
    return host_;
}

void RemoteEngineService::apply_host_settings_locked() {
    if (stopping_) {
        return;
    }
    const auto settings = deps_.settings.snapshot();
    auto server = host();
    if (!remote::host_enabled(*settings)) {
        if (server) {
            server->stop();
        }
        std::lock_guard lock(host_mutex_);
        host_error_.clear();
        return;
    }
    const int port = deps_.listen_port.value_or(remote::host_port(*settings));
    if (server && server->running() && (port == 0 || server->port() == port)) {
        return;
    }
    try {
        if (!server) {
            HostIdentity identity =
                ensure_host_identity(deps_.paths.data_root(), deps_.computer_name);
            const auto fingerprint = identity.fingerprint;
            SpeechHostDeps options{
                .reactor = deps_.client_env.reactor,
                .registry = *registry_,
                .identity = std::move(identity),
                .host_name = deps_.computer_name,
                .clock = deps_.clock,
                .tailscale = deps_.tailscale.get(),
                .history = &history_,
                .discovery = deps_.client_env.discovery,
                .engine_provider = deps_.engine,
                .tailscale_owner = [this] { return tailscale_owner(); },
                .addresses = [this] { return host_addresses(); },
            };
            if (deps_.engine_control) {
                // Paired computers hear "The host is loading its speech engine." while a reload
                // runs or is queued here (ApplicationController._engine_settled).
                options.engine_settled = [control = deps_.engine_control] {
                    return control->engine_settled();
                };
            }
            if (deps_.components) {
                options.runtime = [this] {
                    return runtime_state(engine_state(), *deps_.settings.snapshot(),
                                         deps_.components->store().host());
                };
            }
            if (models_) {
                options.model_management = [this] {
                    return remote::host_model_management(*deps_.settings.snapshot());
                };
                options.models = [manager = models_] {
                    Json result = Json::array();
                    for (const auto& model : manager->ready_models()) {
                        result.push_back({{"family", model.family},
                                          {"model", model.model},
                                          {"label", model.label}});
                    }
                    return result;
                };
                options.manage_models = [manager = models_](std::string_view op, const Json& fields,
                                                            std::string_view) {
                    return manager->handle(op, fields);
                };
                options.select_model = [manager = models_](std::string family, std::string model,
                                                           std::string device_name,
                                                           std::optional<std::string> device) {
                    return manager->select(family, model, device, device_name);
                };
                options.configure_runtime = [manager = models_](std::string family,
                                                                std::string model, Json changes,
                                                                std::string device_name) {
                    return manager->configure(family, model, changes, device_name);
                };
            }
            if (deps_.mcp_session) {
                options.mcp_peer_allowed = [session = deps_.mcp_session](std::string_view device) {
                    return session->allowed(device);
                };
                options.mcp_peer_request =
                    [session = deps_.mcp_session](std::string_view op, const Json& header,
                                                  std::string_view device,
                                                  std::string_view connection) {
                        return session->request(op, header, device, connection);
                    };
                options.mcp_peer_disconnect = [session =
                                                   deps_.mcp_session](std::string_view connection) {
                    session->disconnect(connection);
                };
            }
            if (deps_.records) {
                options.records_enabled = [this] {
                    return remote::host_keeps_records(*deps_.settings.snapshot());
                };
                options.records = [this,
                                   store = deps_.records](std::string_view op, const Json& fields,
                                                          std::span<const std::byte> bytes,
                                                          const remote::PairedDevice& device) {
                    // service.py records_request: the owners a device may read are the
                    // registry's at this request, so a recovery grant reaches a connection
                    // already open, and nothing the connection carries widens it.
                    remote::PairedDevice grant = device;
                    grant.record_owner_ids.clear();
                    for (const auto& paired : registry_->list()) {
                        if (paired.id == device.id) {
                            grant.record_owner_ids = paired.record_owner_ids;
                        }
                    }
                    try {
                        return store->handle(op, fields, bytes, grant);
                    } catch (const remote_records::RecordsBusy& problem) {
                        throw RecordsBusy(problem.what());
                    } catch (const remote_records::RecordNotFound& problem) {
                        throw RecordNotFound(problem.what());
                    }
                };
                options.records_summary = [this](std::string_view device) {
                    return records_summary(device);
                };
            }
            server = std::make_shared<SpeechHost>(std::move(options));
            host_events_ = server->events.connect(
                deps_.io, [this](std::string event, Json) { changed.emit(event); });
            std::lock_guard lock(host_mutex_);
            host_ = server;
            fingerprint_ = fingerprint;
        } else {
            server->stop();
        }
        const auto started = server->start(port, deps_.bind, deps_.discovery_port);
        if (started) {
            // Read fresh: the network may have changed since sharing was last on.
            (void)network_profiles(true);
        }
        std::lock_guard lock(host_mutex_);
        host_error_ = started ? std::string()
                              : std::format("Couldn't listen on port {} ({}). Another program "
                                            "may be using it; choose a different port.",
                                            port, started.error());
    } catch (const std::exception& error) {
        std::lock_guard lock(host_mutex_);
        host_error_ = std::format("Couldn't start sharing: {}", error.what());
    }
}

void RemoteEngineService::apply_host_settings() {
    assert_not_gui_thread();
    {
        std::lock_guard lock(lifecycle_);
        apply_host_settings_locked();
    }
    changed.emit("state");
}

void RemoteEngineService::shutdown() {
    if (stopping_.exchange(true)) {
        return;
    }
    shutdown_.cancel();
    close_mcp_control();
    if (deps_.mcp_session) {
        deps_.mcp_session->revoke();
    }
    speech_events_.disconnect();
    background_work_->close();
    {
        std::lock_guard lock(pairing_mutex_);
        pending_.reset();
    }
    Subscription events;
    {
        std::lock_guard lock(lifecycle_);
        events = std::move(host_events_);
        if (auto server = host()) {
            server->stop();
        }
    }
    events.disconnect();
}

void RemoteEngineService::watch_engine(speech::SpeechService& speech) {
    speech_events_ = speech.readiness_changed.connect(
        deps_.io, [this](speech::Slot slot, speech::Readiness readiness) {
            if (slot == speech::Slot::Dictation && readiness.state != speech::ReadyState::Loading &&
                readiness.state != speech::ReadyState::Warming) {
                engine_changed();
            }
        });
}

void RemoteEngineService::engine_changed() {
    if (auto server = host(); server && !stopping_) {
        deps_.io.post([guard = background_work_, weak = std::weak_ptr(server)] {
            guard->run([&weak] {
                if (const auto current = weak.lock()) {
                    current->engine_changed();
                }
            });
        });
    }
    changed.emit("engine");
}

bool RemoteEngineService::update_active() {
    const auto server = host();
    return server && server->update_active();
}

Json RemoteEngineService::update_status() {
    const auto server = host();
    return server ? server->update_status() : Json::object();
}

Json RemoteEngineService::prepare_update() {
    const auto server = host();
    return server ? server->prepare_update() : Json{{"reason", "Speech sharing is off"}};
}

bool RemoteEngineService::cancel_update(std::string_view token) {
    const auto server = host();
    return server && server->cancel_update(token);
}

std::optional<net::TailscaleStatus> RemoteEngineService::tailscale_status(bool refresh) {
    std::optional<net::TailscaleStatus> cached;
    bool start = false;
    {
        std::lock_guard lock(tailscale_mutex_);
        cached = tailscale_;
        const bool stale = !cached || deps_.clock.steady() - tailscale_at_ >= 30s;
        start = !stopping_ && !tailscale_refreshing_ && (refresh || stale);
        if (start) {
            tailscale_refreshing_ = true;
        }
    }
    if (start) {
        deps_.io.post([guard = background_work_, this] {
            guard->run([this] {
                (void)current_tailscale(0s);
                {
                    std::lock_guard lock(tailscale_mutex_);
                    tailscale_refreshing_ = false;
                }
                changed.emit("tailscale");
            });
        });
    }
    return cached;
}

net::TailscaleStatus RemoteEngineService::current_tailscale(std::chrono::seconds max_age) {
    std::lock_guard refresh(tailscale_refresh_mutex_);
    {
        std::lock_guard lock(tailscale_mutex_);
        if (max_age > 0s && tailscale_ && deps_.clock.steady() - tailscale_at_ < max_age) {
            return *tailscale_;
        }
    }
    auto result = deps_.tailscale->status();
    {
        std::lock_guard lock(tailscale_mutex_);
        tailscale_ = result;
        tailscale_at_ = deps_.clock.steady();
    }
    return result;
}

std::string RemoteEngineService::tailscale_owner() {
    if (deps_.settings.get(keys::remote_host_tailscale_trust)) {
        const auto status = current_tailscale(120s);
        if (status.running()) {
            return status.owner;
        }
    }
    return {};
}

std::vector<std::string> RemoteEngineService::host_addresses() {
    std::vector<std::string> addresses;
    for (const auto& address : local_addresses()) {
        if (address.is_lan || address.is_tailscale) {
            addresses.push_back(address.address);
        }
    }
    if (const auto status = current_tailscale(120s);
        status.running() && !status.address.empty() &&
        std::ranges::find(addresses, status.address) == addresses.end()) {
        addresses.push_back(status.address);
    }
    return addresses;
}

std::vector<net::LocalAddress> RemoteEngineService::local_addresses() const {
    return deps_.local_addresses ? deps_.local_addresses() : net::local_ipv4_addresses();
}

std::optional<std::vector<NetworkProfile>> RemoteEngineService::network_profiles(bool refresh) {
#ifndef _WIN32
    if (!deps_.network_profiles) {
        return std::vector<NetworkProfile>{}; // Only Windows has network profiles to read.
    }
#endif
    std::optional<std::vector<NetworkProfile>> cached;
    bool start = false;
    {
        std::lock_guard lock(tailscale_mutex_);
        cached = profiles_;
        const bool stale = !cached || deps_.clock.steady() - profiles_at_ >=
                                          std::chrono::duration<double>(NETWORK_PROFILES_MAX_AGE_S);
        start = !stopping_ && !profiles_refreshing_ && (refresh || stale);
        if (start) {
            profiles_refreshing_ = true;
        }
    }
    if (start) {
        deps_.io.post([guard = background_work_, this] {
            guard->run([this] {
                std::vector<NetworkProfile> read;
                try {
                    read = deps_.network_profiles ? deps_.network_profiles()
                                                  : windows_network_profiles();
                } catch (const std::exception& error) {
                    kLog.debug("Could not read the network profiles: {}", error.what());
                }
                bool differs = false;
                {
                    std::lock_guard lock(tailscale_mutex_);
                    profiles_refreshing_ = false;
                    differs = profiles_ != read;
                    profiles_ = std::move(read);
                    profiles_at_ = deps_.clock.steady();
                }
                if (differs) {
                    changed.emit("state");
                }
            });
        });
    }
    return cached;
}

remote::HostState RemoteEngineService::host_state() {
    assert_not_gui_thread();
    const auto settings = deps_.settings.snapshot();
    remote::HostState state;
    state.enabled = remote::host_enabled(*settings);
    state.port = remote::host_port(*settings);
    state.host_name = deps_.computer_name;
    state.tailscale_trust = remote::host_tailscale_trust(*settings);
    state.model_management = models_ && remote::host_model_management(*settings);
    state.keep_records = deps_.records && remote::host_keeps_records(*settings);
    state.engine = engine_state();
    state.devices = registry_->list();
    state.manage_mcp =
        deps_.mcp_session && std::ranges::any_of(state.devices, [this](const auto& peer) {
            return deps_.mcp_session->allowed(peer.id);
        });
    state.tailscale = tailscale_status();
    const auto server = host();
    {
        std::lock_guard lock(host_mutex_);
        state.fingerprint = fingerprint_;
        state.error = host_error_;
    }
    if (server && server->running()) {
        state.running = true;
        state.port = server->port().value_or(state.port);
        state.pairing = server->pairing_status();
        state.pair_request = server->pending_request();
        state.clients = server->connected_clients();
        const auto addresses = local_addresses();
        auto chosen = std::ranges::find_if(addresses, &net::LocalAddress::is_lan);
        if (chosen == addresses.end()) {
            chosen = std::ranges::find_if(addresses, &net::LocalAddress::is_vpn);
        }
        std::optional<net::LocalAddress> lan;
        if (chosen != addresses.end()) {
            state.address = remote::format_address(chosen->address, state.port);
            state.address_kind = chosen->kind;
            lan = *chosen;
        }
        // Why other computers may not reach this one (reachability.Note).
        try {
            state.notes = host_notes(lan, addresses, network_profiles(), server->discovery_error());
        } catch (const std::exception& error) {
            kLog.debug("Reachability check failed: {}", error.what());
        }
    }
    return state;
}

Json RemoteEngineService::host_activity(std::optional<int> events) {
    const auto server = host();
    return server && server->running() ? server->activity().snapshot(events) : empty_snapshot();
}

Json RemoteEngineService::engine_state() {
    const auto engine = deps_.engine ? deps_.engine() : nullptr;
    // host_engine_for(None): no engine still describes itself, as unavailable.
    return engine
               ? engine->describe()
               : unavailable_engine("No transcription engine is selected on the host")->describe();
}

std::vector<remote::ConnectedClient> RemoteEngineService::connected_clients() {
    const auto server = host();
    return server ? server->connected_clients() : std::vector<remote::ConnectedClient>{};
}

void RemoteEngineService::set_host_enabled(bool enabled) {
    if (!enabled && deps_.mcp_session) {
        deps_.mcp_session->revoke();
    }
    assert_not_gui_thread();
    {
        std::lock_guard lock(lifecycle_);
        deps_.settings.save_setting(keys::remote_host_enabled.key(), enabled);
        apply_host_settings_locked();
    }
    changed.emit("state");
}

void RemoteEngineService::set_host_port(int port) {
    assert_not_gui_thread();
    if (port < 1 || port > 65535) {
        throw py::ValueError("Enter a port from 1 to 65535.");
    }
    {
        std::lock_guard lock(lifecycle_);
        deps_.settings.save_setting(keys::remote_host_port.key(), port);
        apply_host_settings_locked();
    }
    changed.emit("state");
}

std::optional<std::string> RemoteEngineService::open_pairing() {
    const auto server = host();
    return server && server->running() ? std::make_optional(server->open_pairing()) : std::nullopt;
}

void RemoteEngineService::cancel_pairing() {
    if (const auto server = host()) {
        server->close_pairing();
    }
}

std::optional<remote::PairRequestInfo> RemoteEngineService::pending_pair_request() {
    const auto server = host();
    return server ? server->pending_request() : std::nullopt;
}

bool RemoteEngineService::answer_pair_request(std::string_view id, bool allow) {
    const auto server = host();
    return server && server->answer_request(id, allow);
}

std::vector<std::string> RemoteEngineService::pairing_grants() {
    const auto settings = deps_.settings.snapshot();
    std::vector<std::string> grants;
    if (models_ && remote::host_model_management(*settings)) {
        grants.push_back("download models and runtimes here");
    }
    if (deps_.records && remote::host_keeps_records(*settings)) {
        grants.push_back("keep its dictations and finished meetings here");
    }
    return grants;
}

bool RemoteEngineService::remove_device(std::string_view id, bool delete_records) {
    assert_not_gui_thread();
    const auto devices = registry_->list();
    const auto device = std::ranges::find_if(
        devices, [id](const remote::PairedDevice& value) { return value.id == id; });
    if (device == devices.end()) {
        return false;
    }
    if (delete_records && !deps_.records) {
        throw remote::RemoteEngineError("Remote record storage is unavailable.");
    }
    const auto server = host();
    const bool removed = server ? server->remove_device(id) : registry_->remove(id);
    if (removed && deps_.mcp_session && deps_.mcp_session->allowed(id)) {
        deps_.mcp_session->revoke();
    }
    if (removed && delete_records) {
        deps_.records->delete_device(id);
        for (const auto& owner : device->record_owner_ids) {
            deps_.records->delete_device(owner);
        }
        changed.emit("records");
    }
    if (removed && !server) {
        changed.emit("devices");
    }
    return removed;
}

std::optional<remote::PairedDevice> RemoteEngineService::rename_device(std::string_view id,
                                                                       std::string_view name) {
    assert_not_gui_thread();
    const auto server = host();
    auto result = server ? server->rename_device(id, name) : registry_->rename(id, name);
    if (result && !server) {
        changed.emit("devices");
    }
    if (result && deps_.records) {
        // service.py: "records" only when the store renamed something, after "devices".
        int renamed = deps_.records->rename_device(id, result->name);
        for (const auto& owner : result->record_owner_ids) {
            renamed += deps_.records->rename_device(owner, result->name);
        }
        if (renamed > 0) {
            changed.emit("records");
        }
    }
    return result;
}

void RemoteEngineService::set_option(std::string_view key, bool value, std::string_view event) {
    assert_not_gui_thread();
    deps_.settings.save_setting(key, value);
    changed.emit(std::string(event));
}

void RemoteEngineService::set_tailscale_trust(bool trust) {
    set_option(keys::remote_host_tailscale_trust.key(), trust, "tailscale");
}

void RemoteEngineService::set_keep_records(bool keep) {
    if (keep && !deps_.records) {
        throw remote::RemoteEngineError("Remote record storage is unavailable.");
    }
    set_option(keys::remote_host_keep_records.key(), keep, "records");
}

void RemoteEngineService::set_model_management(bool allow) {
    if (allow && !models_) {
        throw remote::RemoteEngineError("Remote model management is unavailable on this host.");
    }
    set_option(keys::remote_host_model_management.key(), allow, "models");
}

void RemoteEngineService::set_manage_mcp(bool allow) {
    if (allow) {
        throw remote::RemoteEngineError("Remote MCP management is unavailable on this host.");
    }
    if (deps_.mcp_session) {
        deps_.mcp_session->revoke();
        changed.emit("mcp");
    }
}

void RemoteEngineService::grant_mcp_session(std::string_view device) {
    assert_not_gui_thread();
    const auto server = host();
    const auto peers = registry_->list();
    if (!deps_.mcp_session || !server || !server->running() ||
        std::ranges::find(peers, device, &remote::PairedDevice::id) == peers.end()) {
        throw remote::RemoteEngineError("Choose a computer currently paired to this sharing host.");
    }
    if (!deps_.mcp_session->grant(std::string(device))) {
        throw remote::RemoteEngineError("End the current MCP grant before starting another.");
    }
    changed.emit("mcp");
}

std::string RemoteEngineService::local_mcp_session_token() {
    return deps_.mcp_session ? deps_.mcp_session->local_token() : std::string();
}

void RemoteEngineService::close_mcp_control() {
    std::lock_guard lock(mcp_client_mutex_);
    if (mcp_client_) {
        mcp_client_->close();
        mcp_client_.reset();
        mcp_client_peer_.reset();
    }
}

Json RemoteEngineService::records_summary(std::string_view id) {
    if (!deps_.records) {
        return Json::object();
    }
    for (const auto& device : registry_->list()) {
        if (device.id == id) {
            return deps_.records->summary(device);
        }
    }
    return Json::object();
}

std::vector<remote::RecordOwnerGroup> RemoteEngineService::recoverable_record_owners() {
    if (!deps_.records) {
        return {};
    }
    const auto devices = registry_->list();
    auto owners = deps_.records->owners();
    std::erase_if(owners, [&](const remote::RecordOwnerGroup& owner) {
        return std::ranges::any_of(devices, [&](const remote::PairedDevice& device) {
            return device.id == owner.id || std::ranges::find(device.record_owner_ids, owner.id) !=
                                                device.record_owner_ids.end();
        });
    });
    // service.py sorts the owners by (name, id).
    std::ranges::sort(owners, {}, [](const remote::RecordOwnerGroup& owner) {
        return std::tie(owner.name, owner.id);
    });
    return owners;
}

void RemoteEngineService::recover_device_records(std::string_view owner, std::string_view device) {
    assert_not_gui_thread();
    if (!deps_.records) {
        throw remote::RemoteEngineError("Remote record storage is unavailable.");
    }
    {
        const auto pause = deps_.records->gate().paused();
        const auto owners = recoverable_record_owners();
        if (std::ranges::none_of(owners, [owner](const remote::RecordOwnerGroup& group) {
                return group.id == owner;
            })) {
            throw py::ValueError("These records are no longer available for recovery.");
        }
        registry_->attach_record_owner(device, owner);
    }
    changed.emit("records");
}

std::vector<remote::HostModel> RemoteEngineService::host_models() {
    return models_ ? models_->ready_models() : std::vector<remote::HostModel>{};
}

std::uint64_t RemoteEngineService::begin_pairing() {
    assert_not_gui_thread();
    std::lock_guard lock(pairing_mutex_);
    if (stopping_) {
        throw remote::PairingCanceled("Pairing canceled.");
    }
    return ++pairing_generation_;
}

std::optional<remote::ClientPairing> RemoteEngineService::client_pairing() {
    assert_not_gui_thread();
    return remote::load_client_pairing(deps_.settings);
}

remote::ClientPairing RemoteEngineService::pair(std::string_view address, std::string_view code,
                                                bool tailscale) {
    const auto [host, port] = remote::parse_address(address);
    const auto generation = begin_pairing();
    auto result =
        remote::pair_with_host(deps_.client_env, host, port, code, deps_.computer_name, tailscale);
    return save_pairing({host, port, std::move(result)}, generation);
}

remote::ClientPairing RemoteEngineService::request_pairing(std::string_view address,
                                                           std::function<void(std::string)> on_code,
                                                           CancelToken cancel) {
    const auto [host, port] = remote::parse_address(address);
    const auto generation = begin_pairing();
    CancelSource stop(shutdown_.token());
    auto user_cancel = cancel.on_cancel([stop]() mutable { stop.cancel(); });
    auto result = remote::request_pairing(deps_.client_env, host, port, deps_.computer_name,
                                          std::move(on_code), stop.token());
    if (stop.cancelled()) {
        throw remote::PairingCanceled("Pairing canceled.");
    }
    return save_pairing({host, port, std::move(result)}, generation);
}

remote::ClientPairing RemoteEngineService::save_pairing_locked(const PendingPairing& pending) {
    if (stopping_) {
        throw remote::PairingCanceled("Pairing canceled.");
    }
    return remote::save_client_pairing(deps_.settings, deps_.credentials, deps_.paths,
                                       deps_.identity, pending.host, pending.port, pending.result,
                                       deps_.clock);
}

remote::ClientPairing RemoteEngineService::save_pairing(PendingPairing pending,
                                                        std::uint64_t generation) {
    remote::ClientPairing pairing;
    {
        std::lock_guard lock(pairing_mutex_);
        if (generation != pairing_generation_) {
            throw remote::PairingCanceled("Pairing canceled.");
        }
        try {
            pairing = save_pairing_locked(pending);
        } catch (const CredentialStoreError&) {
            pending_ = std::move(pending);
            throw;
        }
        pending_.reset();
    }
    changed.emit("client");
    return pairing;
}

std::string RemoteEngineService::pending_pairing_name() {
    std::lock_guard lock(pairing_mutex_);
    return pending_ ? pending_->result.host_name : std::string();
}

remote::ClientPairing RemoteEngineService::retry_pairing_save() {
    assert_not_gui_thread();
    remote::ClientPairing pairing;
    {
        std::lock_guard lock(pairing_mutex_);
        if (!pending_) {
            throw py::ValueError("Connect to a computer before retrying key storage.");
        }
        deps_.credentials.request_access(remote::token_credential(deps_.paths, deps_.identity));
        pairing = save_pairing_locked(*pending_);
        pending_.reset();
    }
    changed.emit("client");
    return pairing;
}

std::optional<remote::ClientPairing> RemoteEngineService::rename_host(std::string_view name) {
    assert_not_gui_thread();
    auto pairing = remote::rename_client_host(deps_.settings, name);
    if (pairing) {
        changed.emit("host_renamed");
    }
    return pairing;
}

void RemoteEngineService::forget_host() {
    close_mcp_control();
    assert_not_gui_thread();
    {
        std::lock_guard lock(pairing_mutex_);
        ++pairing_generation_;
        pending_.reset();
        remote::forget_client_pairing(deps_.settings, deps_.credentials, deps_.paths,
                                      deps_.identity);
    }
    changed.emit("client");
}

remote::NearbyScan RemoteEngineService::scan_nearby(bool sweep) {
    assert_not_gui_thread();
    const auto fingerprint = own_fingerprint(deps_.paths.data_root());
    return remote::scan_nearby(deps_.client_env, *deps_.tailscale, sweep, fingerprint);
}

std::unique_ptr<remote::RemoteConnection>
RemoteEngineService::make_connection(const std::optional<remote::ClientPairing>& expected,
                                     std::optional<bool> history, bool reports_recording,
                                     std::string_view missing, std::string_view changed_message) {
    assert_not_gui_thread();
    std::lock_guard lock(pairing_mutex_);
    const auto pairing = remote::load_client_pairing(deps_.settings);
    if (stopping_ || !pairing) {
        throw remote::RemoteEngineError(std::string(missing));
    }
    if (expected && *pairing != *expected) {
        throw remote::RemoteEngineError(std::string(changed_message));
    }
    const auto token = remote::load_client_token(deps_.credentials, deps_.env, dotenv_, deps_.paths,
                                                 deps_.identity);
    if (!token || token->empty()) {
        // Only pairing again fixes it, so the dictation engine's link stops retrying.
        throw remote::RemotePairingIncomplete(std::string(missing));
    }
    return std::make_unique<remote::RemoteConnection>(
        deps_.client_env, pairing->host, pairing->port, *token, pairing->fingerprint,
        pairing->alternates, history, reports_recording);
}

std::unique_ptr<remote::RemoteConnection>
RemoteEngineService::client_connection(const std::optional<remote::ClientPairing>& expected,
                                       std::optional<bool> history, bool reports_recording) {
    return make_connection(expected, history, reports_recording, "Pair with a host first.",
                           "The paired host changed.");
}

std::string remote_model_refusal(std::string_view op, const Json& fields, const Json& ready,
                                 std::string_view product) {
    const auto& caps = capabilities(ready);
    if (!caps.contains("model_management")) {
        return std::format("Update {} on the host to manage its models remotely.", product);
    }
    if (caps["model_management"] != true) {
        return "Model management is disabled on the host. Enable it in Settings → Remote engine "
               "there.";
    }
    if ((op == "install_runtime" || (fields.is_object() && fields.contains("device"))) &&
        (!caps.contains("runtime_installation") || caps["runtime_installation"] != true)) {
        return std::format(
            "Update {} on the host to install runtimes and choose a model's device remotely.",
            product);
    }
    if (op == "configure_runtime" &&
        (!caps.contains("engine_controls") || caps["engine_controls"] != true)) {
        return "This host cannot change the speech runtime remotely.";
    }
    return {};
}

Json RemoteEngineService::remote_model_request(
    std::string_view op, const Json& fields, const std::optional<remote::ClientPairing>& expected) {
    if (op != "model_catalog" && op != "download_model" && op != "install_runtime" &&
        op != "select_model" && op != "configure_runtime") {
        throw py::ValueError("Unknown model management operation.");
    }
    auto connection = make_connection(
        expected, {}, false, "Pair with a host before managing its models.",
        "The paired host changed. Close this window and open Manage host models again.");
    // Under the shutdown token, as remote_mcp_request's: a select_model waits up to 250 s for
    // the host's load, and an io task still waiting would hold Quit that long.
    const Json ready = connection->connect(shutdown_.token());
    const auto& caps = capabilities(ready);
    if (auto refusal = remote_model_refusal(op, fields, ready, deps_.identity.product);
        !refusal.empty()) {
        throw remote::RemoteEngineError(std::move(refusal));
    }
    const auto timeout = op == "select_model" || op == "configure_runtime" ? 250s : 30s;
    auto result = connection->request_timed(op, fields, timeout, {}, {}, shutdown_.token()).first;
    if (op == "model_catalog" && result.is_object()) {
        result["active_runtime"] = ready.value("runtime", Json::object());
        result["can_configure_runtime"] = caps.value("engine_controls", false);
    }
    return result;
}

Json RemoteEngineService::remote_mcp_request(std::string_view op, const Json& fields,
                                             const std::optional<remote::ClientPairing>& expected) {
    if (op != "mcp_state" && op != "mcp_configure") {
        throw py::ValueError("Unknown MCP operation.");
    }
    const bool configure = op == "mcp_configure";
    if (configure &&
        (!expected || !fields.is_object() || fields.size() != 1 || !fields.contains("settings") ||
         !fields["settings"].is_object() || fields["settings"].size() != 1 ||
         !fields["settings"].contains("enabled") || !fields["settings"]["enabled"].is_boolean())) {
        throw py::ValueError("Confirm the exact host and the fixed loopback session first.");
    }
    const bool start = configure && fields["settings"]["enabled"] == true;
    std::lock_guard lock(mcp_client_mutex_);
    if (mcp_client_ && (!mcp_client_->alive() || !expected || !mcp_client_peer_ ||
                        *mcp_client_peer_ != *expected)) {
        mcp_client_->close();
        mcp_client_.reset();
        mcp_client_peer_.reset();
    }
    std::shared_ptr<remote::RemoteConnection> connection = mcp_client_;
    if (!connection) {
        connection =
            make_connection(expected, {}, false, "Pair with a host before managing its MCP server.",
                            "The paired host changed.");
    }
    if (const auto current = client_pairing(); !current || (expected && *current != *expected)) {
        throw remote::RemoteEngineError("The paired host changed. Confirm the new destination.");
    }
    const Json ready = connection->connect(shutdown_.token());
    const auto& caps = capabilities(ready);
    if (!caps.contains("mcp_control")) {
        throw remote::RemoteRequestError(
            std::format("Update {} on the host to manage its MCP server from here.",
                        deps_.identity.product),
            "unsupported");
    }
    if (caps["mcp_control"] != true) {
        // The host may be OpenWhisper_CPP, which grants one computer a session ("Grant session MCP
        // control to one paired computer"), or OpenWhisper, whose switch allows every paired
        // computer: the words fit both.
        throw remote::RemoteRequestError(
            "The host isn't letting this computer manage its MCP server. On the host, grant this "
            "computer MCP control in Settings → Remote engine.",
            "forbidden");
    }
    auto result = connection->request_timed(op, fields, 30s, {}, {}, shutdown_.token()).first;
    if (start) {
        mcp_client_ = std::move(connection);
        mcp_client_peer_ = expected;
    } else if (configure && mcp_client_) {
        mcp_client_->close();
        mcp_client_.reset();
        mcp_client_peer_.reset();
    }
    return result;
}

} // namespace fv::remote_host
