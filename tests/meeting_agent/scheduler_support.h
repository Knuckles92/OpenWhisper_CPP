#pragma once

// What the scheduler tests share: the internals OpenWhisper's tests read and set (SchedulerProbe),
// tests/fakes/scheduler.py's rows and snapshots, the FakeAgent and FakeNotesAgent of the Python
// tests as FakeAgentCore hooks, and a captured log.

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <future>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include "agents/loopback_mcp.h"
#include "base/log.h"
#include "base/testing/fake_clock.h"
#include "meeting_agent/scheduler.h"
#include "meeting_agent/testing/fake_agent_core.h"
#include "meeting_agent/testing/fake_scheduler_host.h"
#include "meeting_agent/tool_policy.h"

namespace fv::meeting_agent {

// The scheduler's internals, as OpenWhisper's tests reach them (sched._fire(), ...). Callers
// use them while no worker runs, except where they say otherwise.
struct SchedulerProbe {
    static void fire(CheckpointScheduler& s) { s.fire(); }
    static void maybe_fire_notes(CheckpointScheduler& s) { s.maybe_fire_notes(); }
    static void maybe_fire_polish(CheckpointScheduler& s) { s.maybe_fire_polish(); }
    static void fire_note_request(CheckpointScheduler& s) { s.fire_note_request(); }
    static bool detect_topic_shift(CheckpointScheduler& s) { return s.detect_topic_shift(); }
    static double interval_for(const CheckpointScheduler& s, int pending) {
        return s.interval_for(pending);
    }
    static void mark_sent(CheckpointScheduler& s, const std::vector<Json>& segments) {
        s.mark_sent(segments);
    }
    static double base_interval_s(const CheckpointScheduler& s) { return s.base_interval_s_; }
    static std::optional<CheckpointCadence> cadence(const CheckpointScheduler& s) {
        return s.cadence_;
    }
    static double pace_notes_min(const CheckpointScheduler& s) { return s.pace_notes_min(); }

    static int pending_segments(const CheckpointScheduler& s) {
        std::lock_guard lock(s.mutex_);
        return s.pending_segments_;
    }
    static void set_pending_segments(CheckpointScheduler& s, int pending) {
        std::lock_guard lock(s.mutex_);
        s.pending_segments_ = pending;
    }
    static bool guidance_pending(const CheckpointScheduler& s) {
        std::lock_guard lock(s.mutex_);
        return s.guidance_pending_;
    }
    static bool notes_guidance_pending(const CheckpointScheduler& s) {
        std::lock_guard lock(s.mutex_);
        return s.notes_guidance_pending_;
    }
    static void set_notes_guidance_pending(CheckpointScheduler& s, bool pending) {
        std::lock_guard lock(s.mutex_);
        s.notes_guidance_pending_ = pending;
    }
    static double retry_not_before(const CheckpointScheduler& s) {
        std::lock_guard lock(s.mutex_);
        return s.retry_not_before_;
    }
    static void set_retry_not_before(CheckpointScheduler& s, double value) {
        std::lock_guard lock(s.mutex_);
        s.retry_not_before_ = value;
    }
    static double notes_retry_not_before(const CheckpointScheduler& s) {
        std::lock_guard lock(s.mutex_);
        return s.notes_retry_not_before_;
    }
    static void set_notes_retry_not_before(CheckpointScheduler& s, double value) {
        std::lock_guard lock(s.mutex_);
        s.notes_retry_not_before_ = value;
    }
    // Queues a request the way Python's tests append to _note_requests (no checks).
    static std::future<AgentResult> push_note_request(CheckpointScheduler& s, std::string text) {
        auto promise = std::make_shared<std::promise<AgentResult>>();
        auto future = promise->get_future();
        std::lock_guard lock(s.mutex_);
        s.note_requests_.push_back({std::move(text),
                                    [promise = std::move(promise)](AgentResult result) {
                                        promise->set_value(std::move(result));
                                    },
                                    {}});
        return future;
    }

    static double mono(const CheckpointScheduler& s) { return s.mono(); }
    static void set_last_fire_mono(CheckpointScheduler& s, double value) {
        s.last_fire_mono_ = value;
    }
    static void set_last_shift_check_mono(CheckpointScheduler& s, double value) {
        s.last_shift_check_mono_ = value;
    }
    static int successful_checkpoints(const CheckpointScheduler& s) {
        return s.successful_checkpoints_;
    }
    static void set_successful_checkpoints(CheckpointScheduler& s, int count) {
        s.successful_checkpoints_ = count;
    }
    static int consecutive_failures(const CheckpointScheduler& s) {
        return s.consecutive_failures_;
    }
    static std::map<std::string, double, std::less<>> sent_starts(const CheckpointScheduler& s) {
        return s.sent_starts_;
    }
    static std::map<std::string, double, std::less<>>
    notes_sent_starts(const CheckpointScheduler& s) {
        return s.notes_sent_starts_;
    }
    static double notes_max_sent_start_s(const CheckpointScheduler& s) {
        return s.notes_max_sent_start_s_;
    }
    static bool consolidating(const CheckpointScheduler& s) { return s.consolidating_; }
    static bool stop_requested(const CheckpointScheduler& s) { return s.stop_requested_; }
    static bool worker_running(const CheckpointScheduler& s) {
        std::lock_guard lock(s.thread_mutex_);
        return s.thread_.joinable() && !s.thread_exited_->is_set();
    }
    static bool has_worker(const CheckpointScheduler& s) {
        std::lock_guard lock(s.thread_mutex_);
        return s.thread_.joinable();
    }
};

namespace scheduler_test {

using testing::FakeAgentCore;
using testing::FakeSchedulerHost;
using namespace std::chrono_literals;

// A transcript row as engine.get_transcript returns it (tests/fakes/scheduler.py segment).
inline Json segment(const std::string& id, double start_s, const std::string& text = "hello there",
                    const std::string& channel = "mic") {
    return Json{{"id", id},
                {"start_s", start_s},
                {"end_s", start_s + 2.0},
                {"text", text},
                {"channel", channel}};
}

inline Json row(const std::string& id, double start_s, double end_s, const std::string& text) {
    return Json{{"id", id}, {"start_s", start_s}, {"end_s", end_s}, {"text", text}};
}

inline std::vector<std::string> ids_of(const std::vector<Json>& rows) {
    std::vector<std::string> ids;
    for (const auto& r : rows) {
        ids.push_back(r.value("id", std::string()));
    }
    return ids;
}

// A dashboard with a topic and a summary but no cards yet (blank_snapshot).
inline Json blank_snapshot() {
    return Json{{"meeting_id", "m_test"},
                {"seq", 1},
                {"cards", Json::object()},
                {"topic", Json{{"current", "seeded topic"}, {"history", Json::array()}}},
                {"rolling_summary", "seeded summary"}};
}

// A FakeNotesEngine: the note-taker tests' meeting with no cards yet.
inline std::unique_ptr<FakeSchedulerHost> notes_host(const std::vector<Json>& segments) {
    auto host = std::make_unique<FakeSchedulerHost>(segments);
    host->set_snapshot(blank_snapshot());
    return host;
}

inline AgentResult ok_with(const char* op_name) {
    AgentResult result;
    result.ok = true;
    meeting_state::OpResult op;
    op.ok = true;
    op.op = Json{{"op", op_name}};
    op.seq = 1;
    result.op_results.push_back(op);
    return result;
}

inline AgentResult failed(const std::string& error) {
    AgentResult result;
    result.error = error;
    return result;
}

// A FakeAgentCore playing test_meeting_scheduler.py's FakeAgent: card passes (calls) and notes
// passes (notes_calls) apart, the first fail_times card passes failing with fail_error, and a
// card pass blocking until `released` is set while `blocking` is on.
struct FakeAgent {
    std::shared_ptr<FakeAgentCore> core = std::make_shared<FakeAgentCore>();
    std::atomic<int> fail_left{0};
    std::string fail_error = "forced";
    std::atomic<bool> blocking{false};
    agents::Event entered;
    std::shared_ptr<agents::Event> released = std::make_shared<agents::Event>();

    explicit FakeAgent(int fail_times = 0, std::string error = "forced")
        : fail_left(fail_times), fail_error(std::move(error)) {
        released->set();
        core->set_hook([this](const CheckpointPayload& payload, AgentToolHost*) {
            if (payload.is_notes) {
                AgentResult ok;
                ok.ok = true;
                return ok;
            }
            entered.set();
            if (blocking) {
                released->wait(std::chrono::seconds(7));
                std::this_thread::sleep_for(10ms);
            }
            if (fail_left > 0) {
                --fail_left;
                return failed(fail_error);
            }
            return ok_with("set_topic");
        });
    }
    // Card, polish and consolidation passes (everything but notes), in order.
    std::vector<CheckpointPayload> calls() const {
        std::vector<CheckpointPayload> out;
        for (const auto& pass : core->passes()) {
            if (!pass.payload.is_notes) {
                out.push_back(pass.payload);
            }
        }
        return out;
    }
    std::vector<CheckpointPayload> notes_calls() const {
        std::vector<CheckpointPayload> out;
        for (const auto& pass : core->passes()) {
            if (pass.payload.is_notes) {
                out.push_back(pass.payload);
            }
        }
        return out;
    }
};

// tests/fakes/scheduler.py FakeNotesAgent: records every pass; the first fail_times fail
// "forced", the others apply one add_item.
struct FakeNotesAgent {
    std::shared_ptr<FakeAgentCore> core = std::make_shared<FakeAgentCore>();
    std::atomic<int> fail_left{0};

    explicit FakeNotesAgent(int fail_times = 0) : fail_left(fail_times) {
        core->set_hook([this](const CheckpointPayload&, AgentToolHost*) {
            if (fail_left > 0) {
                --fail_left;
                return failed("forced");
            }
            return ok_with("add_item");
        });
    }
    std::vector<CheckpointPayload> calls() const {
        std::vector<CheckpointPayload> out;
        for (const auto& pass : core->passes()) {
            out.push_back(pass.payload);
        }
        return out;
    }
};

inline std::vector<CheckpointPayload> card_passes(const std::vector<CheckpointPayload>& calls) {
    std::vector<CheckpointPayload> out;
    for (const auto& call : calls) {
        if (!call.is_notes && !call.is_polish && !call.is_consolidation) {
            out.push_back(call);
        }
    }
    return out;
}

// A FakeClock far from its epoch, as time.monotonic() is: 0 is the scheduler's "never".
inline void start_late(FakeClock& clock) {
    clock.advance(std::chrono::hours(10));
}

// Every log line written while it lives, as caplog.text.
class CapturedLog {
public:
    CapturedLog() : sink_(std::make_shared<Sink>()) { log::add_sink(sink_); }
    ~CapturedLog() { log::remove_sink(sink_); }
    CapturedLog(const CapturedLog&) = delete;
    CapturedLog& operator=(const CapturedLog&) = delete;

    std::string text() const {
        std::lock_guard lock(sink_->mutex);
        return sink_->text;
    }
    bool contains(std::string_view needle) const {
        return text().find(needle) != std::string::npos;
    }

private:
    struct Sink final : log::Sink {
        void write(const log::Record& record) override {
            std::lock_guard lock(mutex);
            text.append(record.message);
            text.push_back('\n');
        }
        mutable std::mutex mutex;
        std::string text;
    };
    std::shared_ptr<Sink> sink_;
};

// Polls until `done()` or the timeout (real time), for tests that drive the real worker.
template <class Done> bool eventually(Done done, std::chrono::milliseconds timeout = 3s) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (!done()) {
        if (std::chrono::steady_clock::now() >= deadline) {
            return false;
        }
        std::this_thread::sleep_for(10ms);
    }
    return true;
}

} // namespace scheduler_test
} // namespace fv::meeting_agent
