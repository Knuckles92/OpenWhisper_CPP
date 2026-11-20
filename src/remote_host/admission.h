#pragma once

#include <chrono>
#include <cstddef>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>

#include "base/executor.h"

// How many connections and requests the host takes at once (services/remote_asr/host.py
// _Reservation and _WorkAdmission). Every connection starts in a handshake slot; a paired
// computer's hello trades it for a long-lived connection slot. Costly requests (decodes, model
// and runtime changes) then pass a FIFO work admission shared by every connection.
namespace fv::remote_host {

// A counting semaphore that never blocks (threading.BoundedSemaphore acquired without
// blocking). At least one slot.
// Affinity: thread-safe.
class SlotPool {
public:
    explicit SlotPool(int slots);
    bool try_acquire();
    void release();
    int available() const;
    int capacity() const { return capacity_; }

private:
    const int capacity_;
    mutable std::mutex mutex_;
    int available_;
};

// One slot of a SlotPool, released at most once (and on destruction).
// Affinity: one thread at a time.
class Reservation {
public:
    Reservation() = default;
    explicit Reservation(SlotPool& slots) : slots_(&slots) {}
    Reservation(Reservation&& other) noexcept;
    Reservation& operator=(Reservation&& other) noexcept;
    Reservation(const Reservation&) = delete;
    Reservation& operator=(const Reservation&) = delete;
    ~Reservation();

    bool acquire();
    void release();
    bool held() const { return held_; }

private:
    SlotPool* slots_ = nullptr;
    bool held_ = false;
};

// FIFO admission for costly requests across every paired connection: at most `active` run, at
// most `queued` wait, each for at most `wait`. A waiter holds no thread: its continuation runs
// once it is admitted (true) or refused (false: the queue was full, its wait ran out, or it was
// cancelled because its connection went away or sharing stopped).
// Affinity: thread-safe. Continuations run on the thread that decided them (the caller of
// acquire, release or cancel, or `timers`'), outside the admission's lock, and must not block.
class WorkAdmission {
public:
    using Done = std::function<void(bool admitted)>;
    struct Waiter;
    using Ticket = std::shared_ptr<Waiter>;

    // `timers` runs the wait deadlines and outlives the admission. active is at least 1,
    // queued at least 0.
    WorkAdmission(Executor& timers, int active, int queued, std::chrono::milliseconds wait);
    ~WorkAdmission();
    WorkAdmission(const WorkAdmission&) = delete;
    WorkAdmission& operator=(const WorkAdmission&) = delete;

    // Admits at once when a slot is free and nobody waits, refuses at once when the queue is
    // full; otherwise queues. The ticket cancels a queued waiter; it is null when `done` already
    // ran.
    Ticket acquire(Done done);
    // A waiter that is still queued leaves the queue and is refused; nothing otherwise.
    void cancel(const Ticket& ticket);
    // Refuses every waiter (sharing stopped).
    void cancel_all();
    // An admitted request finished: the next waiter in line takes its slot.
    void release();

    int active() const;
    std::size_t queued() const;

    // Shared with the deadline timers, which may still fire while the admission goes away.
    struct Core;

private:
    std::shared_ptr<Core> core_;
};

} // namespace fv::remote_host
