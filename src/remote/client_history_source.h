#pragma once

#include <string>
#include <string_view>
#include <vector>

#include "base/json.h"
#include "remote/errors.h"

// The host's view of its paired computers' own history (services/remote_history/channel.py's
// HistoryBroker, as the History API and the MCP tools ask it): who is reachable over the direct
// history channel, and one read query to one of them.
namespace fv::remote {

struct ClientHistoryStatus {
    std::string device_id;
    std::string name;
    // "online", "sharing_disabled" (connected, but its owner hasn't turned sharing on) or
    // "unavailable" (not connected).
    std::string status;

    friend bool operator==(const ClientHistoryStatus&, const ClientHistoryStatus&) = default;
};

// Affinity: thread-safe; query blocks for up to 5 s, never on the GUI thread.
class ClientHistorySource {
public:
    virtual ~ClientHistorySource() = default;
    // Every paired device, in the registry's order.
    virtual std::vector<ClientHistoryStatus> clients() = 0;
    // The client's answer to a history operation (a public History API result). Throws
    // ClientUnavailable with the client's sanitized code, or "unavailable" when it is not
    // connected, has 8 queries pending, or did not answer in time.
    virtual Json query(std::string_view device_id, std::string_view operation,
                       const Json& params) = 0;
};

} // namespace fv::remote
