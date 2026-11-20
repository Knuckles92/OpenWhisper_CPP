// The end of the meeting: the final transcript polish and the consolidation pass, each run once
// the rolling worker has stopped, and the outcome messages the finalization card keeps.

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <format>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <thread>
#include <utility>

#include "meeting_agent/detail/scheduler_support.h"
#include "meeting_agent/scheduler.h"
#include "meeting_state/repair.h"

namespace fv::meeting_agent {

using detail::scheduler_log;

namespace {

ConsolidationOutcome outcome(std::string_view status, std::string message) {
    return ConsolidationOutcome{std::string(status), std::move(message)};
}

} // namespace

ConsolidationOutcome detail::consolidation_outcome(const std::optional<AgentResult>& result,
                                                   double timeout_s) {
    if (!result) {
        scheduler_log.warning("Consolidation produced no result (timed out)");
        return outcome("failed", std::format("Final insights timed out after {}s.",
                                             static_cast<long long>(timeout_s)));
    }
    if (result->ok) {
        scheduler_log.info("Consolidation done: {}/{} ops applied", detail::applied_count(*result),
                           result->op_results.size());
        return outcome("completed", "Final insights are ready.");
    }
    const std::string error =
        result->error.empty() ? std::string("consolidation failed") : result->error;
    scheduler_log.warning("Consolidation failed: {}", error);
    return outcome("failed", "Final insights failed: " + error);
}

namespace {

// What the consolidation call and the thread running it share: the thread may outlive the
// scheduler when the agent ignores its cancel.
struct CallBox {
    std::mutex mutex;
    std::condition_variable done;
    std::optional<AgentResult> result;
};

// How long one wait for the consolidation call lasts before the clock is read again: the clock
// may be a test's, which moves without waking anyone.
constexpr std::chrono::milliseconds kClockPoll{20};

} // namespace

bool CheckpointScheduler::stop_inflight_checkpoint() {
    // Two agent runs sharing one core would interleave tool calls and corrupt a final pass; a
    // stale rolling checkpoint does far less damage, so a final pass starts only once the
    // worker is gone.
    if (join_worker(options_.consolidation_join_s)) {
        return true;
    }
    scheduler_log.warning("In-flight checkpoint still running; canceling before the final pass");
    try {
        agent_->cancel();
    } catch (const std::exception& error) {
        scheduler_log.error_with(error, "Agent cancel raised");
    }
    return join_worker(options_.consolidation_join_s);
}

void CheckpointScheduler::revoke_writes() {
    try {
        host_.revoke_agent_writes();
    } catch (const std::exception& error) {
        scheduler_log.error_with(error, "Engine revoke_agent_writes raised");
    }
}

ConsolidationOutcome
CheckpointScheduler::run_consolidation(std::optional<double> timeout_s,
                                       std::function<void(const std::string&)> progress) {
    consolidating_ = true;
    stop();
    // Every way out revokes agent writes first, so a late cancelled pass cannot change durable
    // state afterwards.
    if (auto skipped = consolidation_precheck()) {
        revoke_writes();
        return *skipped;
    }
    std::vector<Json> segments;
    try {
        segments = host_.get_transcript(-1.0);
    } catch (const std::exception& error) {
        scheduler_log.error_with(error, "Consolidation transcript fetch failed");
        revoke_writes();
        return outcome("failed", std::string("Could not load the transcript for final insights: ") +
                                     error.what());
    }
    auto payload = build_payload(segments, true);
    if (!payload) {
        scheduler_log.warning("No state store available; skipping consolidation");
        revoke_writes();
        return outcome("failed", "Meeting state was unavailable for final insights.");
    }
    const double timeout = timeout_s.value_or(CONSOLIDATION_TIMEOUT_CAP_S);
    const auto result = consolidate(*payload, segments.size(), timeout, progress);
    auto final_outcome = detail::consolidation_outcome(result, timeout);
    // Only this pass's ops already applied stay authoritative.
    revoke_writes();
    repair_after_consolidation(segments);
    return final_outcome;
}

std::optional<ConsolidationOutcome> CheckpointScheduler::consolidation_precheck() {
    if (!stop_inflight_checkpoint()) {
        scheduler_log.error("Checkpoint worker did not stop; skipping the consolidation pass to "
                            "avoid two concurrent agent runs");
        return outcome("failed", "Final insights were skipped because a previous checkpoint was "
                                 "still running.");
    }
    if (!agent_->is_healthy()) {
        scheduler_log.warning("Agent core unhealthy; skipping consolidation pass");
        return outcome("unavailable", "AI insights are offline; final insights could not run.");
    }
    return std::nullopt;
}

std::optional<AgentResult>
CheckpointScheduler::consolidate(const CheckpointPayload& payload, std::size_t segment_count,
                                 double timeout_s,
                                 const std::function<void(const std::string&)>& progress) {
    agent_->set_progress_callback(progress);
    if (progress) {
        try {
            progress(std::format("Synthesizing executive summary, key decisions, and action items "
                                 "over {} segments...",
                                 segment_count));
        } catch (const std::exception& error) {
            scheduler_log.error_with(error, "Consolidation progress callback failed");
        }
    }
    scheduler_log.info("Running consolidation {} over {} segments (timeout {}s)",
                       payload.request_id, segment_count, detail::fixed(timeout_s, 0));

    // The bounded agent call of meeting/finalization.py run_agent_call: the pass runs on a
    // thread of its own that owns everything it touches, since a pass that ignores its cancel
    // is abandoned.
    auto box = std::make_shared<CallBox>();
    std::thread([agent = agent_, payload, box] {
        detail::set_thread_name("fv-meeting-consolidation");
        AgentResult result;
        try {
            result = agent->consolidate(payload);
        } catch (const std::exception& error) {
            scheduler_log.error_with(error, "Agent call meeting-consolidation raised");
            result = AgentResult{};
            result.error = error.what();
        } catch (...) {
            scheduler_log.error("Agent call meeting-consolidation raised");
            result = AgentResult{};
            result.error = "unknown error";
        }
        {
            std::lock_guard lock(box->mutex);
            box->result = std::move(result);
        }
        box->done.notify_all();
    }).detach();

    const double deadline = mono() + timeout_s;
    std::optional<AgentResult> result;
    {
        std::unique_lock lock(box->mutex);
        while (!box->result && mono() < deadline) {
            box->done.wait_for(lock, kClockPoll);
        }
        result = box->result;
    }
    if (!result) {
        scheduler_log.warning("meeting-consolidation timed out after {}s; canceling",
                              detail::fixed(timeout_s, 0));
        // Mute progress, then revoke before the cancel, so a pass that ignores the cancel can
        // neither report nor land late writes.
        agent_->set_progress_callback({});
        revoke_writes();
        try {
            agent_->cancel();
        } catch (const std::exception& error) {
            scheduler_log.error_with(error,
                                     "Agent cancel raised after meeting-consolidation timed out");
        }
        std::unique_lock lock(box->mutex);
        box->done.wait_for(lock, std::chrono::duration<double>(options_.cancel_grace_s),
                           [&] { return box->result.has_value(); });
    }
    agent_->set_progress_callback({});
    return result;
}

void CheckpointScheduler::repair_after_consolidation(const std::vector<Json>& segments) {
    // Even when the agent failed or skipped the timeline, evidenced key points are promoted (or
    // the transcript sampled) so the durable record has beats. A repair failure never replaces
    // the consolidation outcome.
    try {
        if (const auto store = host_.store()) {
            meeting_state::repair_meeting_state(*store, segments);
        }
    } catch (const std::exception& error) {
        scheduler_log.error_with(error, "State repair after consolidation failed");
    }
}

ConsolidationOutcome
CheckpointScheduler::run_final_polish(const PolishTranscript& polish, double timeout_s,
                                      std::function<void(const std::string&, int, int)> progress) {
    if (!polish) {
        throw std::invalid_argument("run_final_polish needs the transcript polish function");
    }
    consolidating_ = true;
    stop();
    if (!stop_inflight_checkpoint()) {
        scheduler_log.error("Checkpoint worker did not stop; skipping final polish");
        return outcome("failed", "Transcript cleanup was skipped because a previous checkpoint "
                                 "was still running.");
    }
    if (!agent_->is_healthy()) {
        return outcome("unavailable",
                       "Meeting intelligence is offline; transcript cleanup could not run.");
    }
    std::vector<Json> segments;
    try {
        segments = host_.get_transcript(-1.0);
    } catch (const std::exception& error) {
        scheduler_log.error_with(error, "Final polish transcript fetch failed");
        return outcome("failed",
                       std::string("Could not load the transcript for cleanup: ") + error.what());
    }
    if (segments.empty()) {
        return outcome("completed", "No transcript text needed cleanup.");
    }
    const auto store = host_.store();
    if (!store) {
        return outcome("failed", "Meeting state was unavailable for transcript cleanup.");
    }
    const auto error = polish(*agent_, *store, segments, timeout_s, progress);
    // Python's `if error:`: an empty reason is no failure.
    if (error && !error->empty()) {
        return outcome("failed", *error);
    }
    return outcome("completed", "Transcript cleanup is ready.");
}

} // namespace fv::meeting_agent
