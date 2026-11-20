#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "automation/event_bus.h"
#include "base/clock.h"
#include "base/executor.h"
#include "net/types.h"

// One client's GET /v1/events (services/automation/api.py EventStream): "retry: 2000", the
// replay of what the client missed (after an events.gap when it may have missed more), then live
// events until the stream ends. The bus hands events over on its dispatch thread, which only
// queues them here; frames go to the socket while it takes them, and a client that falls
// STREAM_QUEUE_SIZE events behind gets an events.gap with reason "slow" and the end, so that it
// reconnects with Last-Event-ID and replays the rest.
namespace fv::agentapi {

// Events waiting for one slow client; past this the stream ends with a gap.
inline constexpr std::size_t STREAM_QUEUE_SIZE = 1000;
// Frames handed to the socket and not yet sent, past which it counts as paused. uvicorn paused
// the stream's generator once 64 KiB waited for the socket, and only then did events gather in
// api.py's queue; an event's frame is a few hundred bytes, so 64 frames stand for that buffer.
inline constexpr std::size_t SOCKET_FRAMES = 64;

// Affinity: value type.
struct StreamOptions {
    std::size_t queue_size = STREAM_QUEUE_SIZE;
    std::size_t socket_frames = SOCKET_FRAMES;
    std::chrono::milliseconds ping_interval{15'000};
    // How often a stream rereads the live events permission.
    std::chrono::milliseconds permission_check{2'000};
    // How often a stream notices that the live API was switched off, or that its client left.
    std::chrono::milliseconds enabled_check{500};
    // How soon queued events are offered to a paused socket again.
    std::chrono::milliseconds drain_poll{20};
};

// Affinity: thread-safe. The bus's dispatch thread, the timers on `timers` and the caller of
// start() and close() may all be in it at once.
class EventStream : public std::enable_shared_from_this<EventStream> {
public:
    // `types`: the event types to send (nullopt: all). `replay_from`: the last event the client
    // has; it gets every buffered event after it. `enabled` and `allowed` are asked from the
    // timers, never under a lock: the live API being on, and the live events permission.
    // `enabled` is also asked before each live event goes out (on the bus's dispatch thread),
    // so that none is sent once it is false.
    // `bus` and `clock` must outlive the stream's start().
    EventStream(std::optional<std::vector<std::string>> types, std::int64_t replay_from,
                std::string label, std::function<bool()> enabled, std::function<bool()> allowed,
                automation::EventBus& bus, Executor& timers, const Clock& clock,
                StreamOptions options = {});
    // Ends the stream if it has not ended.
    ~EventStream();
    EventStream(const EventStream&) = delete;
    EventStream& operator=(const EventStream&) = delete;

    // Subscribes to the bus (before reading its buffer, so nothing falls between the replay and
    // the live events), writes "retry: 2000", the replay, then follows the bus into `out`. Ends
    // at once, after the replay, when enabled() is already false.
    void start(std::shared_ptr<net::EventStream> out);
    // Ends the stream now, without a frame: its subscription closes and `out` ends after the
    // frames already written.
    void close();
    bool ended() const;
    // The seq of the first event that found the queue full; 0 while none has.
    std::int64_t overflow_seq() const;

private:
    void deliver(const automation::Event& event);
    void tick();
    void pump();
    // The four below need mutex_ held. pump_locked() is false once the client has gone.
    bool pump_locked();
    void schedule_tick_locked();
    void schedule_drain_locked();
    void slow_gap_locked();
    // Ends the stream; the timers and subscription it hands back are released by the caller
    // after unlocking.
    struct Released {
        std::shared_ptr<automation::EventSubscription> subscription;
        TimerHandle tick;
        TimerHandle drain;
    };
    Released end_locked();
    static void release(Released released);

    const std::optional<std::vector<std::string>> types_;
    const std::int64_t replay_from_;
    const std::string label_;
    const std::function<bool()> enabled_;
    const std::function<bool()> allowed_;
    automation::EventBus& bus_;
    Executor& timers_;
    const Clock& clock_;
    const StreamOptions options_;

    mutable std::mutex mutex_;
    std::shared_ptr<net::EventStream> out_;
    std::shared_ptr<automation::EventSubscription> subscription_;
    std::deque<automation::Event> pending_;
    std::int64_t position_ = 0;
    std::int64_t overflow_seq_ = 0;
    bool replayed_ = false;
    bool ended_ = false;
    std::chrono::steady_clock::time_point next_ping_{};
    std::chrono::steady_clock::time_point next_permission_{};
    TimerHandle tick_timer_;
    TimerHandle drain_timer_;
};

} // namespace fv::agentapi
