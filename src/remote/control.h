#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "base/cancel.h"
#include "base/json.h"
#include "base/signal.h"
#include "net/tailscale.h"
#include "remote/discovery_codec.h"
#include "remote/errors.h"
#include "remote/link.h"
#include "remote/pairing_settings.h"
#include "remote/protocol.h"

// What the Settings page, the Host Mode dashboard, the pair-request dialog and the dictation
// runtime ask of the remote engine, in both roles (the surface of
// services/remote_asr/service.py's RemoteEngineService that the UI reads). The service implements
// it; features look it up with ctx.find<RemoteEngineControl>() and show their unbound state when
// nothing provides it.
namespace fv::remote {

// A computer paired with this host (remote_host_devices without the token digest).
struct PairedDevice {
    std::string id;
    std::string name;
    std::string paired_at; // ISO 8601
    std::string last_seen; // ISO 8601
    std::string via;       // one of PAIRING_ROUTES
    std::string paired_name;
    std::vector<std::string> record_owner_ids;

    friend bool operator==(const PairedDevice&, const PairedDevice&) = default;
};

// One open speech connection from a paired computer.
struct ConnectedClient {
    std::string device_id;
    std::string name;
    std::string address;
    bool busy = false;
    double since = 0.0; // the host's time.time() when it connected, in seconds
    bool recording = false;

    friend bool operator==(const ConnectedClient&, const ConnectedClient&) = default;
};

// A pairing request waiting for this host's owner.
struct PairRequestInfo {
    std::string id;
    std::string name;
    std::string address;
    std::string sas; // six digits
    double seconds_left = 0.0;

    friend bool operator==(const PairRequestInfo&, const PairRequestInfo&) = default;
};

// A reachability problem and how to fix it (firewall, VPN, public network); `action` is the
// button's text and `url` what it opens, both empty when there is nothing to click.
struct Note {
    std::string kind;
    std::string message;
    std::string action;
    std::string url;

    friend bool operator==(const Note&, const Note&) = default;
};

// host_state(): everything the host side of the page and the dashboard draw.
struct HostState {
    bool enabled = false;
    bool running = false;
    int port = DEFAULT_PORT;
    std::string error;
    std::string host_name;
    std::optional<std::string> address;      // host:port, IPv6 bracketed
    std::optional<std::string> address_kind; // "lan" or "vpn"
    std::string fingerprint;
    // The pairing code and its seconds left, while one is open.
    std::optional<std::pair<std::string, double>> pairing;
    std::vector<ConnectedClient> clients;
    std::vector<PairedDevice> devices;
    Json engine; // null, or describe()
    std::optional<net::TailscaleStatus> tailscale;
    bool tailscale_trust = true;
    bool model_management = false;
    bool keep_records = false;
    bool manage_mcp = false;
    std::optional<PairRequestInfo> pair_request;
    std::vector<Note> notes;
};

// A computer on the tailnet that answered the remote engine probe.
struct TailnetHost {
    net::TailscalePeer peer;
    int port = DEFAULT_PORT;
    std::string host_name;
    Json engine = Json::object();
    bool tailscale_pairing = false;
    bool compatible = false;
    std::string fingerprint;
    bool approval = false;

    std::string address() const { return format_address(peer.address, port); }
    bool can_pair_without_code() const { return compatible && tailscale_pairing && peer.mine; }
};

// A computer sharing its engine, found on this network, the tailnet, or both; finds by both
// routes are one entry, matched by certificate.
struct NearbyHost {
    std::string host_name;
    std::string fingerprint;
    Json engine = Json::object();
    bool compatible = false;
    // Takes pairing requests that its owner allows on its own screen.
    bool approval = false;
    std::optional<LanHost> lan;
    std::optional<TailnetHost> tailnet;

    // Its Tailscale machine name when it has one, as the tailnet lists it.
    std::string name() const { return tailnet ? tailnet->peer.name : host_name; }
    bool can_pair_without_code() const { return tailnet && tailnet->can_pair_without_code(); }
    // Where to pair: the tailnet for a code-free pairing, else this network.
    std::string address() const {
        if (lan && !can_pair_without_code()) {
            return lan->where();
        }
        return tailnet ? tailnet->address() : std::string();
    }
};

struct NearbyScan {
    std::optional<net::TailscaleStatus> status;
    std::vector<NearbyHost> hosts;
    // Every address of this network was tried as well (a sweep).
    bool swept = false;
};

// Records an old pairing left on this host, which recover_device_records can hand to a paired
// computer; counts per kind ("dictation", "meeting").
struct RecordOwnerGroup {
    std::string id;
    std::string name;
    std::map<std::string, std::int64_t> counts;

    friend bool operator==(const RecordOwnerGroup&, const RecordOwnerGroup&) = default;
};

// Affinity: thread-safe. The readers marked "cheap" answer from memory; every other method may
// wait on the network, the settings file or the credential store and asserts it is not on the
// GUI thread: pages reach them through ctx.io() and come back with qt::post and a TokenGate.
class RemoteEngineControl {
public:
    virtual ~RemoteEngineControl() = default;

    // ---- host role ----
    // Asks Tailscale for its state, so off the GUI thread.
    virtual HostState host_state() = 0;
    // activity.py's snapshot: {since, transcriptions, audio_s, host_s, previews, errors,
    // last_at, serial, devices{id: {...}}, events[newest first]}; at most `events` events.
    virtual Json host_activity(std::optional<int> events = {}) = 0; // cheap
    virtual Json engine_state() = 0;                                // cheap: describe()
    virtual std::vector<ConnectedClient> connected_clients() = 0;   // cheap
    virtual void set_host_enabled(bool enabled) = 0;
    virtual void set_host_port(int port) = 0;
    // A fresh six-digit code (valid 300 s, one pairing), or nullopt while not sharing.
    virtual std::optional<std::string> open_pairing() = 0;
    virtual void cancel_pairing() = 0;
    virtual std::optional<PairRequestInfo> pending_pair_request() = 0; // cheap
    // The owner's answer; false when that request is gone.
    virtual bool answer_pair_request(std::string_view id, bool allow) = 0;
    // What Allow grants besides dictation ("manage its models", ...), for the dialog.
    virtual std::vector<std::string> pairing_grants() = 0;
    virtual bool remove_device(std::string_view id, bool delete_records) = 0;
    virtual std::optional<PairedDevice> rename_device(std::string_view id,
                                                      std::string_view name) = 0;
    virtual void set_tailscale_trust(bool trust) = 0;
    virtual void set_keep_records(bool keep) = 0;
    virtual void set_model_management(bool allow) = 0;
    virtual void set_manage_mcp(bool allow) = 0;
    virtual void grant_mcp_session(std::string_view) {
        throw std::runtime_error("Session MCP control is unavailable.");
    }
    virtual std::string local_mcp_session_token() { return {}; }
    virtual void close_mcp_control() {}
    // {"dictation": {"count", "bytes"}, "meeting": {...}} kept here for one device.
    virtual Json records_summary(std::string_view device_id) = 0;
    virtual std::vector<RecordOwnerGroup> recoverable_record_owners() = 0;
    virtual void recover_device_records(std::string_view owner, std::string_view device) = 0;
    // The models this computer can serve, from the ones ready here.
    virtual std::vector<HostModel> host_models() = 0;

    // ---- client role ----
    virtual std::optional<ClientPairing> client_pairing() = 0;
    // Pairs by code (or Tailscale); RemoteEngineError (and its kinds), py::ValueError for an
    // address parse_address refuses, CredentialStoreError when the key couldn't be saved (the
    // pairing then waits for retry_pairing_save).
    virtual ClientPairing pair(std::string_view address, std::string_view code, bool tailscale) = 0;
    // Pairs by asking the host's owner: `on_code` gets the six digits once both sides have them;
    // `cancel` stops the wait (PairingCanceled).
    virtual ClientPairing request_pairing(std::string_view address,
                                          std::function<void(std::string sas)> on_code,
                                          CancelToken cancel) = 0;
    // The host whose approved pairing waits for its key to be saved; "" when none does.
    virtual std::string pending_pairing_name() = 0; // cheap
    virtual ClientPairing retry_pairing_save() = 0;
    virtual std::optional<ClientPairing> rename_host(std::string_view name) = 0;
    virtual void forget_host() = 0;
    virtual NearbyScan scan_nearby(bool sweep) = 0;
    virtual std::optional<net::TailscaleStatus> tailscale_status(bool refresh = false) = 0;
    // A model-management or MCP request to the paired host (its reply's result); `expected`
    // refuses when the pairing changed meanwhile.
    virtual Json remote_model_request(std::string_view op, const Json& fields,
                                      const std::optional<ClientPairing>& expected = {}) = 0;
    virtual Json remote_mcp_request(std::string_view op, const Json& fields,
                                    const std::optional<ClientPairing>& expected = {}) = 0;

    // "state", "pairing", "pair_request", "devices", "clients", "activity", "paired", "engine",
    // "models", "components", "records", "mcp", "tailscale", "client", "host_renamed"; emitted
    // from whichever thread changed it.
    Signal<std::string> changed;
};

} // namespace fv::remote
