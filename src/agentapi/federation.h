#pragma once

#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <tuple>

#include "agentapi/history_models.h"
#include "base/clock.h"
#include "remote/client_history_source.h"

namespace fv::agentapi {

// Authenticated read-only federation. Callers opt into peer history; saved cursors retain
// the same source set, and queued rows are discarded when a peer revokes sharing.
class FederatedHistory {
public:
    using LocalRead =
        std::function<Json(const std::optional<std::string>& cursor, std::int64_t limit,
                           const std::optional<std::string>& origin)>;

    FederatedHistory(std::shared_ptr<remote::ClientHistorySource> clients, const Clock& clock);
    std::vector<ClientStatus> clients();
    Json query(std::string_view device_id, std::string_view op, const Json& params);
    Json page(std::string_view operation, const Json& params,
              const std::optional<std::string>& device_id, bool include_remote,
              const std::optional<std::string>& cursor, std::int64_t limit, const LocalRead& read);

private:
    using Identity = std::tuple<std::string, std::string, std::string>;
    struct Source {
        std::optional<std::string> device;
        std::optional<std::string> cursor;
        std::deque<Json> items;
        bool exhausted = false;
    };
    struct Page {
        std::chrono::steady_clock::time_point expires;
        std::string scope;
        std::vector<Source> sources;
        std::set<Identity> seen;
        std::uint64_t sequence = 0;
    };

    std::string save(Page page);

    std::shared_ptr<remote::ClientHistorySource> clients_;
    const Clock& clock_;
    std::mutex mutex_;
    std::map<std::string, Page, std::less<>> cursors_;
    std::uint64_t sequence_ = 0;
};

} // namespace fv::agentapi
