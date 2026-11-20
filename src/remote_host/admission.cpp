#include "remote_host/admission.h"

#include <algorithm>
#include <utility>
#include <vector>

namespace fv::remote_host {

// ---- SlotPool ----------------------------------------------------------------------------------

SlotPool::SlotPool(int slots) : capacity_(std::max(1, slots)), available_(capacity_) {}

bool SlotPool::try_acquire() {
    std::lock_guard lock(mutex_);
    if (available_ == 0) {
        return false;
    }
    --available_;
    return true;
}

void SlotPool::release() {
    std::lock_guard lock(mutex_);
    if (available_ < capacity_) {
        ++available_;
    }
}

int SlotPool::available() const {
    std::lock_guard lock(mutex_);
    return available_;
}

// ---- Reservation -------------------------------------------------------------------------------

Reservation::Reservation(Reservation&& other) noexcept
    : slots_(std::exchange(other.slots_, nullptr)), held_(std::exchange(other.held_, false)) {}

Reservation& Reservation::operator=(Reservation&& other) noexcept {
    if (this != &other) {
        release();
        slots_ = std::exchange(other.slots_, nullptr);
        held_ = std::exchange(other.held_, false);
    }
    return *this;
}

Reservation::~Reservation() {
    release();
}

bool Reservation::acquire() {
    held_ = slots_ != nullptr && slots_->try_acquire();
    return held_;
}

void Reservation::release() {
    if (held_) {
        held_ = false;
        slots_->release();
    }
}

// ---- WorkAdmission -----------------------------------------------------------------------------

struct WorkAdmission::Waiter {
    Done done;
    TimerHandle deadline;
};

struct WorkAdmission::Core : std::enable_shared_from_this<Core> {
    Core(Executor& timers, int active, int queued, std::chrono::milliseconds wait)
        : timers(timers), active_limit(std::max(1, active)),
          queue_limit(static_cast<std::size_t>(std::max(0, queued))), wait(wait) {}

    // Waiters that left the queue under the lock; their deadlines are cancelled and their
    // continuations run once it is released. A waiter's deadline handle is only touched under
    // the lock: acquire() stores it after queueing, when another thread may already admit it.
    struct Batch {
        std::vector<Ticket> waiters;
        std::vector<TimerHandle> deadlines;

        void take(Ticket waiter) {
            deadlines.push_back(std::move(waiter->deadline));
            waiters.push_back(std::move(waiter));
        }
        void run(bool admitted) {
            deadlines.clear();
            for (const auto& waiter : waiters) {
                if (auto done = std::exchange(waiter->done, nullptr)) {
                    done(admitted);
                }
            }
        }
    };

    // Takes the waiters at the front that a free slot now admits.
    void admit_locked(Batch& admitted) {
        while (!queue.empty() && active < active_limit) {
            admitted.take(std::move(queue.front()));
            queue.pop_front();
            ++active;
        }
    }

    // Takes the waiter out of the queue into `refused`; false when it had already left it.
    bool remove(const Ticket& waiter, Batch& refused, Batch& admitted) {
        std::lock_guard lock(mutex);
        const auto found = std::find(queue.begin(), queue.end(), waiter);
        if (found == queue.end()) {
            return false;
        }
        refused.take(*found);
        queue.erase(found);
        // The front waiter giving up lets the next one in, as Python's notify_all does.
        admit_locked(admitted);
        return true;
    }

    void expire(const std::weak_ptr<Waiter>& weak) {
        const auto waiter = weak.lock();
        if (!waiter) {
            return;
        }
        Batch refused;
        Batch admitted;
        remove(waiter, refused, admitted);
        refused.run(false);
        admitted.run(true);
    }

    Executor& timers;
    const int active_limit;
    const std::size_t queue_limit;
    const std::chrono::milliseconds wait;
    mutable std::mutex mutex;
    int active = 0;
    std::deque<Ticket> queue;
};

WorkAdmission::WorkAdmission(Executor& timers, int active, int queued,
                             std::chrono::milliseconds wait)
    : core_(std::make_shared<Core>(timers, active, queued, wait)) {}

WorkAdmission::~WorkAdmission() {
    cancel_all();
}

WorkAdmission::Ticket WorkAdmission::acquire(Done done) {
    auto waiter = std::make_shared<Waiter>();
    // Decided under the lock: once queued, the waiter's `done` belongs to whichever thread
    // admits or refuses it, so it is not looked at here again.
    enum class Decision { admitted, refused, queued } decision = Decision::queued;
    {
        std::lock_guard lock(core_->mutex);
        if (core_->active < core_->active_limit && core_->queue.empty()) {
            ++core_->active;
            decision = Decision::admitted;
        } else if (core_->queue.size() >= core_->queue_limit) {
            decision = Decision::refused;
        } else {
            waiter->done = std::move(done);
            core_->queue.push_back(waiter);
        }
    }
    if (decision != Decision::queued) {
        done(decision == Decision::admitted);
        return nullptr;
    }
    std::weak_ptr<Core> core = core_;
    std::weak_ptr<Waiter> weak = waiter;
    auto deadline = core_->timers.post_after(core_->wait, [core, weak] {
        if (auto self = core.lock()) {
            self->expire(weak);
        }
    });
    {
        std::lock_guard lock(core_->mutex);
        if (std::find(core_->queue.begin(), core_->queue.end(), waiter) != core_->queue.end()) {
            waiter->deadline = std::move(deadline);
        }
    }
    return waiter;
}

void WorkAdmission::cancel(const Ticket& ticket) {
    if (!ticket) {
        return;
    }
    Core::Batch refused;
    Core::Batch admitted;
    core_->remove(ticket, refused, admitted);
    refused.run(false);
    admitted.run(true);
}

void WorkAdmission::cancel_all() {
    Core::Batch refused;
    {
        std::lock_guard lock(core_->mutex);
        for (auto& waiter : core_->queue) {
            refused.take(std::move(waiter));
        }
        core_->queue.clear();
    }
    refused.run(false);
}

void WorkAdmission::release() {
    Core::Batch admitted;
    {
        std::lock_guard lock(core_->mutex);
        core_->active = std::max(0, core_->active - 1);
        core_->admit_locked(admitted);
    }
    admitted.run(true);
}

int WorkAdmission::active() const {
    std::lock_guard lock(core_->mutex);
    return core_->active;
}

std::size_t WorkAdmission::queued() const {
    std::lock_guard lock(core_->mutex);
    return core_->queue.size();
}

} // namespace fv::remote_host
