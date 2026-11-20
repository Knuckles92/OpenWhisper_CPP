#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>

namespace fv::log {

class LogWriter;
class LogQueue;

// Work that is never dropped and never queued twice: wake() asks for one run on the writer, and
// wakes that arrive before that run starts collapse into it. The failure journal and the metrics
// file use it, so stalled storage costs one pending run, not a growing queue of writes.
// Affinity: thread-safe.
class CoalescedJob {
public:
    void wake() const;

private:
    friend class LogWriter;
    struct State;
    explicit CoalescedJob(std::shared_ptr<State> state) : state_(std::move(state)) {}
    std::shared_ptr<State> state_;
};

// The one thread ("fv-log") that touches the log file, the console, the failure journal and the
// metrics file. Callers hand it work and return at once; nothing they do waits for storage.
// Affinity: thread-safe; owned thread fv-log.
class LogWriter {
public:
    // Log lines queued while the writer is stalled; beyond it the oldest are dropped.
    static constexpr std::size_t kDefaultCapacity = 10'000;

    explicit LogWriter(std::size_t capacity = kDefaultCapacity);
    // stop(1 s), unless stop() was called already: its budget was the whole wait.
    ~LogWriter();
    LogWriter(const LogWriter&) = delete;
    LogWriter& operator=(const LogWriter&) = delete;

    // Queues one job (a log line). When `capacity` jobs are waiting, the oldest is dropped and
    // counted. Never blocks; ignored after stop.
    void post(std::function<void()> job);

    // A job run on this writer whenever its wake() is called.
    CoalescedJob coalesced(std::function<void()> job);

    // A handle to this writer's queue that stays safe to use after this object is gone.
    LogQueue queue() const;

    // Waits at most `budget` until everything posted or woken before the call has run or been
    // dropped; true when it has. Work submitted later does not count, even when the writer runs
    // it first (woken jobs go before log lines).
    bool flush(std::chrono::milliseconds budget);

    // Log lines dropped because the queue was full.
    std::uint64_t dropped() const;

    // Runs what is queued, within `budget`, and ends the thread. A thread still inside a job when
    // the budget runs out is left to finish on its own (its state outlives this object), so a
    // stalled disk never holds up exit.
    void stop(std::chrono::milliseconds budget);

    struct State;

private:
    std::shared_ptr<State> state_;
    bool stopped_ = false;
};

// A copyable handle to a writer's queue. Sinks hold one rather than a LogWriter&: the facade calls
// sinks from a snapshot, outside its lock, so another thread may still be inside a removed sink's
// write() after the LogWriter is destroyed. Once the writer has stopped, posts are ignored.
// Affinity: thread-safe.
class LogQueue {
public:
    // LogWriter::post.
    void post(std::function<void()> job) const;
    // LogWriter::flush.
    bool flush(std::chrono::milliseconds budget) const;

private:
    friend class LogWriter;
    explicit LogQueue(std::shared_ptr<LogWriter::State> state) : state_(std::move(state)) {}
    std::shared_ptr<LogWriter::State> state_;
};

} // namespace fv::log
