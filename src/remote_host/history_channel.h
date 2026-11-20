#pragma once

#include <memory>
#include <optional>
#include <string>
#include <string_view>

#include "net/types.h"
#include "remote/control.h"

// The seam the host hands a paired computer's direct-history connection to (`hello` with
// `purpose: "history"`): services/remote_history/channel.py's HistoryBroker as SpeechHost calls
// it. The broker sends `ready` and its `history_query` frames over the connection and reads the
// client's `history_result` frames.
namespace fv::remote_host {

// Affinity: thread-safe; the host calls it from its connections' strands.
class HistoryChannel {
public:
    virtual ~HistoryChannel() = default;
    // Starts accepting history connections (sharing started).
    virtual void start() = 0;
    // Takes over a history connection after hello: `device` as the registry authenticated it,
    // `token` to authenticate it again for every frame, `enabled` the client's history_enabled.
    // Frames then arrive through on_text and the end through on_close.
    virtual void serve(std::shared_ptr<net::WsConnection> connection,
                       const remote::PairedDevice& device, std::string token, bool enabled) = 0;
    virtual void on_text(const std::shared_ptr<net::WsConnection>& connection,
                         std::string_view text) = 0;
    virtual void on_close(const std::shared_ptr<net::WsConnection>& connection) = 0;
    // Closes the history connections of one device; nullopt closes every one and stops
    // accepting new ones (sharing stopped).
    virtual void remove(std::optional<std::string> device_id) = 0;
};

} // namespace fv::remote_host
