#pragma once

#include <chrono>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <tuple>
#include <vector>

#include "base/clock.h"
#include "base/json.h"

// services/agent_api/federation.py's FederatedHistory as the standalone History API runs it:
// with no paired clients. Requests that name clients get its answers: include_clients is
// "unsupported", a device_id without include_remote is "unavailable", and a device_id with
// include_remote reads this computer's stored copies of that device's records, paged through
// cursors the server keeps ("clients-<hex>", ten minutes, at most 128). Federation with live
// clients (M9) grows this into the full merge.
namespace fv::agentapi::detail {

// services/remote_history/channel.py ClientUnavailable: 404 for not_found, else 503.
class ClientUnavailable : public std::runtime_error {
public:
    explicit ClientUnavailable(std::string_view code = "unavailable");
    const std::string& code() const noexcept { return code_; }
    int status() const noexcept { return code_ == "not_found" ? 404 : 503; }

private:
    std::string code_;
};

// Affinity: thread-safe.
class StandaloneHistory {
public:
    // One page of this computer's history: (cursor, limit, origin_device_id) -> Page JSON.
    using LocalRead =
        std::function<Json(const std::optional<std::string>& cursor, std::int64_t limit,
                           const std::optional<std::string>& origin_device_id)>;

    explicit StandaloneHistory(const Clock& clock);

    // FederatedHistory._page. `params` are the filters that bind a client cursor (q, kind,
    // include_remote, since, before).
    Json page(std::string_view operation, const Json& params, bool include_clients,
              const std::optional<std::string>& device_id, bool include_remote,
              const std::optional<std::string>& cursor, std::int64_t limit, const LocalRead& read);

private:
    using Identity = std::tuple<std::optional<std::string>, std::string, std::string>;
    struct Source {
        std::deque<Json> items;
        std::optional<std::string> cursor;
        bool started = false;
    };
    struct Saved {
        std::chrono::steady_clock::time_point expires;
        std::string scope;
        Source local;
        std::set<Identity> seen;
        std::uint64_t order = 0; // insertion order: the oldest goes first when full
    };

    const Clock& clock_;
    std::mutex mutex_;
    std::map<std::string, Saved> cursors_;
    std::uint64_t next_order_ = 0;
};

} // namespace fv::agentapi::detail
