#include "automation/event_bus.h"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <format>
#include <map>
#include <mutex>
#include <thread>
#include <utility>

#include "automation/catalog.h"
#include "automation/detail/pyvalue.h"
#include "automation/detail/thread_name.h"
#include "base/ids.h"
#include "base/log.h"
#include "pyport/errors.h"
#include "pyport/json.h"

namespace fv::automation {

namespace {

constexpr log::Logger kLog{"services.automation.events"};
// How long close() waits for the dispatch thread before leaving it to finish on its own.
constexpr std::chrono::seconds kJoinTimeout{2};

using TypeSet = std::set<std::string, std::less<>>;

std::optional<TypeSet> type_set(const std::optional<std::vector<std::string>>& types) {
    if (!types) {
        return std::nullopt;
    }
    TypeSet set;
    for (const std::string& type : *types) {
        if (!type.empty()) {
            set.insert(type);
        }
    }
    return set;
}

bool wanted(const std::optional<TypeSet>& types, std::string_view type) {
    return !types || types->contains(type);
}

// _now_iso: UTC with milliseconds and "Z".
std::string now_iso(const Clock& clock) {
    const auto now = std::chrono::floor<std::chrono::milliseconds>(clock.utc());
    const auto seconds = std::chrono::floor<std::chrono::seconds>(now);
    const auto millis = (now - seconds).count();
    return std::format("{:%Y-%m-%dT%H:%M:%S}.{:03d}Z", seconds, millis);
}

std::string label_or_unnamed(const std::string& label) {
    return label.empty() ? std::string("(unnamed)") : label;
}

} // namespace

namespace detail {

struct BusState {
    using Recipients = std::vector<std::shared_ptr<EventSubscription>>;

    BusState(std::size_t capacity, std::int64_t seq_base, const Clock& clock)
        : capacity(std::max<std::size_t>(1, capacity)), base(std::max<std::int64_t>(0, seq_base)),
          next_seq(base), last_seq(base), clock(&clock) {}

    void remove(const EventSubscription* subscription) {
        std::lock_guard lock(mutex);
        std::erase_if(subscriptions, [&](const std::shared_ptr<EventSubscription>& each) {
            return each.get() == subscription;
        });
    }

    // Caller holds the mutex.
    void ensure_thread(const std::shared_ptr<BusState>& self) {
        if (thread.joinable()) {
            return;
        }
        thread_done = false;
        thread = std::thread([self] {
            automation::detail::set_current_thread_name("automation-events");
            self->run();
        });
        thread_id = thread.get_id();
    }

    void run() {
        while (true) {
            std::pair<Event, Recipients> next;
            {
                std::unique_lock lock(mutex);
                wake.wait(lock, [&] { return !pending.empty() || closed; });
                if (closed) {
                    break;
                }
                next = std::move(pending.front());
                pending.pop_front();
            }
            deliver(next.first, next.second);
        }
        std::lock_guard lock(mutex);
        thread_done = true;
        done.notify_all();
    }

    void deliver(const Event& event, const Recipients& recipients) {
        for (const auto& subscription : recipients) {
            const auto started = std::chrono::steady_clock::now();
            try {
                subscription->callback_(event);
            } catch (const std::exception& error) {
                kLog.warning("Event subscriber {} failed: {}",
                             label_or_unnamed(subscription->label_),
                             automation::detail::error_type(error));
            } catch (...) {
                kLog.warning("Event subscriber {} failed: {}",
                             label_or_unnamed(subscription->label_), "unknown");
            }
            const std::chrono::duration<double> elapsed =
                std::chrono::steady_clock::now() - started;
            if (elapsed > SLOW_CALLBACK && !subscription->warned_slow_) {
                subscription->warned_slow_ = true;
                kLog.warning("Event subscriber {} took {:.1f} s; callbacks must only enqueue",
                             label_or_unnamed(subscription->label_), elapsed.count());
            }
        }
    }

    mutable std::mutex mutex;
    std::condition_variable wake;
    std::condition_variable done;
    const std::size_t capacity;
    std::deque<Event> buffer;
    // Each event with the subscriptions open when it was published.
    std::deque<std::pair<Event, Recipients>> pending;
    Recipients subscriptions;
    const std::int64_t base;
    std::int64_t next_seq;
    std::int64_t last_seq;
    // Per type, the newest event no longer in the buffer (evicted or cleared); a type this run
    // has dropped none of is absent.
    std::map<std::string, std::int64_t, std::less<>> dropped;
    std::thread thread;
    std::thread::id thread_id;
    bool thread_done = true;
    bool closed = false;
    const Clock* clock;
};

} // namespace detail

Json Event::to_dict() const {
    Json result = Json::object();
    result["id"] = id;
    result["seq"] = seq;
    result["type"] = type;
    result["time"] = time;
    result["v"] = SCHEMA_VERSION;
    result["data"] = data.is_object() ? data : Json::object();
    return result;
}

std::string Event::to_json(bool ensure_ascii) const {
    return py::dumps(to_dict(), {.ensure_ascii = ensure_ascii, .item_sep = ",", .key_sep = ":"});
}

EventSubscription::EventSubscription(std::weak_ptr<detail::BusState> bus,
                                     std::function<void(const Event&)> callback,
                                     std::optional<std::set<std::string, std::less<>>> types,
                                     std::string label)
    : bus_(std::move(bus)), callback_(std::move(callback)), types_(std::move(types)),
      label_(std::move(label)) {}

void EventSubscription::close() {
    if (closed_.exchange(true)) {
        return;
    }
    if (const auto bus = bus_.lock()) {
        bus->remove(this);
    }
}

bool EventSubscription::wants(const Event& event) const {
    return wanted(types_, event.type);
}

EventBus::EventBus(std::size_t buffer_size, std::int64_t seq_base, const Clock& clock)
    : state_(std::make_shared<detail::BusState>(buffer_size, seq_base, clock)) {}

EventBus::~EventBus() {
    close();
}

Event EventBus::publish_event(std::string_view type, Json data, bool replay) {
    // dict(data or {}): null, false, 0, "" and [] read as no data.
    Json payload = automation::detail::truthy(data) ? std::move(data) : Json::object();
    if (!payload.is_object()) {
        throw py::TypeError("event data must be a mapping");
    }
    // Fail here, at the publisher, rather than in every subscriber: the streams, webhooks and
    // programs all send it as UTF-8 JSON.
    if (!automation::detail::encodes_as_utf8(payload)) {
        throw py::TypeError("event data must encode as UTF-8");
    }
    auto& state = *state_;
    std::lock_guard lock(state.mutex);
    Event event{"evt_" + random_hex(24), ++state.next_seq, std::string(type), now_iso(*state.clock),
                std::move(payload)};
    state.last_seq = event.seq;
    if (replay) {
        if (state.buffer.size() == state.capacity) {
            const Event& oldest = state.buffer.front();
            state.dropped[oldest.type] = oldest.seq;
            state.buffer.pop_front();
        }
        state.buffer.push_back(event);
    }
    // The recipients are fixed now: a subscription opened later never gets this event, one
    // closed later still does.
    detail::BusState::Recipients recipients;
    if (!state.closed) {
        for (const auto& subscription : state.subscriptions) {
            if (subscription->wants(event)) {
                recipients.push_back(subscription);
            }
        }
    }
    if (!recipients.empty()) {
        state.pending.emplace_back(event, std::move(recipients));
        state.ensure_thread(state_);
        state.wake.notify_all();
    }
    return event;
}

void EventBus::publish(std::string_view type, Json data, bool replay) {
    try {
        publish_event(type, std::move(data), replay);
    } catch (const std::exception& error) {
        kLog.warning("Could not publish {}: {}", type, automation::detail::error_type(error));
    }
}

std::shared_ptr<EventSubscription>
EventBus::subscribe(std::function<void(const Event&)> callback,
                    std::optional<std::vector<std::string>> types, std::string label) {
    auto subscription = std::make_shared<EventSubscription>(state_, std::move(callback),
                                                            type_set(types), std::move(label));
    std::lock_guard lock(state_->mutex);
    if (state_->closed) {
        subscription->closed_ = true;
        return subscription;
    }
    state_->subscriptions.push_back(subscription);
    return subscription;
}

std::size_t EventBus::subscribers(std::string_view label) const {
    std::lock_guard lock(state_->mutex);
    return static_cast<std::size_t>(
        std::ranges::count_if(state_->subscriptions, [&](const auto& subscription) {
            return !subscription->closed() && (label.empty() || subscription->label() == label);
        }));
}

SinceResult EventBus::since(std::int64_t seq,
                            const std::optional<std::vector<std::string>>& types) const {
    const auto filter = type_set(types);
    std::vector<Event> buffered;
    std::int64_t last = 0;
    std::int64_t dropped = 0;
    {
        std::lock_guard lock(state_->mutex);
        buffered.assign(state_->buffer.begin(), state_->buffer.end());
        last = state_->last_seq;
        for (const auto& [kind, newest] : state_->dropped) {
            if (wanted(filter, kind)) {
                dropped = std::max(dropped, newest);
            }
        }
    }
    std::optional<std::string> gap;
    if (0 < seq && state_->base <= seq && seq <= last) {
        if (seq < dropped) {
            gap = std::string(catalog::GAP_EVICTED);
        }
    } else {
        // 0, or a position this run never gave out (an earlier run's, or a later one's after
        // the clock was set back): every event of this run is new to the client, and any this
        // run dropped is lost.
        if (dropped != 0) {
            gap = std::string(catalog::GAP_EVICTED);
        } else if (seq != 0) {
            gap = std::string(catalog::GAP_RESTARTED);
        }
        seq = 0;
    }
    SinceResult result;
    for (Event& event : buffered) {
        if (event.seq > seq && wanted(filter, event.type)) {
            result.events.push_back(std::move(event));
        }
    }
    result.gap = std::move(gap);
    return result;
}

std::vector<Event> EventBus::recent(std::int64_t limit,
                                    const std::optional<std::vector<std::string>>& types) const {
    const auto filter = type_set(types);
    std::vector<Event> events;
    {
        std::lock_guard lock(state_->mutex);
        for (const Event& event : state_->buffer) {
            if (wanted(filter, event.type)) {
                events.push_back(event);
            }
        }
    }
    if (limit <= 0) {
        return {};
    }
    if (static_cast<std::uint64_t>(limit) < events.size()) {
        events.erase(events.begin(), events.end() - static_cast<std::ptrdiff_t>(limit));
    }
    return events;
}

std::int64_t EventBus::last_seq() const {
    std::lock_guard lock(state_->mutex);
    return state_->last_seq;
}

void EventBus::clear() {
    std::lock_guard lock(state_->mutex);
    // Oldest first, so the newest of each type stays.
    for (const Event& event : state_->buffer) {
        state_->dropped[event.type] = event.seq;
    }
    state_->buffer.clear();
}

void EventBus::close() {
    auto& state = *state_;
    std::unique_lock lock(state.mutex);
    state.closed = true;
    state.pending.clear();
    for (const auto& subscription : state.subscriptions) {
        subscription->closed_ = true;
    }
    state.subscriptions.clear();
    state.wake.notify_all();
    if (!state.thread.joinable()) {
        return;
    }
    std::thread thread = std::move(state.thread);
    if (thread.get_id() == std::this_thread::get_id()) {
        // Closed from a subscriber: the thread ends on its own once the callback returns.
        thread.detach();
        return;
    }
    const bool finished =
        state.done.wait_for(lock, kJoinTimeout, [&] { return state.thread_done; });
    lock.unlock();
    if (finished) {
        thread.join();
    } else {
        // A subscriber is stuck; the thread keeps the state alive and ends after it returns.
        thread.detach();
    }
}

std::int64_t run_seq_base(const Clock& clock) {
    return std::chrono::duration_cast<std::chrono::milliseconds>(clock.utc().time_since_epoch())
        .count();
}

namespace {

struct Global {
    std::mutex mutex;
    std::shared_ptr<EventBus> bus;
    std::atomic<const Clock*> clock{nullptr};
};

Global& global() {
    static Global instance;
    return instance;
}

} // namespace

void set_run_clock(const Clock* clock) {
    global().clock = clock;
}

const Clock& run_clock() {
    const Clock* clock = global().clock.load();
    return clock != nullptr ? *clock : system_clock();
}

std::shared_ptr<EventBus> shared_bus() {
    auto& g = global();
    std::lock_guard lock(g.mutex);
    if (!g.bus) {
        const Clock& clock = run_clock();
        g.bus = std::make_shared<EventBus>(DEFAULT_BUFFER_SIZE, run_seq_base(clock), clock);
    }
    return g.bus;
}

EventBus& bus() {
    return *shared_bus();
}

std::shared_ptr<EventBus> set_bus(std::shared_ptr<EventBus> replacement) {
    auto& g = global();
    std::lock_guard lock(g.mutex);
    std::swap(g.bus, replacement);
    return replacement;
}

void clear_bus() {
    std::shared_ptr<EventBus> current;
    {
        auto& g = global();
        std::lock_guard lock(g.mutex);
        current = g.bus;
    }
    if (!current) {
        return;
    }
    try {
        current->clear();
    } catch (const std::exception& error) {
        // Deleted text could still be replayed to a reconnecting stream: say so.
        kLog.warning("Could not clear the event buffer: {}", automation::detail::error_type(error));
    }
}

std::optional<Event> publish(std::string_view type, Json data, bool replay) {
    try {
        return shared_bus()->publish_event(type, std::move(data), replay);
    } catch (const std::exception& error) {
        kLog.warning("Could not publish {}: {}", type, automation::detail::error_type(error));
        return std::nullopt;
    }
}

} // namespace fv::automation
