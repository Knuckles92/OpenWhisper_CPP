#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <vector>

#include "base/cancel.h"
#include "base/executor.h"
#include "llm/judge.h"
#include "meeting_state/repository.h"
#include "meeting_state/store.h"

namespace fv::meeting_intel {

// A bounded worker over completed transcript minutes. Model answers select exact source IDs;
// all text and time anchors in the published highlights come from the saved transcript.
class LiveHighlights {
public:
    struct Deps {
        std::function<std::shared_ptr<llm::Judge>()> judge;
        std::function<bool()> enabled;
        std::function<bool()> radar_enabled;
    };

    LiveHighlights(std::shared_ptr<meeting_state::MeetingStateStore> store,
                   std::shared_ptr<meeting_state::MeetingRepository> repository, Deps deps,
                   const CancelToken& cancel = {});
    ~LiveHighlights();
    void observe(const std::vector<Json>& rows, double frontier);
    // A separate end worker calls this after transcript edits and saves settle.
    void finalize(double budget_seconds = 30);
    void shutdown();
    // Python's `pending or busy`: a minute is waiting or being checked by the worker.
    bool busy() const;

private:
    void drain();
    void check(std::int64_t minute, bool final, std::optional<double> timeout = {});
    bool highlights_enabled() const;
    bool radar_enabled() const;

    std::shared_ptr<meeting_state::MeetingStateStore> store_;
    std::shared_ptr<meeting_state::MeetingRepository> repository_;
    Deps deps_;
    CancelSource closed_;
    mutable std::mutex mutex_;
    std::set<std::int64_t> pending_;
    std::int64_t next_minute_ = 0;
    bool running_ = false;
    std::mutex judgment_mutex_;
    std::map<std::int64_t, Json> checked_;
    ThreadPool worker_{"fv-live-highlights", 1};
};

} // namespace fv::meeting_intel
