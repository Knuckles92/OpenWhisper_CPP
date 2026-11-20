#pragma once

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include "agentapi/mcp_events.h"
#include "base/clock.h"
#include "base/json.h"

// MCP Events' saved subscriptions (events_ext.py EventSubscription, _Status, _Registry): one file
// of subscriptions without secrets, and each secret in the credential store.
namespace fv {
class CredentialStore;
}

namespace fv::agentapi::events_ext {

// Affinity: value type.
struct EventSubscription {
    std::string id;
    std::string name;
    Json arguments = Json::object();
    std::string url;
    // Unix seconds; delivery stops at this moment unless refreshed.
    std::int64_t refresh_before = 0;
    std::string created_at;
    // ORIGIN_LOCAL (an agent on this computer) or ORIGIN_TAILSCALE (a peer, delivered to only
    // while the user allows agents over Tailscale): who made or last refreshed it.
    std::string origin{ORIGIN_TAILSCALE};

    // `now` in Unix seconds.
    bool expired(double now) const { return now >= static_cast<double>(refresh_before); }
    // {id, name, arguments, url, refresh_before (ISO), created_at, origin}
    Json to_record() const;
    // A saved record, or nullopt when it is not one; an unknown origin is the more restricted
    // ORIGIN_TAILSCALE.
    static std::optional<EventSubscription> from_record(const Json& record);

    friend bool operator==(const EventSubscription&, const EventSubscription&) = default;
};

// Subscriptions saved in one file, with their secrets in the credential store. Two locks, so slow
// storage never holds anyone up: the memory lock guards the state and is never held across I/O;
// the I/O lock orders the credential-store and file writes of each change, so the file always
// ends up holding the newest state. Readers (the bus's dispatch thread matching, the GUI thread
// counting) take neither: every change publishes an immutable snapshot.
// Affinity: thread-safe.
class Registry {
public:
    // Writes the file's bytes; throws std::system_error (OSError) when it cannot.
    using Writer = std::function<void(const std::filesystem::path& path, const std::string& bytes)>;

    // Reads `path` at once; a file that cannot be read or parsed is logged by error type only.
    Registry(std::filesystem::path path, CredentialStore& credentials, const Clock& clock,
             std::size_t max_subscriptions = MAX_SUBSCRIPTIONS);
    // Waits for a prune running on its own thread.
    ~Registry();
    Registry(const Registry&) = delete;
    Registry& operator=(const Registry&) = delete;

    const std::filesystem::path& path() const { return path_; }

    // Never blocks: an unexpired subscription, or the unexpired ones in saved order.
    std::optional<EventSubscription> get(std::string_view id) const;
    std::vector<EventSubscription> active() const;
    std::vector<EventSubscription> matching(std::string_view event_type, const Json& data) const;

    // Saves a new or refreshed subscription; true when it is new. Throws EventsError when a new
    // one would pass the cap, CredentialStoreError when the secret cannot be saved, and
    // std::system_error when the file cannot be written, with everything as it was, secret and
    // rotation included: the agent hears the refresh failed and keeps checking signatures with
    // the secret it had.
    bool upsert(const EventSubscription& subscription, const std::string& secret);
    // Forgets a subscription and its secret; false when there was none.
    bool remove(std::string_view id);
    // Forgets every subscription and its secret; how many were active.
    std::size_t clear();
    // Forgets expired subscriptions and their secrets; a failed save is logged and tried again
    // by the next prune.
    void prune();
    // prune() on a thread of its own when something has expired; returns at once.
    void prune_later();

    // The current secret, then the previous one while the rotation window lasts.
    std::vector<std::string> signing_secrets(std::string_view id);
    // A delivery's outcome: "" for success, else its fixed category.
    void record(std::string_view id, std::string_view error = {});
    // {active, lastDeliveryAt?, lastError?, failedSince?}
    Json delivery_status(std::string_view id, bool active) const;

    // Replaces the file writer (tests make it fail).
    void set_writer(Writer writer);

private:
    using Subscriptions = std::vector<EventSubscription>;
    struct Status {
        std::optional<std::string> last_delivery_at;
        std::optional<std::string> last_error;
        std::optional<std::string> failed_since;
    };

    double now() const;
    std::shared_ptr<const Subscriptions> view() const;
    // With mutex_ held.
    EventSubscription* find(std::string_view id);
    void commit();
    void drop(std::string_view id);
    std::string payload() const;
    // With io_mutex_ held, never mutex_.
    void write(const std::string& bytes);
    void delete_secret(std::string_view id);
    void restore_secret(std::string_view id, const std::optional<std::string>& value);
    void load();

    std::filesystem::path path_;
    CredentialStore& credentials_;
    const Clock& clock_;
    std::size_t max_subscriptions_;
    Writer writer_;

    mutable std::mutex mutex_;
    std::mutex io_mutex_;
    Subscriptions subs_;
    std::atomic<std::shared_ptr<const Subscriptions>> view_;
    std::map<std::string, Status, std::less<>> status_;
    // id -> (old secret, steady deadline) after a refresh rotated it.
    std::map<std::string, std::pair<std::string, std::chrono::steady_clock::time_point>,
             std::less<>>
        previous_;
    bool pruning_ = false;

    std::mutex thread_mutex_;
    std::thread prune_thread_;
};

} // namespace fv::agentapi::events_ext
