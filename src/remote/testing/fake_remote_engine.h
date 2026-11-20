#pragma once

#include <exception>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "remote/control.h"

namespace fv::remote::testing {

// A RemoteEngineControl with scripted state for pages, dialogs and the dashboard. Each method
// records a call ("set_host_enabled(true)", "pair(192.168.1.20:47821,123456,false)", ...) and
// answers from the public fields below, which a test sets before (or between) calls; setters
// that the real service applies (set_host_enabled, set_*, rename_host, forget_host, cancel_pairing,
// answer_pair_request, remove_device, rename_device) also update the scripted state. `fail_next`
// makes the next blocking call that can fail throw that error instead.
// Affinity: thread-safe once scripted: the methods share one lock and emit `changed` outside it;
// a test writes the public fields only while no other thread is calling in.
class FakeRemoteEngine : public RemoteEngineControl {
public:
    // ---- scripted state ----
    HostState state;
    Json activity = Json::object();
    Json engine = nullptr;
    std::vector<HostModel> models;
    std::vector<std::string> grants;
    std::optional<std::string> pairing_code = std::string("123456");
    std::vector<RecordOwnerGroup> owners;
    Json summary = Json::object();
    std::optional<ClientPairing> pairing;
    std::string pending_name;
    // What pair, request_pairing and retry_pairing_save return (a pairing with this host_name and
    // the address paired with when unset).
    std::optional<ClientPairing> pair_result;
    // Handed to request_pairing's on_code before it returns.
    std::optional<std::string> sas = std::string("123456");
    NearbyScan scan;
    std::optional<net::TailscaleStatus> tailscale_value;
    Json request_reply = Json::object();

    // The next pair, request_pairing, retry_pairing_save, scan_nearby, remote_model_request or
    // remote_mcp_request throws `error` (once).
    void fail_next(std::exception_ptr error);
    // changed.emit(kind), from the calling thread.
    void emit_changed(const std::string& kind) { changed.emit(kind); }
    std::vector<std::string> calls() const;
    void clear_calls();

    // ---- RemoteEngineControl ----
    HostState host_state() override;
    Json host_activity(std::optional<int> events = {}) override;
    Json engine_state() override;
    std::vector<ConnectedClient> connected_clients() override;
    void set_host_enabled(bool enabled) override;
    void set_host_port(int port) override;
    std::optional<std::string> open_pairing() override;
    void cancel_pairing() override;
    std::optional<PairRequestInfo> pending_pair_request() override;
    bool answer_pair_request(std::string_view id, bool allow) override;
    std::vector<std::string> pairing_grants() override;
    bool remove_device(std::string_view id, bool delete_records) override;
    std::optional<PairedDevice> rename_device(std::string_view id, std::string_view name) override;
    void set_tailscale_trust(bool trust) override;
    void set_keep_records(bool keep) override;
    void set_model_management(bool allow) override;
    void set_manage_mcp(bool allow) override;
    Json records_summary(std::string_view device_id) override;
    std::vector<RecordOwnerGroup> recoverable_record_owners() override;
    void recover_device_records(std::string_view owner, std::string_view device) override;
    std::vector<HostModel> host_models() override;
    std::optional<ClientPairing> client_pairing() override;
    ClientPairing pair(std::string_view address, std::string_view code, bool tailscale) override;
    ClientPairing request_pairing(std::string_view address,
                                  std::function<void(std::string sas)> on_code,
                                  CancelToken cancel) override;
    std::string pending_pairing_name() override;
    ClientPairing retry_pairing_save() override;
    std::optional<ClientPairing> rename_host(std::string_view name) override;
    void forget_host() override;
    NearbyScan scan_nearby(bool sweep) override;
    std::optional<net::TailscaleStatus> tailscale_status(bool refresh = false) override;
    Json remote_model_request(std::string_view op, const Json& fields,
                              const std::optional<ClientPairing>& expected = {}) override;
    Json remote_mcp_request(std::string_view op, const Json& fields,
                            const std::optional<ClientPairing>& expected = {}) override;
    void close_mcp_control() override;

private:
    void record(std::string call);
    void throw_if_failing();
    ClientPairing paired_with(std::string_view address);

    mutable std::recursive_mutex mutex_;
    std::vector<std::string> calls_;
    std::exception_ptr failure_;
};

} // namespace fv::remote::testing
