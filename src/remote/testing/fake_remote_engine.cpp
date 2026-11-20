#include "remote/testing/fake_remote_engine.h"

#include <algorithm>
#include <format>
#include <utility>

namespace fv::remote::testing {

namespace {

const char* flag(bool value) {
    return value ? "true" : "false";
}

} // namespace

void FakeRemoteEngine::fail_next(std::exception_ptr error) {
    std::lock_guard lock(mutex_);
    failure_ = std::move(error);
}

std::vector<std::string> FakeRemoteEngine::calls() const {
    std::lock_guard lock(mutex_);
    return calls_;
}

void FakeRemoteEngine::clear_calls() {
    std::lock_guard lock(mutex_);
    calls_.clear();
}

void FakeRemoteEngine::record(std::string call) {
    std::lock_guard lock(mutex_);
    calls_.push_back(std::move(call));
}

void FakeRemoteEngine::throw_if_failing() {
    std::exception_ptr failure;
    {
        std::lock_guard lock(mutex_);
        failure = std::exchange(failure_, nullptr);
    }
    if (failure) {
        std::rethrow_exception(failure);
    }
}

ClientPairing FakeRemoteEngine::paired_with(std::string_view address) {
    std::lock_guard lock(mutex_);
    if (pair_result) {
        return *pair_result;
    }
    ClientPairing made;
    const auto [host, port] = parse_address(address);
    made.host = host;
    made.port = port;
    made.fingerprint = std::string(64, 'A');
    made.host_name = host;
    made.device_id = "fake-device";
    made.paired_at = "2026-01-01T00:00:00+00:00";
    return made;
}

HostState FakeRemoteEngine::host_state() {
    std::lock_guard lock(mutex_);
    calls_.emplace_back("host_state()");
    return state;
}

Json FakeRemoteEngine::host_activity(std::optional<int> events) {
    std::lock_guard lock(mutex_);
    calls_.push_back(events ? std::format("host_activity({})", *events) : "host_activity()");
    return activity;
}

Json FakeRemoteEngine::engine_state() {
    std::lock_guard lock(mutex_);
    calls_.emplace_back("engine_state()");
    return engine;
}

std::vector<ConnectedClient> FakeRemoteEngine::connected_clients() {
    std::lock_guard lock(mutex_);
    calls_.emplace_back("connected_clients()");
    return state.clients;
}

void FakeRemoteEngine::set_host_enabled(bool enabled) {
    {
        std::lock_guard lock(mutex_);
        calls_.push_back(std::format("set_host_enabled({})", flag(enabled)));
        state.enabled = enabled;
        state.running = enabled;
    }
    changed.emit("state");
}

void FakeRemoteEngine::set_host_port(int port) {
    {
        std::lock_guard lock(mutex_);
        calls_.push_back(std::format("set_host_port({})", port));
        state.port = port;
    }
    changed.emit("state");
}

std::optional<std::string> FakeRemoteEngine::open_pairing() {
    std::optional<std::string> code;
    {
        std::lock_guard lock(mutex_);
        calls_.emplace_back("open_pairing()");
        code = pairing_code;
        if (code) {
            state.pairing = std::pair(*code, 300.0);
        }
    }
    changed.emit("pairing");
    return code;
}

void FakeRemoteEngine::cancel_pairing() {
    {
        std::lock_guard lock(mutex_);
        calls_.emplace_back("cancel_pairing()");
        state.pairing.reset();
    }
    changed.emit("pairing");
}

std::optional<PairRequestInfo> FakeRemoteEngine::pending_pair_request() {
    std::lock_guard lock(mutex_);
    calls_.emplace_back("pending_pair_request()");
    return state.pair_request;
}

bool FakeRemoteEngine::answer_pair_request(std::string_view id, bool allow) {
    bool answered = false;
    {
        std::lock_guard lock(mutex_);
        calls_.push_back(std::format("answer_pair_request({},{})", id, flag(allow)));
        answered = state.pair_request && state.pair_request->id == id;
        if (answered) {
            state.pair_request.reset();
        }
    }
    if (answered) {
        changed.emit("pair_request");
    }
    return answered;
}

std::vector<std::string> FakeRemoteEngine::pairing_grants() {
    std::lock_guard lock(mutex_);
    calls_.emplace_back("pairing_grants()");
    return grants;
}

bool FakeRemoteEngine::remove_device(std::string_view id, bool delete_records) {
    bool removed = false;
    {
        std::lock_guard lock(mutex_);
        calls_.push_back(std::format("remove_device({},{})", id, flag(delete_records)));
        removed =
            std::erase_if(state.devices, [&](const PairedDevice& d) { return d.id == id; }) > 0;
    }
    if (removed) {
        changed.emit("devices");
    }
    return removed;
}

std::optional<PairedDevice> FakeRemoteEngine::rename_device(std::string_view id,
                                                            std::string_view name) {
    std::optional<PairedDevice> renamed;
    {
        std::lock_guard lock(mutex_);
        calls_.push_back(std::format("rename_device({},{})", id, name));
        for (auto& device : state.devices) {
            if (device.id == id) {
                const std::string own =
                    device.paired_name.empty() ? device.name : device.paired_name;
                std::string chosen = clean_name(name);
                if (chosen.empty()) {
                    chosen = own;
                }
                device.name = chosen;
                device.paired_name = chosen != own ? own : std::string();
                renamed = device;
            }
        }
    }
    if (renamed) {
        changed.emit("devices");
    }
    return renamed;
}

void FakeRemoteEngine::set_tailscale_trust(bool trust) {
    {
        std::lock_guard lock(mutex_);
        calls_.push_back(std::format("set_tailscale_trust({})", flag(trust)));
        state.tailscale_trust = trust;
    }
    changed.emit("state");
}

void FakeRemoteEngine::set_keep_records(bool keep) {
    {
        std::lock_guard lock(mutex_);
        calls_.push_back(std::format("set_keep_records({})", flag(keep)));
        state.keep_records = keep;
    }
    changed.emit("state");
}

void FakeRemoteEngine::set_model_management(bool allow) {
    {
        std::lock_guard lock(mutex_);
        calls_.push_back(std::format("set_model_management({})", flag(allow)));
        state.model_management = allow;
    }
    changed.emit("state");
}

void FakeRemoteEngine::set_manage_mcp(bool allow) {
    {
        std::lock_guard lock(mutex_);
        calls_.push_back(std::format("set_manage_mcp({})", flag(allow)));
        state.manage_mcp = allow;
    }
    changed.emit("state");
}

Json FakeRemoteEngine::records_summary(std::string_view device_id) {
    std::lock_guard lock(mutex_);
    calls_.push_back(std::format("records_summary({})", device_id));
    return summary;
}

std::vector<RecordOwnerGroup> FakeRemoteEngine::recoverable_record_owners() {
    std::lock_guard lock(mutex_);
    calls_.emplace_back("recoverable_record_owners()");
    return owners;
}

void FakeRemoteEngine::recover_device_records(std::string_view owner, std::string_view device) {
    record(std::format("recover_device_records({},{})", owner, device));
    changed.emit("records");
}

std::vector<HostModel> FakeRemoteEngine::host_models() {
    std::lock_guard lock(mutex_);
    calls_.emplace_back("host_models()");
    return models;
}

std::optional<ClientPairing> FakeRemoteEngine::client_pairing() {
    std::lock_guard lock(mutex_);
    calls_.emplace_back("client_pairing()");
    return pairing;
}

ClientPairing FakeRemoteEngine::pair(std::string_view address, std::string_view code,
                                     bool tailscale) {
    record(std::format("pair({},{},{})", address, code, flag(tailscale)));
    throw_if_failing();
    ClientPairing made = paired_with(address);
    {
        std::lock_guard lock(mutex_);
        pairing = made;
    }
    changed.emit("client");
    return made;
}

ClientPairing FakeRemoteEngine::request_pairing(std::string_view address,
                                                std::function<void(std::string sas)> on_code,
                                                CancelToken cancel) {
    record(std::format("request_pairing({})", address));
    std::optional<std::string> number;
    {
        std::lock_guard lock(mutex_);
        number = sas;
    }
    if (number && on_code) {
        on_code(*number);
    }
    if (cancel.cancelled()) {
        throw PairingCanceled("Pairing canceled.");
    }
    throw_if_failing();
    ClientPairing made = paired_with(address);
    {
        std::lock_guard lock(mutex_);
        pairing = made;
    }
    changed.emit("client");
    return made;
}

std::string FakeRemoteEngine::pending_pairing_name() {
    std::lock_guard lock(mutex_);
    calls_.emplace_back("pending_pairing_name()");
    return pending_name;
}

ClientPairing FakeRemoteEngine::retry_pairing_save() {
    record("retry_pairing_save()");
    throw_if_failing();
    std::string address = "host";
    {
        std::lock_guard lock(mutex_);
        if (pairing) {
            address = pairing->address();
        }
    }
    ClientPairing made = paired_with(address);
    {
        std::lock_guard lock(mutex_);
        pairing = made;
        pending_name.clear();
    }
    changed.emit("client");
    return made;
}

std::optional<ClientPairing> FakeRemoteEngine::rename_host(std::string_view name) {
    std::optional<ClientPairing> renamed;
    {
        std::lock_guard lock(mutex_);
        calls_.push_back(std::format("rename_host({})", name));
        if (pairing) {
            const std::string own =
                pairing->paired_name.empty() ? pairing->host_name : pairing->paired_name;
            std::string chosen = clean_name(name);
            if (chosen.empty()) {
                chosen = own;
            }
            pairing->host_name = chosen;
            pairing->paired_name = chosen != own ? own : std::string();
            renamed = pairing;
        }
    }
    if (renamed) {
        changed.emit("host_renamed");
    }
    return renamed;
}

void FakeRemoteEngine::forget_host() {
    {
        std::lock_guard lock(mutex_);
        calls_.emplace_back("forget_host()");
        pairing.reset();
    }
    changed.emit("client");
}

NearbyScan FakeRemoteEngine::scan_nearby(bool sweep) {
    record(std::format("scan_nearby({})", flag(sweep)));
    throw_if_failing();
    std::lock_guard lock(mutex_);
    NearbyScan result = scan;
    result.swept = result.swept || sweep;
    return result;
}

std::optional<net::TailscaleStatus> FakeRemoteEngine::tailscale_status(bool refresh) {
    std::lock_guard lock(mutex_);
    calls_.push_back(std::format("tailscale_status({})", flag(refresh)));
    return tailscale_value;
}

Json FakeRemoteEngine::remote_model_request(std::string_view op, const Json& fields,
                                            const std::optional<ClientPairing>& expected) {
    record(std::format("remote_model_request({},{}{})", op, fields.dump(),
                       expected ? ",expected" : ""));
    throw_if_failing();
    std::lock_guard lock(mutex_);
    return request_reply;
}

Json FakeRemoteEngine::remote_mcp_request(std::string_view op, const Json& fields,
                                          const std::optional<ClientPairing>& expected) {
    record(
        std::format("remote_mcp_request({},{}{})", op, fields.dump(), expected ? ",expected" : ""));
    throw_if_failing();
    std::lock_guard lock(mutex_);
    return request_reply;
}

void FakeRemoteEngine::close_mcp_control() {
    record("close_mcp_control()");
}

} // namespace fv::remote::testing
