#pragma once

#include <atomic>
#include <memory>
#include <mutex>

#include "base/cancel.h"
#include "base/subscription.h"
#include "remote/connection.h"
#include "remote/control.h"
#include "remote/remote_backend.h"
#include "remote_host/history_broker.h"
#include "remote_host/model_management.h"
#include "remote_host/reachability.h"
#include "remote_host/speech_host.h"
#include "remote_records/host_store.h"

namespace fv::models {
class ComponentCoordinator;
}

namespace fv::remote_host {
class PeerMcpSession;

struct RemoteEngineServiceDeps {
    const Paths& paths;
    const AppIdentity& identity;
    SettingsStore& settings;
    CredentialStore& credentials;
    const EnvReader& env;
    Clock& clock;
    Executor& io;
    remote::ClientEnv client_env;
    std::string computer_name;
    std::shared_ptr<remote::TailscaleCli> tailscale;
    std::function<std::shared_ptr<HostEngine>()> engine;
    models::ComponentCoordinator* components = nullptr;
    models::SpeechModelCache* model_cache = nullptr;
    models::HuggingFaceAccessCoordinator* model_access = nullptr;
    models::EngineControl* engine_control = nullptr;
    speech::SpeechService* speech_service = nullptr;
    Executor* ui = nullptr;
    models::CachedModelsSnapshot* inventory = nullptr;
    std::shared_ptr<remote_records::HostRecordStore> records;
    std::string bind = "0.0.0.0";
    std::optional<int> discovery_port = remote::DEFAULT_PORT;
    std::optional<int> listen_port; // 0 lets a loopback test use an ephemeral port.
    std::shared_ptr<PeerMcpSession> mcp_session;
    // This computer's IPv4 addresses; null is net::local_ipv4_addresses (tests pin them).
    std::function<std::vector<net::LocalAddress>()> local_addresses;
    // Reads the Windows network profiles (blocks); null is windows_network_profiles on Windows
    // and none elsewhere.
    std::function<std::vector<NetworkProfile>()> network_profiles;
};

// remote_model_request's checks of the paired host's `ready` before anything is sent: the
// sentence that refuses `op` with `fields` (a host too old to manage models, to install runtimes
// and choose a device, or to change its runtime; model management off there), or "" when the
// host takes it. `product` names the app in "Update <product> on the host ...".
std::string remote_model_refusal(std::string_view op, const Json& fields, const Json& ready,
                                 std::string_view product);

// Threads other than the GUI call the settings and network operations. The app stops sharing
// before shutting down its network reactor or executors.
class RemoteEngineService final : public remote::RemoteEngineControl,
                                  public std::enable_shared_from_this<RemoteEngineService> {
public:
    explicit RemoteEngineService(RemoteEngineServiceDeps deps);
    ~RemoteEngineService() override;

    void apply_host_settings();
    // The dictation slot's remote engine. It keeps its own link: a host that drops is tried
    // again on `io` after remote::REMOTE_RETRY_DELAYS, without a reload.
    std::unique_ptr<speech::Backend> make_backend();
    // The link of the dictation engine make_backend() made last (RemoteRuntime.retry_now and
    // .restoring); inert once the SpeechService has destroyed that engine. Any thread.
    remote::RemoteSpeechBackend::LinkHandle dictation_link() const;
    // That engine's link_changed, re-emitted on `io` (RemoteRuntime.remote_link_poke): read a
    // fresh dictation_link().link() then.
    Signal<> dictation_link_changed;
    std::unique_ptr<speech::Backend> make_meeting_backend(std::shared_ptr<Json> route);
    // Client: has the next make_backend()'s load switch the paired computer to `model`
    // (RemoteSpeechBackend.request_model); the caller then reloads the engine. The host's reason
    // for keeping its model arrives as that load's Readiness::message.
    // With `runtime`, that load asks the host to apply those runtime settings to the model
    // instead (RemoteSpeechBackend.request_runtime; the caller checked it against the backend's
    // state with remote::check_runtime_request).
    remote::HostModel request_model(std::string_view family, std::string_view model,
                                    std::optional<Json> runtime = std::nullopt);
    void engine_changed();
    void watch_engine(speech::SpeechService& speech);
    void shutdown();
    remote::ClientHistorySource& history() { return history_; }
    remote_records::RecordGate* record_gate() {
        return deps_.records ? &deps_.records->gate() : nullptr;
    }
    bool update_active();
    Json update_status();
    Json prepare_update();
    bool cancel_update(std::string_view token);

    // The Windows network profiles; nullopt until first read. Never blocks: a stale or missing
    // answer is read again on `io`, and listeners hear "state" when it changes.
    std::optional<std::vector<NetworkProfile>> network_profiles(bool refresh = false);

    remote::HostState host_state() override;
    Json host_activity(std::optional<int> events = {}) override;
    Json engine_state() override;
    std::vector<remote::ConnectedClient> connected_clients() override;
    void set_host_enabled(bool enabled) override;
    void set_host_port(int port) override;
    std::optional<std::string> open_pairing() override;
    void cancel_pairing() override;
    std::optional<remote::PairRequestInfo> pending_pair_request() override;
    bool answer_pair_request(std::string_view id, bool allow) override;
    std::vector<std::string> pairing_grants() override;
    bool remove_device(std::string_view id, bool delete_records) override;
    std::optional<remote::PairedDevice> rename_device(std::string_view id,
                                                      std::string_view name) override;
    void set_tailscale_trust(bool trust) override;
    void set_keep_records(bool keep) override;
    void set_model_management(bool allow) override;
    void set_manage_mcp(bool allow) override;
    void grant_mcp_session(std::string_view device) override;
    std::string local_mcp_session_token() override;
    void close_mcp_control() override;
    Json records_summary(std::string_view device_id) override;
    std::vector<remote::RecordOwnerGroup> recoverable_record_owners() override;
    void recover_device_records(std::string_view owner, std::string_view device) override;
    std::vector<remote::HostModel> host_models() override;

    std::optional<remote::ClientPairing> client_pairing() override;
    remote::ClientPairing pair(std::string_view address, std::string_view code,
                               bool tailscale) override;
    remote::ClientPairing request_pairing(std::string_view address,
                                          std::function<void(std::string)> on_code,
                                          CancelToken cancel) override;
    std::string pending_pairing_name() override;
    remote::ClientPairing retry_pairing_save() override;
    std::optional<remote::ClientPairing> rename_host(std::string_view name) override;
    void forget_host() override;
    remote::NearbyScan scan_nearby(bool sweep) override;
    std::optional<net::TailscaleStatus> tailscale_status(bool refresh = false) override;
    Json remote_model_request(std::string_view op, const Json& fields,
                              const std::optional<remote::ClientPairing>& expected = {}) override;
    Json remote_mcp_request(std::string_view op, const Json& fields,
                            const std::optional<remote::ClientPairing>& expected = {}) override;

    // A new connection for a speech backend, with current persisted credentials and route.
    // An expected pairing guards an open dialog from sending a command to another host.
    std::unique_ptr<remote::RemoteConnection>
    client_connection(const std::optional<remote::ClientPairing>& expected = {},
                      std::optional<bool> history = {}, bool reports_recording = false);

private:
    struct PendingPairing {
        std::string host;
        int port;
        remote::PairingResult result;
    };

    std::shared_ptr<SpeechHost> host() const;
    void apply_host_settings_locked();
    std::vector<std::string> host_addresses();
    std::string tailscale_owner();
    net::TailscaleStatus current_tailscale(std::chrono::seconds max_age);
    std::uint64_t begin_pairing();
    void set_option(std::string_view key, bool value, std::string_view event);
    remote::ClientPairing save_pairing_locked(const PendingPairing& pending);
    remote::ClientPairing save_pairing(PendingPairing pending, std::uint64_t generation);
    std::unique_ptr<remote::RemoteConnection>
    make_connection(const std::optional<remote::ClientPairing>& expected,
                    std::optional<bool> history, bool reports_recording, std::string_view missing,
                    std::string_view changed_message);

    RemoteEngineServiceDeps deps_;
    std::mutex mcp_client_mutex_;
    std::shared_ptr<remote::RemoteConnection> mcp_client_;
    std::optional<remote::ClientPairing> mcp_client_peer_;
    DotEnv dotenv_;
    std::unique_ptr<DeviceRegistry> registry_;
    HistoryBroker history_;
    std::shared_ptr<HostModelManager> models_;
    CancelSource shutdown_;
    std::atomic<bool> stopping_{false};
    std::mutex lifecycle_;
    mutable std::mutex host_mutex_;
    std::shared_ptr<SpeechHost> host_;
    std::string fingerprint_;
    std::string host_error_;
    Subscription host_events_;
    Subscription speech_events_;
    mutable std::mutex dictation_mutex_;
    remote::RemoteSpeechBackend::LinkHandle dictation_link_;
    Subscription dictation_link_watch_;
    std::mutex requested_mutex_;
    std::optional<remote::HostModel> requested_model_;
    std::optional<Json> requested_runtime_;
    std::mutex pairing_mutex_;
    std::optional<PendingPairing> pending_;
    std::uint64_t pairing_generation_ = 0;
    std::vector<net::LocalAddress> local_addresses() const;
    std::shared_ptr<CallbackGuard> background_work_ = std::make_shared<CallbackGuard>();
    std::mutex tailscale_refresh_mutex_;
    std::mutex tailscale_mutex_;
    bool tailscale_refreshing_ = false;
    std::optional<net::TailscaleStatus> tailscale_;
    std::chrono::steady_clock::time_point tailscale_at_{};
    bool profiles_refreshing_ = false;
    std::optional<std::vector<NetworkProfile>> profiles_;
    std::chrono::steady_clock::time_point profiles_at_{};
};

} // namespace fv::remote_host
