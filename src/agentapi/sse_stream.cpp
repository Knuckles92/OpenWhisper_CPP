#include "agentapi/sse_stream.h"

#include <algorithm>
#include <utility>

#include "agentapi/live_api.h"
#include "automation/catalog.h"

namespace fv::agentapi {

namespace {

constexpr std::string_view kRetry = "retry: 2000\n\n";
constexpr std::string_view kPing = ": ping\n\n";

std::chrono::milliseconds until(std::chrono::steady_clock::time_point when,
                                std::chrono::steady_clock::time_point now) {
    return std::max(std::chrono::milliseconds(0),
                    std::chrono::ceil<std::chrono::milliseconds>(when - now));
}

} // namespace

EventStream::EventStream(std::optional<std::vector<std::string>> types, std::int64_t replay_from,
                         std::string label, std::function<bool()> enabled,
                         std::function<bool()> allowed, automation::EventBus& bus, Executor& timers,
                         const Clock& clock, StreamOptions options)
    : types_(std::move(types)), replay_from_(replay_from), label_(std::move(label)),
      enabled_(std::move(enabled)), allowed_(std::move(allowed)), bus_(bus), timers_(timers),
      clock_(clock), options_(options) {}

EventStream::~EventStream() {
    Released released;
    {
        std::lock_guard lock(mutex_);
        if (!ended_) {
            released = end_locked();
        }
    }
    release(std::move(released));
}

void EventStream::start(std::shared_ptr<net::EventStream> out) {
    const std::weak_ptr<EventStream> weak = weak_from_this();
    Released released;
    {
        std::lock_guard lock(mutex_);
        out_ = std::move(out);
        subscription_ = bus_.subscribe(
            [weak](const automation::Event& event) {
                if (auto self = weak.lock()) {
                    self->deliver(event);
                }
            },
            types_, label_);
        out_->write(std::string(kRetry));
        // A position past the newest event is from another app session, whose numbering this
        // one does not continue: everything of this session follows a gap from 1.
        const std::int64_t last = bus_.last_seq();
        const automation::SinceResult replay = bus_.since(replay_from_, types_);
        const bool from_other_session = replay_from_ > last;
        if (replay.gap) {
            const std::int64_t missed = from_other_session ? 1 : replay_from_ + 1;
            out_->write(sse_frame(gap_event(missed, *replay.gap, clock_), false));
        }
        // The position only rises: a live event at or below it was replayed, or came before the
        // stream's start (the bus's dispatch can lag behind publishing), and sending it now
        // would repeat it or put it after newer ones.
        position_ = from_other_session ? 0 : replay_from_;
        for (const automation::Event& event : replay.events) {
            position_ = std::max(position_, event.seq);
            out_->write(sse_frame(event));
        }
        replayed_ = true;
        const auto now = clock_.steady();
        next_ping_ = now + options_.ping_interval;
        next_permission_ = now + options_.permission_check;
    }
    const bool enabled = !enabled_ || enabled_();
    {
        std::lock_guard lock(mutex_);
        if (!ended_) {
            if (!enabled || !pump_locked()) {
                released = end_locked();
            } else {
                schedule_tick_locked();
            }
        }
    }
    release(std::move(released));
}

void EventStream::close() {
    Released released;
    {
        std::lock_guard lock(mutex_);
        if (!ended_) {
            released = end_locked();
        }
    }
    release(std::move(released));
}

bool EventStream::ended() const {
    std::lock_guard lock(mutex_);
    return ended_;
}

std::int64_t EventStream::overflow_seq() const {
    std::lock_guard lock(mutex_);
    return overflow_seq_;
}

void EventStream::deliver(const automation::Event& event) {
    // The bus's dispatch thread: only queue, and hand frames to the socket while it takes them.
    {
        std::lock_guard lock(mutex_);
        if (ended_ || overflow_seq_ != 0) {
            return;
        }
    }
    // api.py's loop asks enabled() before every event it sends: once the live API is off, no
    // event goes out, not even one that arrives before the next check. Asked without the lock.
    const bool enabled = !enabled_ || enabled_();
    Released released;
    {
        std::lock_guard lock(mutex_);
        if (ended_ || overflow_seq_ != 0) {
            return;
        }
        if (!enabled) {
            released = end_locked();
        } else if (pending_.size() >= options_.queue_size) {
            overflow_seq_ = event.seq;
            // The client fell too far behind: say where it lost track and end, so it
            // reconnects with Last-Event-ID and replays.
            slow_gap_locked();
            released = end_locked();
        } else {
            pending_.push_back(event);
            if (replayed_ && !pump_locked()) {
                released = end_locked();
            }
        }
    }
    release(std::move(released));
}

void EventStream::pump() {
    const bool enabled = !enabled_ || enabled_();
    Released released;
    {
        std::lock_guard lock(mutex_);
        if (!ended_ && (!enabled || !pump_locked())) {
            released = end_locked();
        }
    }
    release(std::move(released));
}

bool EventStream::pump_locked() {
    while (!pending_.empty() && out_->queued_frames() < options_.socket_frames) {
        automation::Event event = std::move(pending_.front());
        pending_.pop_front();
        if (event.seq <= position_) {
            continue;
        }
        position_ = event.seq;
        if (!out_->write(sse_frame(event))) {
            return false; // the client has gone; nothing will read this stream again
        }
    }
    if (!pending_.empty()) {
        schedule_drain_locked();
    }
    return true;
}

void EventStream::schedule_drain_locked() {
    if (drain_timer_.active()) {
        return;
    }
    drain_timer_ =
        timers_.post_after(options_.drain_poll, [self = shared_from_this()] { self->pump(); });
}

void EventStream::schedule_tick_locked() {
    const auto now = clock_.steady();
    const auto wake = std::min({next_ping_, next_permission_, now + options_.enabled_check});
    tick_timer_ =
        timers_.post_after(until(wake, now), [self = shared_from_this()] { self->tick(); });
}

void EventStream::tick() {
    bool check_permission = false;
    {
        std::lock_guard lock(mutex_);
        if (ended_) {
            return;
        }
        check_permission = clock_.steady() >= next_permission_;
    }
    // Neither is asked under the lock: they read settings and the listener's state.
    const bool enabled = !enabled_ || enabled_();
    const bool allowed = !enabled || !check_permission || !allowed_ || allowed_();
    Released released;
    {
        std::lock_guard lock(mutex_);
        if (ended_) {
            return;
        }
        const auto now = clock_.steady();
        if (check_permission) {
            next_permission_ = now + options_.permission_check;
        }
        bool open = enabled && allowed && pump_locked();
        if (open && pending_.empty() && now >= next_ping_) {
            next_ping_ = now + options_.ping_interval;
            open = out_->write(std::string(kPing));
        }
        // An empty frame sends nothing and fails once the client has gone, which is how a
        // stream with nothing to say learns it.
        open = open && out_->write(std::string());
        if (open) {
            schedule_tick_locked();
        } else {
            released = end_locked();
        }
    }
    release(std::move(released));
}

void EventStream::slow_gap_locked() {
    std::int64_t missed = overflow_seq_;
    for (const automation::Event& event : pending_) {
        if (event.seq > position_) {
            missed = event.seq;
            break;
        }
    }
    pending_.clear();
    out_->write(sse_frame(gap_event(missed, automation::catalog::GAP_SLOW, clock_), false));
}

EventStream::Released EventStream::end_locked() {
    ended_ = true;
    pending_.clear();
    if (out_) {
        out_->close();
    }
    if (subscription_) {
        subscription_->close();
    }
    return Released{std::move(subscription_), std::move(tick_timer_), std::move(drain_timer_)};
}

void EventStream::release(Released released) {
    // Cancelling a timer drops its task, which may hold a reference to this stream: done without
    // the stream's lock, by a caller that holds a reference of its own.
    released.tick.cancel();
    released.drain.cancel();
    released.subscription.reset();
}

} // namespace fv::agentapi
