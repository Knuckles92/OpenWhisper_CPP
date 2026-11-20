#pragma once

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string_view>
#include <utility>
#include <vector>

#include "base/clock.h"
#include "base/subscription.h"

namespace fv {

// A clock that moves only when told. Steady time starts at the steady epoch; wall time at
// 2026-01-01T00:00:00Z unless given; the zone is UTC unless given. A ManualExecutor built on it
// runs its due timers whenever the clock moves.
// Affinity: thread-safe.
class FakeClock final : public Clock {
public:
    FakeClock() : FakeClock(default_utc()) {}
    explicit FakeClock(std::chrono::system_clock::time_point utc, std::string_view zone = "UTC")
        : utc_(utc), zone_(std::chrono::locate_zone(zone)) {}

    std::chrono::steady_clock::time_point steady() const override {
        std::lock_guard lock(mutex_);
        return steady_;
    }
    std::chrono::system_clock::time_point utc() const override {
        std::lock_guard lock(mutex_);
        return utc_;
    }
    const std::chrono::time_zone* zone() const override {
        std::lock_guard lock(mutex_);
        return zone_;
    }

    // Moves steady and wall time together, then tells the listeners.
    void advance(std::chrono::nanoseconds by) {
        {
            std::lock_guard lock(mutex_);
            steady_ += std::chrono::duration_cast<std::chrono::steady_clock::duration>(by);
            utc_ += std::chrono::duration_cast<std::chrono::system_clock::duration>(by);
        }
        notify();
    }
    // A wall-clock jump (the user changed the time); steady time does not move.
    void set_utc(std::chrono::system_clock::time_point utc) {
        {
            std::lock_guard lock(mutex_);
            utc_ = utc;
        }
        notify();
    }
    void set_zone(std::string_view name) {
        const auto* zone = std::chrono::locate_zone(name);
        std::lock_guard lock(mutex_);
        zone_ = zone;
    }

    // Called after every move, on the moving thread, outside the clock's lock.
    Subscription on_change(std::function<void()> listener) {
        auto guard = std::make_shared<CallbackGuard>();
        std::uint64_t id = 0;
        {
            std::lock_guard lock(listeners_->mutex);
            id = listeners_->next_id++;
            listeners_->entries.push_back(
                {id, guard, std::make_shared<std::function<void()>>(std::move(listener))});
        }
        std::weak_ptr<Listeners> weak = listeners_;
        return Subscription(guard, [weak, id] {
            if (auto listeners = weak.lock()) {
                std::lock_guard lock(listeners->mutex);
                std::erase_if(listeners->entries,
                              [id](const Listener& entry) { return entry.id == id; });
            }
        });
    }

    static std::chrono::system_clock::time_point default_utc() {
        return std::chrono::sys_days{std::chrono::year{2026} / 1 / 1};
    }

private:
    struct Listener {
        std::uint64_t id;
        std::shared_ptr<CallbackGuard> guard;
        std::shared_ptr<std::function<void()>> fn;
    };
    struct Listeners {
        std::mutex mutex;
        std::vector<Listener> entries;
        std::uint64_t next_id = 0;
    };

    void notify() {
        std::vector<Listener> entries;
        {
            std::lock_guard lock(listeners_->mutex);
            entries = listeners_->entries;
        }
        for (const auto& listener : entries) {
            listener.guard->run([&] { (*listener.fn)(); });
        }
    }

    mutable std::mutex mutex_;
    std::chrono::steady_clock::time_point steady_{};
    std::chrono::system_clock::time_point utc_;
    const std::chrono::time_zone* zone_;
    std::shared_ptr<Listeners> listeners_ = std::make_shared<Listeners>();
};

} // namespace fv
