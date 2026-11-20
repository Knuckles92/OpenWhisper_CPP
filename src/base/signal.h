#pragma once

#include <functional>
#include <memory>
#include <mutex>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>

#include "base/executor.h"
#include "base/subscription.h"

namespace fv {

// Thread-safe multicast with Qt::AutoConnection semantics: a subscriber whose executor is
// current runs inline, in connection order (Python's synchronous callbacks); any other
// subscriber is posted. Never emit while holding a lock another thread needs.
// Posted calls carry copies of the arguments, so Args are values (copyable), not references.
// A subscriber that throws inline stops the emit there, as a raising Python callback does.
// Movable, not copyable: subscriptions stay with the moved-to signal.
// Affinity: thread-safe.
template <class... Args> class Signal {
public:
    Signal() : state_(std::make_shared<State>()) {}
    Signal(Signal&&) noexcept = default;
    Signal& operator=(Signal&&) noexcept = default;
    Signal(const Signal&) = delete;
    Signal& operator=(const Signal&) = delete;

    Subscription connect(Executor& executor, std::function<void(Args...)> fn) {
        auto slot = std::make_shared<Slot>(
            Slot{&executor, std::move(fn), std::make_shared<CallbackGuard>()});
        {
            std::lock_guard lock(state_->mutex);
            state_->slots.push_back(slot);
        }
        std::weak_ptr<State> weak = state_;
        const Slot* key = slot.get();
        return Subscription(slot->guard, [weak, key] {
            if (auto state = weak.lock()) {
                std::lock_guard lock(state->mutex);
                std::erase_if(state->slots,
                              [key](const auto& entry) { return entry.get() == key; });
            }
        });
    }

    void emit(const Args&... args) const {
        if (!state_) {
            return;
        }
        std::vector<std::shared_ptr<Slot>> slots;
        {
            std::lock_guard lock(state_->mutex);
            slots = state_->slots;
        }
        for (const auto& slot : slots) {
            if (slot->guard->closed()) {
                continue;
            }
            if (slot->executor->is_current()) {
                slot->guard->run([&] { slot->fn(args...); });
            } else {
                slot->executor->post(
                    [slot, values = std::tuple<std::decay_t<Args>...>(args...)]() mutable {
                        slot->guard->run([&] { std::apply(slot->fn, values); });
                    });
            }
        }
    }

    std::size_t subscriber_count() const {
        if (!state_) {
            return 0;
        }
        std::lock_guard lock(state_->mutex);
        return state_->slots.size();
    }

private:
    struct Slot {
        Executor* executor;
        std::function<void(Args...)> fn;
        std::shared_ptr<CallbackGuard> guard;
    };
    struct State {
        std::mutex mutex;
        std::vector<std::shared_ptr<Slot>> slots;
    };

    std::shared_ptr<State> state_;
};

} // namespace fv
