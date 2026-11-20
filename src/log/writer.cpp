#include "log/writer.h"

#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>
#include <utility>

#include "log/detail/files.h"

namespace fv::log {

struct CoalescedJob::State {
    std::weak_ptr<LogWriter::State> writer;
    std::function<void()> job;
    bool pending = false; // guarded by the writer's mutex
};

struct LogWriter::State {
    std::mutex mutex;
    std::condition_variable changed;
    // Every post and wake takes the next sequence number (from 1), so flush() can wait for
    // exactly the work submitted before it, whatever order the thread runs it in.
    std::deque<std::pair<std::uint64_t, std::function<void()>>> lines;
    std::deque<std::pair<std::uint64_t, std::shared_ptr<CoalescedJob::State>>> woken;
    std::size_t capacity = LogWriter::kDefaultCapacity;
    std::uint64_t last_sequence = 0;
    std::uint64_t running = 0; // sequence number of the job being run, 0 when none
    std::uint64_t dropped = 0;
    bool stopping = false;
    bool finished = false;

    // Under `mutex`: nothing numbered `sequence` or lower is still queued or running. Both
    // queues are in sequence order.
    bool done_through(std::uint64_t sequence) const {
        return finished || ((lines.empty() || lines.front().first > sequence) &&
                            (woken.empty() || woken.front().first > sequence) &&
                            (running == 0 || running > sequence));
    }

    void post(std::function<void()> job) {
        {
            std::lock_guard lock(mutex);
            if (stopping) {
                return;
            }
            if (lines.size() >= capacity) {
                lines.pop_front();
                ++dropped;
            }
            lines.emplace_back(++last_sequence, std::move(job));
        }
        changed.notify_all();
    }

    bool flush(std::chrono::milliseconds budget) {
        std::unique_lock lock(mutex);
        const std::uint64_t target = last_sequence;
        return changed.wait_for(lock, budget, [&] { return done_through(target); });
    }

    void run() {
        detail::set_thread_name("fv-log");
        std::unique_lock lock(mutex);
        for (;;) {
            changed.wait(lock, [&] { return stopping || !lines.empty() || !woken.empty(); });
            if (lines.empty() && woken.empty()) {
                break;
            }
            // Coalesced work first: it is the journal and the metrics, which a flood of log
            // lines must not starve.
            std::function<void()> work;
            std::shared_ptr<CoalescedJob::State> job;
            if (!woken.empty()) {
                running = woken.front().first;
                job = std::move(woken.front().second);
                woken.pop_front();
                job->pending = false;
            } else {
                running = lines.front().first;
                work = std::move(lines.front().second);
                lines.pop_front();
            }
            lock.unlock();
            try {
                job ? job->job() : work();
            } catch (...) {
                // A log sink never takes the writer down.
            }
            job.reset();
            work = nullptr;
            lock.lock();
            running = 0;
            changed.notify_all();
        }
        finished = true;
        changed.notify_all();
    }
};

void CoalescedJob::wake() const {
    const auto writer = state_->writer.lock();
    if (!writer) {
        return;
    }
    {
        std::lock_guard lock(writer->mutex);
        if (state_->pending || writer->stopping) {
            return;
        }
        state_->pending = true;
        writer->woken.emplace_back(++writer->last_sequence, state_);
    }
    writer->changed.notify_all();
}

LogWriter::LogWriter(std::size_t capacity) : state_(std::make_shared<State>()) {
    state_->capacity = capacity == 0 ? 1 : capacity;
    std::thread([state = state_] { state->run(); }).detach();
}

LogWriter::~LogWriter() {
    if (!stopped_) {
        stop(std::chrono::seconds(1));
    }
}

void LogWriter::post(std::function<void()> job) {
    state_->post(std::move(job));
}

CoalescedJob LogWriter::coalesced(std::function<void()> job) {
    auto state = std::make_shared<CoalescedJob::State>();
    state->writer = state_;
    state->job = std::move(job);
    return CoalescedJob(std::move(state));
}

LogQueue LogWriter::queue() const {
    return LogQueue(state_);
}

bool LogWriter::flush(std::chrono::milliseconds budget) {
    return state_->flush(budget);
}

std::uint64_t LogWriter::dropped() const {
    std::lock_guard lock(state_->mutex);
    return state_->dropped;
}

void LogWriter::stop(std::chrono::milliseconds budget) {
    stopped_ = true;
    std::unique_lock lock(state_->mutex);
    state_->stopping = true;
    state_->changed.notify_all();
    // The thread is detached from the start: it holds its own reference to the state, so leaving
    // it inside a stalled write is safe, and there is no join to wait on.
    state_->changed.wait_for(lock, budget, [&] { return state_->finished; });
}

void LogQueue::post(std::function<void()> job) const {
    state_->post(std::move(job));
}

bool LogQueue::flush(std::chrono::milliseconds budget) const {
    return state_->flush(budget);
}

} // namespace fv::log
