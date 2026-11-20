#pragma once

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "automation/event_bus.h"

namespace fv::automation {

// Subscribes to a bus and keeps every event it is handed, for tests that look at what was
// published. Closes its subscription when destroyed.
// Affinity: thread-safe.
class RecordingBusSubscriber {
public:
    explicit RecordingBusSubscriber(EventBus& bus,
                                    std::optional<std::vector<std::string>> types = std::nullopt,
                                    std::string label = {})
        : state_(std::make_shared<State>()) {
        subscription_ = bus.subscribe(
            [state = state_](const Event& event) {
                std::lock_guard lock(state->mutex);
                state->events.push_back(event);
                state->threads.push_back(std::this_thread::get_id());
                state->changed.notify_all();
            },
            std::move(types), std::move(label));
    }
    ~RecordingBusSubscriber() { subscription_->close(); }
    RecordingBusSubscriber(const RecordingBusSubscriber&) = delete;
    RecordingBusSubscriber& operator=(const RecordingBusSubscriber&) = delete;

    // Waits until at least `count` events arrived; false after `timeout`.
    bool wait_for(std::size_t count,
                  std::chrono::milliseconds timeout = std::chrono::milliseconds(2000)) const {
        std::unique_lock lock(state_->mutex);
        return state_->changed.wait_for(lock, timeout,
                                        [&] { return state_->events.size() >= count; });
    }
    std::vector<Event> events() const {
        std::lock_guard lock(state_->mutex);
        return state_->events;
    }
    std::vector<std::string> types() const {
        std::lock_guard lock(state_->mutex);
        std::vector<std::string> names;
        for (const Event& event : state_->events) {
            names.push_back(event.type);
        }
        return names;
    }
    // The threads the callback ran on, one per event.
    std::vector<std::thread::id> threads() const {
        std::lock_guard lock(state_->mutex);
        return state_->threads;
    }
    EventSubscription& subscription() { return *subscription_; }

private:
    struct State {
        mutable std::mutex mutex;
        std::condition_variable changed;
        std::vector<Event> events;
        std::vector<std::thread::id> threads;
    };
    std::shared_ptr<State> state_;
    std::shared_ptr<EventSubscription> subscription_;
};

} // namespace fv::automation
