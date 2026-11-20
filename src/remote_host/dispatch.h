#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <set>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "base/json.h"
#include "remote/control.h"
#include "remote_host/activity.h"
#include "remote_host/host_engine.h"
#include "remote_host/speech_host.h"

// What a paired computer's requests get once the host has admitted them
// (services/remote_asr/host.py _header_language, _transcribe, _stream, _dispatch_unpacked,
// _management_request, _decode_request, _manage_model_request, _mcp_request, _records_request,
// _configure_runtime_request, _switch_model), and the messages built from the host's callbacks
// (_host_info, _model_list, the permission readers, _ready_message, _probe, _discovery_info).
// Field allow-lists are exact sets: an extra field refuses the request.
namespace fv::remote_host {

// What a records callback throws for the replies Python gives services/remote_records/gate.py's
// RecordsBusy ({code: busy, retry_after_ms: 1000}) and a LookupError ({code: not_found}, "No
// such record." when it says nothing). py::ValueError is {code: bad_request}.
class RecordsBusy : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};
class RecordNotFound : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

// The status and error of a host whose engine has not settled after a load.
inline constexpr std::string_view ENGINE_LOADING_MESSAGE = "The host is loading its speech engine.";
// "The host is preparing a <product> update. Try again shortly." (the update hold).
std::string host_update_message();
// The busy refusal's text, for a connection over the limits and a request the work pool can't
// take.
inline constexpr std::string_view BUSY_MESSAGE = "The host is busy. Try again shortly.";

// transcribe, stream, select_model, configure_runtime, model_catalog, download_model and
// install_runtime: the operations that pass the work admission.
bool is_costly(const Json& op);

// header["language"] when it is a string.
std::optional<std::string> header_language(const Json& header);
// One window: the engine's {"text", "segments"}, with the header's vocabulary hints only when
// the engine takes them.
Json transcribe_request(HostEngine& engine, const Json& header, std::span<const float> audio);
// A stream or cancel_stream request's result. Sessions are scoped to the connection
// ("remote-<connection id>-<session>") and kept in `streams` until finished or cancelled, so a
// departed client's can be cancelled. std::invalid_argument "A stream needs a session id".
Json stream_request(HostEngine& engine, const Json& header, std::span<const float> audio,
                    std::string_view connection_id, std::set<std::string>& streams);

// Affinity: thread-safe as its callbacks are; called on the host's pool threads.
class Dispatcher {
public:
    using Emit = std::function<void(std::string kind, Json detail)>;

    // `deps` and `activity` outlive it.
    Dispatcher(const SpeechHostDeps& deps, HostActivity& activity, Emit emit);

    // The engine to serve now; never null ("The host's engine is unavailable" when the provider
    // throws, "No engine is selected on the host" when it gives none).
    std::shared_ptr<HostEngine> engine() const;

    // {name, fingerprint, addresses}.
    Json host_info() const;
    // The models a client may switch this host to; [] when the callback fails.
    Json model_list() const;
    bool can_manage_models() const;
    bool keeps_records() const;
    bool controls_mcp() const;
    // The device's stored-records summary; {} when the callback fails or gives no object.
    Json stored_summary(std::string_view device_id) const;
    std::string tailscale_pairing_owner() const;
    bool engine_settled() const;
    Json runtime() const;

    // The "ready" reply to a paired computer's hello; `maintenance` while an update holds the
    // host.
    Json ready_message(const remote::PairedDevice& device, HostEngine& engine,
                       bool maintenance) const;
    // Who this is and what it serves: enough for a discovery list, no secrets.
    Json probe_message() const;
    // What a discovery answer says: the probe's subset, plus where to connect.
    Json discovery_info(std::optional<std::uint16_t> port) const;

    // A request after the frame was unpacked and admitted; `identity` is the engine the
    // connection was told about in ready, `set_busy` marks the connection as decoding.
    struct Request {
        const Json& header;
        std::span<const std::byte> payload;
        const std::vector<std::string>& identity;
        std::string_view connection_id;
        std::set<std::string>& streams;
        std::string_view device_name;
        std::string_view device_id;
        std::function<void(bool)> set_busy;
    };
    // The reply, without host_ms. py::TypeError for an op Python cannot look up in a set (a list
    // or an object): the connection then drops.
    Json dispatch_unpacked(const Request& request) const;

    Json manage_model_request(const Json& id, const Json& header,
                              std::string_view device_name) const;
    Json mcp_request(const Json& id, const Request& request) const;
    Json records_request(const Json& id, const Json& header, std::span<const std::byte> payload,
                         const remote::PairedDevice& device) const;
    Json configure_runtime_request(const Json& id, const Json& header,
                                   std::string_view device_name) const;
    Json switch_model(const Json& id, const Json& header, std::string_view device_name) const;

private:
    Json management_request(const Json& id, const Json& header, std::string_view device_name) const;
    Json decode_request(const Request& request, std::span<const float> audio,
                        HostEngine& current) const;
    void emit(std::string kind, Json detail) const;

    const SpeechHostDeps& deps_;
    HostActivity& activity_;
    Emit emit_;
};

} // namespace fv::remote_host
