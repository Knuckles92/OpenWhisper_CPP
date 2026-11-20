#pragma once

#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <vector>

#include "base/cancel.h"
#include "base/clock.h"
#include "base/executor.h"
#include "base/json.h"
#include "base/subscription.h"
#include "llm/judge.h"
#include "meeting_state/repository.h"
#include "meeting_state/store.h"

namespace fv::meeting_intel {

// Advisory annotations over proposed items and their cited speech. The store subscriber only
// queues work; the serial worker verifies the current revision and sources before publishing.
class CitationVerifier {
public:
    struct Deps {
        std::function<std::shared_ptr<llm::Judge>()> judge;
        std::function<bool()> allowed;
        const Clock* clock = nullptr;
    };

    CitationVerifier(std::shared_ptr<meeting_state::MeetingStateStore> store,
                     std::shared_ptr<meeting_state::MeetingRepository> repository, Deps deps);
    ~CitationVerifier();
    void shutdown();
    // Python's `pending or busy`: an item or an invalidation is waiting or being handled.
    bool busy() const;

private:
    void observe(const std::vector<meeting_state::OpResult>& results);
    void enqueue(const Json& item);
    void drain();
    void invalidate(const std::set<std::string>& segments, bool all);
    Json sources(const Json& item);
    void check(const Json& item);
    bool allowed() const;

    std::shared_ptr<meeting_state::MeetingStateStore> store_;
    std::shared_ptr<meeting_state::MeetingRepository> repository_;
    Deps deps_;
    mutable std::mutex mutex_;
    std::deque<Json> pending_;
    std::set<std::string> invalidations_;
    bool invalidate_all_ = false;
    bool running_ = false;
    CancelSource closed_;
    ThreadPool worker_{"fv-citation-check", 1};
    Subscription subscription_;
};

} // namespace fv::meeting_intel
