#pragma once

#include <atomic>
#include <functional>
#include <list>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include "automation/detail/thread_name.h"

namespace fv::automation::detail {

// The threads an engine starts, each joined once it has ended (Python's daemon threads, which
// the C++ engines must not leave running past their owner). spawn() joins the ones that have
// already ended; join_all() waits for the rest.
// Affinity: thread-safe; join_all() is never called from one of its own threads.
class WorkerThreads {
public:
    WorkerThreads() = default;
    ~WorkerThreads() { join_all(); }
    WorkerThreads(const WorkerThreads&) = delete;
    WorkerThreads& operator=(const WorkerThreads&) = delete;

    void spawn(std::string name, std::function<void()> work) {
        auto done = std::make_shared<std::atomic<bool>>(false);
        std::thread thread([name = std::move(name), work = std::move(work), done] {
            set_current_thread_name(name);
            work();
            done->store(true);
        });
        std::list<Entry> finished;
        {
            std::lock_guard lock(mutex_);
            for (auto it = threads_.begin(); it != threads_.end();) {
                if (it->done->load()) {
                    finished.splice(finished.end(), threads_, it++);
                } else {
                    ++it;
                }
            }
            threads_.push_back({std::move(thread), std::move(done)});
        }
        for (auto& entry : finished) {
            entry.thread.join(); // its work has returned; only the thread's end is left
        }
    }

    void join_all() {
        std::list<Entry> all;
        {
            std::lock_guard lock(mutex_);
            all.swap(threads_);
        }
        for (auto& entry : all) {
            if (entry.thread.joinable()) {
                entry.thread.join();
            }
        }
    }

private:
    struct Entry {
        std::thread thread;
        std::shared_ptr<std::atomic<bool>> done;
    };

    std::mutex mutex_;
    std::list<Entry> threads_;
};

} // namespace fv::automation::detail
