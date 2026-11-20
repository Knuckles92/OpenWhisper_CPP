#include "hotkeys/dispatcher.h"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <exception>
#include <mutex>
#include <optional>
#include <thread>
#include <utility>
#include <vector>

#include "base/log.h"
#include "hotkeys/detail/thread_name.h"

namespace fv::hotkeys {

namespace {

constexpr log::Logger kLog{"services._hotkey_common"};

enum class ItemKind { Call, Wake, Stop };

struct Item {
    ItemKind kind;
    std::function<void()> call;
};

// One worker's queue; stop() gives the next worker a fresh one.
struct Queue {
    std::deque<Item> items;
};

struct Deadline {
    double when = 0;
    std::function<void()> call;
    std::uint64_t id = 0; // Python compares the deadline tuple by identity
};

void run_call(const std::function<void()>& call) {
    try {
        call();
    } catch (const std::exception& error) {
        kLog.error_with(error, "Hotkey handler failed");
    } catch (...) {
        kLog.error("Hotkey handler failed");
    }
}

} // namespace

namespace detail {

struct DispatcherState {
    std::string name;
    MonotonicClock clock;
    std::mutex mutex;
    std::condition_variable wake;
    std::shared_ptr<Queue> queue = std::make_shared<Queue>();
    bool running = false;
    std::optional<Deadline> deadline;
    std::uint64_t next_deadline_id = 1;
    std::vector<std::thread> threads;

    // The current worker's queue, starting a worker when none runs. Called with the lock held.
    std::shared_ptr<Queue> worker_queue(const std::shared_ptr<DispatcherState>& self) {
        if (!running) {
            running = true;
            // Workers stop() ended stay listed until the destructor joins them; a restart is
            // rare (a stop at shutdown, a test).
            threads.emplace_back([self, items = queue] { self->run(items); });
        }
        return queue;
    }

    void run(const std::shared_ptr<Queue>& items) {
        name_current_thread(name);
        std::unique_lock lock(mutex);
        while (true) {
            std::optional<Deadline> pending;
            if (items == queue && deadline) {
                pending = Deadline{deadline->when, {}, deadline->id};
            }
            if (items->items.empty()) {
                const auto has_items = [&items] {
                    return !items->items.empty();
                };
                if (pending) {
                    const double timeout = std::max(0.0, pending->when - clock());
                    wake.wait_for(lock, std::chrono::duration<double>(timeout), has_items);
                } else {
                    wake.wait(lock, has_items);
                }
                if (items->items.empty()) {
                    if (!deadline || deadline->id != pending->id || items != queue) {
                        continue;
                    }
                    auto call = std::move(deadline->call);
                    deadline.reset();
                    lock.unlock();
                    run_call(call);
                    lock.lock();
                    continue;
                }
            }
            Item item = std::move(items->items.front());
            items->items.pop_front();
            if (item.kind == ItemKind::Stop) {
                return;
            }
            if (item.kind == ItemKind::Call) {
                lock.unlock();
                run_call(item.call);
                lock.lock();
            }
        }
    }
};

} // namespace detail

OrderedDispatcher::OrderedDispatcher(std::string name, MonotonicClock clock)
    : state_(std::make_shared<detail::DispatcherState>()) {
    state_->name = std::move(name);
    state_->clock = std::move(clock);
}

OrderedDispatcher::~OrderedDispatcher() {
    stop();
    std::vector<std::thread> threads;
    {
        std::lock_guard lock(state_->mutex);
        threads.swap(state_->threads);
    }
    for (auto& thread : threads) {
        if (thread.get_id() == std::this_thread::get_id()) {
            thread.detach(); // a call destroying its own dispatcher
        } else if (thread.joinable()) {
            thread.join();
        }
    }
}

void OrderedDispatcher::submit(std::function<void()> call) {
    {
        std::lock_guard lock(state_->mutex);
        state_->worker_queue(state_)->items.push_back({ItemKind::Call, std::move(call)});
    }
    state_->wake.notify_all();
}

void OrderedDispatcher::call_at(double when, std::function<void()> call) {
    {
        std::lock_guard lock(state_->mutex);
        state_->deadline = Deadline{when, std::move(call), state_->next_deadline_id++};
        state_->worker_queue(state_)->items.push_back({ItemKind::Wake, {}});
    }
    state_->wake.notify_all();
}

void OrderedDispatcher::cancel_deadline() {
    std::lock_guard lock(state_->mutex);
    state_->deadline.reset();
}

void OrderedDispatcher::stop() {
    {
        std::lock_guard lock(state_->mutex);
        state_->deadline.reset();
        if (!state_->running) {
            return;
        }
        state_->running = false;
        state_->queue->items.push_back({ItemKind::Stop, {}});
        state_->queue = std::make_shared<Queue>();
    }
    state_->wake.notify_all();
}

} // namespace fv::hotkeys
