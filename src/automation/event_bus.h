#pragma once

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include "base/clock.h"
#include "base/events.h"
#include "base/json.h"

// The in-process event bus behind the live API, webhooks, programs and MCP Events
// (services/automation/events.py). Publishers on any thread only append to a buffer; one
// dispatch thread ("automation-events") hands each event to the subscribers, whose callbacks
// must only enqueue. So a slow webhook or a stalled stream never delays a paste, and no
// subscriber runs on the GUI thread. Events carry dictated text: they are kept in memory only
// (a short replay buffer for reconnecting streams) and are never logged.
namespace fv::automation {

// The `v` every event carries; bumped only for incompatible payload changes.
inline constexpr int SCHEMA_VERSION = 1;
inline constexpr std::size_t DEFAULT_BUFFER_SIZE = 500;
// A callback slower than this is reported once per subscription, by its label only.
inline constexpr std::chrono::milliseconds SLOW_CALLBACK{1000};

// Affinity: value type.
struct Event {
    std::string id; // "evt_" and 24 lowercase hex digits
    std::int64_t seq = 0;
    std::string type;
    std::string time; // UTC, "2026-11-16T09:30:00.123Z"
    Json data = Json::object();

    // {id, seq, type, time, v, data}, in that order.
    Json to_dict() const;
    // Compact JSON; non-ASCII kept, or as \u escapes with ensure_ascii.
    std::string to_json(bool ensure_ascii = false) const;

    friend bool operator==(const Event&, const Event&) = default;
};

// What since() found: the events, and why some may be missing (catalog::GAP_EVICTED,
// catalog::GAP_RESTARTED), or nullopt when the client cannot have missed any.
struct SinceResult {
    std::vector<Event> events;
    std::optional<std::string> gap;

    friend bool operator==(const SinceResult&, const SinceResult&) = default;
};

namespace detail {
struct BusState;
}

// One subscriber; it gets exactly the events published while it is open. close() (idempotent)
// ends that: nothing published afterwards reaches it, though an event published before may
// still be on its way. So replacing a subscription by opening the new one before closing the
// old loses nothing; an event both got is told apart by its seq. It may outlive its bus.
// Affinity: thread-safe.
class EventSubscription {
public:
    EventSubscription(std::weak_ptr<detail::BusState> bus,
                      std::function<void(const Event&)> callback,
                      std::optional<std::set<std::string, std::less<>>> types, std::string label);
    EventSubscription(const EventSubscription&) = delete;
    EventSubscription& operator=(const EventSubscription&) = delete;

    bool closed() const { return closed_.load(); }
    void close();
    const std::string& label() const { return label_; }
    // Whether it follows the event's type (all types without a filter).
    bool wants(const Event& event) const;

private:
    friend class EventBus;
    friend struct detail::BusState;

    std::weak_ptr<detail::BusState> bus_;
    std::function<void(const Event&)> callback_;
    std::optional<std::set<std::string, std::less<>>> types_;
    std::string label_;
    std::atomic<bool> closed_{false};
    bool warned_slow_ = false; // dispatch thread only
};

// Thread-safe publish and subscribe with a bounded replay buffer. Numbering continues after
// `seq_base`: the app's bus starts each run above the last one's (run_seq_base), so a client's
// position from an earlier run is always behind this run's first event and never matches a
// recent one. Event times come from `clock`, which must outlive the bus.
// Affinity: thread-safe.
class EventBus final : public EventSink {
public:
    explicit EventBus(std::size_t buffer_size = DEFAULT_BUFFER_SIZE, std::int64_t seq_base = 0,
                      const Clock& clock = system_clock());
    ~EventBus() override;
    EventBus(const EventBus&) = delete;
    EventBus& operator=(const EventBus&) = delete;

    // Records and queues an event; never blocks on subscribers. replay = false keeps frequent,
    // short-lived events (live preview text) out of since() and recent(). Throws py::TypeError
    // for data that is not a JSON object (falsy data, such as null or [], counts as {}) or does
    // not encode as UTF-8 (a lone surrogate, as from half an emoji), before numbering it.
    Event publish_event(std::string_view type, Json data, bool replay = true);
    // EventSink: publish_event that never throws; a refused event is dropped with a warning that
    // names only its type.
    void publish(std::string_view type, Json data, bool replay = true) override;

    // Calls `callback(event)` on the dispatch thread for each event published after this call
    // (never one published before that the dispatcher has not handed out yet). `types` limits
    // it to those types (empty strings ignored; an empty list follows none); nullopt follows
    // all. `label` lets subscribers(label) count a kind of listener. On a closed bus the
    // subscription comes back closed.
    std::shared_ptr<EventSubscription>
    subscribe(std::function<void(const Event&)> callback,
              std::optional<std::vector<std::string>> types = std::nullopt, std::string label = {});
    // Open subscriptions, or those with `label` when it is not empty.
    std::size_t subscribers(std::string_view label = {}) const;

    // Buffered events after `seq` of the wanted types, and why some may be missing. Only events
    // of the wanted types count: a client is never told of a gap in events it does not follow.
    // seq 0 asks for everything from this run's start; a position this run never gave out (an
    // earlier run's) gets every event of this run and GAP_RESTARTED, unless this run dropped one
    // of the wanted types (GAP_EVICTED).
    SinceResult since(std::int64_t seq,
                      const std::optional<std::vector<std::string>>& types = std::nullopt) const;
    // The newest `limit` buffered events of the wanted types, oldest first; none for limit <= 0.
    std::vector<Event>
    recent(std::int64_t limit = 50,
           const std::optional<std::vector<std::string>>& types = std::nullopt) const;
    std::int64_t last_seq() const;
    // Forgets buffered events (history was deleted); numbering continues. They count as
    // dropped, like evicted ones: a client that had not got one of a type it follows is told of
    // the gap, never its text.
    void clear();
    // Ends delivery: open subscriptions close, queued deliveries are dropped, and the dispatch
    // thread is waited for up to 2 s (not when called from it). Publishing still numbers and
    // buffers events.
    void close();

private:
    std::shared_ptr<detail::BusState> state_;
};

// Where this run's numbering starts: milliseconds since 1970 on `clock`. It only grows from one
// app run to the next; thirteen digits, safe as a JavaScript number.
std::int64_t run_seq_base(const Clock& clock = system_clock());

// The process-wide bus, created on first use numbering from run_seq_base(run_clock()).
EventBus& bus();
std::shared_ptr<EventBus> shared_bus();
// Swaps the process-wide bus (tests, startup); returns the previous one, which the caller
// closes. Null lets the next bus() create a fresh one.
std::shared_ptr<EventBus> set_bus(std::shared_ptr<EventBus> replacement);
// Forgets the process-wide bus's replay buffer, if there is a bus; never throws. Called when
// history entries or meetings are deleted, so their text cannot be replayed to a stream that
// reconnects.
void clear_bus();
// Publishes on the process-wide bus; never throws: a refused event is logged by type and
// dropped (nullopt).
std::optional<Event> publish(std::string_view type, Json data, bool replay = true);
// The clock a bus created by bus() numbers from and stamps with; system_clock() unless a test
// set one (null restores it). The clock must outlive every bus created with it.
void set_run_clock(const Clock* clock);
const Clock& run_clock();

} // namespace fv::automation
