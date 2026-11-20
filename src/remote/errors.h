#pragma once

#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>

#include "base/json.h"

// The client side's errors (services/remote_asr/client.py) and the direct-history channel's
// (services/remote_history/channel.py). Every message is a full sentence the user may see.
namespace fv::remote {

// A user-facing reason the remote engine can't be used right now.
class RemoteEngineError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

// retry_after_ms / 1000 when it is a number (never a bool) of at least 0, else nullopt; NaN is
// nullopt and infinity stays infinite, as Python's comparison and division leave them.
std::optional<double> retry_after_seconds(const Json& retry_after_ms);

// The host rejected a connection before work; a later retry is safe.
class RemoteHostBusy : public RemoteEngineError {
public:
    explicit RemoteHostBusy(const std::string& message, const Json& retry_after_ms = nullptr)
        : RemoteEngineError(message), retry_after_s(retry_after_seconds(retry_after_ms)) {}
    bool retryable() const { return true; }

    std::optional<double> retry_after_s; // seconds
};

// The host answered a request with an error; `code` names the kind, if the reply gave a string.
// Not a RemoteEngineError, as in Python (a RuntimeError).
class RemoteRequestError : public std::runtime_error {
public:
    explicit RemoteRequestError(const std::string& message, const Json& code = nullptr,
                                const Json& retry_after_ms = nullptr);
    bool retryable() const { return code == "busy"; }

    std::optional<std::string> code;
    std::optional<double> retry_after_s; // seconds
};

// The connection dropped. `sent` says whether the host got the request.
class RemoteConnectionLost : public RemoteEngineError {
public:
    RemoteConnectionLost(const std::string& message, bool sent_)
        : RemoteEngineError(message), sent(sent_) {}

    bool sent;
};

// The request was not sent or the host refused it before decoding.
class RemoteDecodeNotStarted : public RemoteEngineError {
public:
    using RemoteEngineError::RemoteEngineError;
};

// Nothing accepted a connection at the address. `refused` when a computer there turned it away
// (nothing listening on the port), as opposed to no answer at all (a firewall, another network,
// or nothing at that address).
class RemoteUnreachable : public RemoteEngineError {
public:
    explicit RemoteUnreachable(const std::string& message, bool refused_ = false)
        : RemoteEngineError(message), refused(refused_) {}

    bool refused;
};

// This computer's pairing has no token to present (the credential store lost it): no retry can
// succeed until it pairs again, so the remote engine's link stops retrying (restore_link's
// "unpaired" in OpenWhisper, which checks load_client_token before connecting).
class RemotePairingIncomplete : public RemoteEngineError {
public:
    using RemoteEngineError::RemoteEngineError;
};

// The person stopped waiting for the host to allow pairing ("Pairing canceled.").
class PairingCanceled : public RemoteEngineError {
public:
    using RemoteEngineError::RemoteEngineError;
};

// A paired client's history could not be read. The code is one of "unavailable",
// "sharing_disabled", "not_found", "invalid_query", "snapshot_unavailable" and "unsupported";
// anything else (or a code that is not a string) becomes "unavailable". what() is the code's
// sentence.
class ClientUnavailable : public std::runtime_error {
public:
    explicit ClientUnavailable(std::string_view code = "unavailable");
    explicit ClientUnavailable(const char* code) : ClientUnavailable(std::string_view(code)) {}
    explicit ClientUnavailable(const std::string& code)
        : ClientUnavailable(std::string_view(code)) {}
    explicit ClientUnavailable(const Json& code);
    const std::string& code() const { return code_; }

private:
    std::string code_;
};

// The code ClientUnavailable keeps for `code`, and its sentence.
std::string client_unavailable_code(std::string_view code);
std::string_view client_unavailable_message(std::string_view code);

} // namespace fv::remote
