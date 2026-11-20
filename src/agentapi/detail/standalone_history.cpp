#include "agentapi/detail/standalone_history.h"

#include <utility>

#include "agentapi/history_models.h"
#include "agentapi/history_store.h"
#include "base/hash.h"
#include "base/ids.h"
#include "pyport/json.h"

namespace fv::agentapi::detail {

namespace {

constexpr auto kCursorTtl = std::chrono::seconds(600);
constexpr std::size_t kMaxCursors = 128;

std::string message_for(std::string_view code) {
    if (code == "sharing_disabled") {
        return "History sharing is disabled on this client.";
    }
    if (code == "not_found") {
        return "Record not found on this client.";
    }
    if (code == "invalid_query") {
        return "Invalid client history query.";
    }
    if (code == "snapshot_unavailable") {
        return "This client's saved insights are unavailable.";
    }
    if (code == "unsupported") {
        return "Direct client queries are not available on this server.";
    }
    return "Client history is unavailable. The client may be offline.";
}

std::string known_code(std::string_view code) {
    for (const std::string_view known : {"unavailable", "sharing_disabled", "not_found",
                                         "invalid_query", "snapshot_unavailable", "unsupported"}) {
        if (code == known) {
            return std::string(code);
        }
    }
    return "unavailable";
}

} // namespace

ClientUnavailable::ClientUnavailable(std::string_view code)
    : std::runtime_error(message_for(known_code(code))), code_(known_code(code)) {}

StandaloneHistory::StandaloneHistory(const Clock& clock) : clock_(clock) {}

Json StandaloneHistory::page(std::string_view operation, const Json& params, bool include_clients,
                             const std::optional<std::string>& device_id, bool include_remote,
                             const std::optional<std::string>& cursor, std::int64_t limit,
                             const LocalRead& read) {
    if (!include_clients && !device_id) {
        return read(cursor, limit, std::nullopt);
    }
    if (include_clients) {
        throw ClientUnavailable("unsupported");
    }
    py::DumpOptions sorted;
    sorted.sort_keys = true;
    const std::string scope = sha256_hex(
        py::dumps(Json::array({std::string(operation), params, *device_id, limit}), sorted));
    if (limit < 1 || limit > 100) {
        throw InvalidQuery("limit must be between 1 and 100.");
    }
    // No client is paired, so the device is known only through its stored copies.
    if (!include_remote) {
        throw ClientUnavailable();
    }
    const ClientStatus status{*device_id, "Paired computer", "unavailable"};

    Source local;
    std::set<Identity> seen;
    if (cursor && !cursor->empty()) {
        std::lock_guard lock(mutex_);
        const auto it = cursors_.find(*cursor);
        if (it == cursors_.end() || it->second.expires < clock_.steady() ||
            it->second.scope != scope) {
            throw InvalidQuery("Client history cursor expired or used with different filters. "
                               "Restart the query.");
        }
        local = it->second.local;
        seen = it->second.seen;
    }

    const auto fetch = [&] {
        if (!local.items.empty() || (local.started && !local.cursor)) {
            return;
        }
        const Json result = read(local.cursor, limit, device_id);
        local.items.assign(result.at("items").begin(), result.at("items").end());
        const Json& next = result.at("next_cursor");
        local.cursor =
            next.is_string() ? std::optional<std::string>(next.get<std::string>()) : std::nullopt;
        local.started = true;
    };
    fetch();

    Json items = Json::array();
    while (static_cast<std::int64_t>(items.size()) < limit && !local.items.empty()) {
        Json item = std::move(local.items.front());
        local.items.pop_front();
        const Json& origin = item.at("origin_device_id");
        Identity identity{origin.is_string() ? std::optional<std::string>(origin.get<std::string>())
                                             : std::nullopt,
                          item.contains("kind") ? item.at("kind").get<std::string>() : "",
                          item.at("id").get<std::string>()};
        // Copies saved by Both and their live originals give one citation.
        if (local.items.empty() && local.cursor) {
            fetch();
        }
        if (seen.insert(identity).second) {
            items.push_back(std::move(item));
        }
    }

    std::optional<std::string> next_cursor;
    if (!local.items.empty() || local.cursor) {
        next_cursor = "clients-" + uuid4_hex();
        std::lock_guard lock(mutex_);
        const auto now = clock_.steady();
        for (auto it = cursors_.begin(); it != cursors_.end();) {
            it = it->second.expires < now ? cursors_.erase(it) : std::next(it);
        }
        while (cursors_.size() >= kMaxCursors) {
            auto oldest = cursors_.begin();
            for (auto it = cursors_.begin(); it != cursors_.end(); ++it) {
                if (it->second.order < oldest->second.order) {
                    oldest = it;
                }
            }
            cursors_.erase(oldest);
        }
        cursors_[*next_cursor] = Saved{now + kCursorTtl, scope, local, seen, next_order_++};
    }
    return page_json(std::move(items), next_cursor, {status});
}

} // namespace fv::agentapi::detail
