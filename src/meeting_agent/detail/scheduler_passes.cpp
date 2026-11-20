// After a card checkpoint: success and failure bookkeeping, health, and the note-taker and
// transcript-polish passes that follow on their own cadence.

#include <algorithm>
#include <utility>

#include "meeting_agent/detail/scheduler_support.h"
#include "meeting_agent/scheduler.h"

namespace fv::meeting_agent {

using detail::scheduler_log;

namespace {

std::string or_unknown(const std::string& error) {
    return error.empty() ? std::string("unknown") : error;
}

} // namespace

void CheckpointScheduler::checkpoint_failed(int claimed, bool guidance, const std::string& error,
                                            const std::string& request_id) {
    if (guidance) {
        notify_guidance();
    }
    record_failure(claimed, error, request_id);
}

void CheckpointScheduler::record_checkpoint_success(const std::string& request_id,
                                                    const std::vector<Json>& segments,
                                                    const AgentResult& result) {
    consecutive_failures_ = 0;
    mark_sent(segments);
    set_online(true);
    scheduler_log.info("Checkpoint {} done: {}/{} ops applied", request_id,
                       detail::applied_count(result), result.op_results.size());
    ++successful_checkpoints_;
    if (detail::revised_transcript(result)) {
        std::lock_guard lock(mutex_);
        notes_guidance_pending_ = true;
        ++guidance_generation_;
    }
}

void CheckpointScheduler::record_failure(int claimed, const std::string& error,
                                         const std::string& request_id) {
    ++consecutive_failures_;
    const auto step = std::min<std::size_t>(static_cast<std::size_t>(consecutive_failures_ - 1),
                                            RETRY_BACKOFF_S.size() - 1);
    const double delay = RETRY_BACKOFF_S[step];
    {
        std::lock_guard lock(mutex_);
        pending_segments_ += claimed;
        retry_not_before_ = mono() + delay;
    }
    const std::string id = request_id.empty() ? std::string("-") : request_id;
    scheduler_log.warning(
        "Checkpoint failed ({} consecutive, claimed={} request_id={}); retrying in {}s: {}",
        consecutive_failures_, claimed, id, detail::fixed(delay, 0), error);
    if (consecutive_failures_ >= MAX_CONSECUTIVE_FAILURES) {
        scheduler_log.error("Intelligence declared offline after {} consecutive checkpoint "
                            "failures (request_id={})",
                            consecutive_failures_, id);
        set_online(false, error);
    }
}

void CheckpointScheduler::maybe_fire_polish() {
    // Reviews transcript text on its count and time cadence, silence included.
    if (background_blocked() || !polish_due() || !agent_->is_healthy()) {
        return;
    }
    std::vector<Json> segments;
    try {
        segments = host_.get_transcript(-1.0);
    } catch (const std::exception& error) {
        scheduler_log.error_with(error, "Polish transcript fetch failed");
        return;
    }
    if (segments.empty()) {
        return;
    }
    // Fill from the newest speech, then restore chronological order.
    std::reverse(segments.begin(), segments.end());
    segments.resize(detail::first_polish_block_size(segments));
    std::reverse(segments.begin(), segments.end());
    auto payload = build_payload(segments, false, true);
    if (!payload) {
        return;
    }
    scheduler_log.info("Firing transcript polish {} over {} segments", payload->request_id,
                       segments.size());
    const AgentResult result = run_agent_pass(*payload, "Agent polish raised");
    // Attempted work counts too: a retry waits for the next cadence.
    last_polish_mono_ = mono();
    polish_checkpoint_mark_ = successful_checkpoints_;
    polish_retry_pending_ = !result.ok;
    if (!result.ok) {
        scheduler_log.warning("Polish {} failed: {}", payload->request_id,
                              or_unknown(result.error));
        return;
    }
    if (detail::revised_transcript(result)) {
        notify_guidance();
    }
    scheduler_log.info("Polish {} done: {}/{} ops applied", payload->request_id,
                       detail::applied_count(result), result.op_results.size());
}

bool CheckpointScheduler::polish_due() const {
    const int since_mark = successful_checkpoints_ - polish_checkpoint_mark_;
    if (since_mark <= 0 && !polish_retry_pending_) {
        return false;
    }
    const double now = mono();
    const bool due_by_count = since_mark >= pace_polish_every();
    bool due_by_time = false;
    if (last_polish_mono_ != 0.0) {
        due_by_time = now - last_polish_mono_ >= pace_polish_min();
    } else {
        due_by_time = now - last_fire_mono_ >= options_.polish_initial_delay_s;
    }
    return due_by_count || due_by_time;
}

void CheckpointScheduler::maybe_fire_notes() {
    // Failures never count toward checkpoint health: a failed batch is not marked consumed, so
    // the notes page catches up on the next pass.
    if (background_blocked()) {
        return;
    }
    bool guidance = false;
    std::int64_t generation = 0;
    {
        std::lock_guard lock(mutex_);
        if (mono() < notes_retry_not_before_) {
            return;
        }
        guidance = notes_guidance_pending_;
        generation = guidance_generation_;
    }
    if (!notes_due(guidance) || !agent_->is_healthy()) {
        return;
    }
    auto notes_pass = prepare_notes_pass(guidance, generation);
    if (!notes_pass) {
        return;
    }
    scheduler_log.info("Firing note-taker pass {} over {} segments", notes_pass->payload.request_id,
                       notes_pass->segments.size());
    const AgentResult result = run_agent_pass(notes_pass->payload, "Agent note-taker pass raised");
    // The cadence moves whether or not the pass succeeded, so a failing core cannot hot-loop
    // notes calls; only a success marks the batch consumed.
    last_notes_mono_ = mono();
    notes_checkpoint_mark_ = successful_checkpoints_;
    if (result.ok) {
        finish_notes_pass(*notes_pass, result);
    } else {
        fail_notes_pass(*notes_pass, result);
    }
}

bool CheckpointScheduler::notes_due(bool guidance) const {
    const int since_mark = successful_checkpoints_ - notes_checkpoint_mark_;
    const bool due_by_count = since_mark >= pace_notes_every();
    const double now = mono();
    const bool due_by_time =
        (last_notes_mono_ > 0.0 && now - last_notes_mono_ >= pace_notes_min()) ||
        (last_notes_mono_ == 0.0 && successful_checkpoints_ >= 1);
    const bool resume_due = notes_resume_pending_ &&
                            (last_notes_mono_ == 0.0 || now - last_notes_mono_ >= pace_notes_min());
    return guidance || due_by_count || due_by_time || resume_due;
}

std::optional<CheckpointScheduler::NotesPass>
CheckpointScheduler::prepare_notes_pass(bool guidance, std::int64_t generation) {
    Fetch fetched;
    try {
        fetched = fetch_for_notes(guidance);
    } catch (const std::exception& error) {
        scheduler_log.error_with(error, "Notes transcript fetch failed");
        return std::nullopt;
    }
    auto segments = notes_segments(fetched.segments, guidance);
    if (segments.empty() && !guidance) {
        return std::nullopt;
    }
    auto payload = build_payload(segments, false, false, true);
    if (!payload) {
        return std::nullopt;
    }
    if (guidance) {
        // Cleared before the agent runs, so guidance arriving meanwhile stays queued for the
        // next pass.
        {
            std::lock_guard lock(mutex_);
            if (generation == guidance_generation_) {
                notes_guidance_pending_ = false;
            }
        }
        payload->state_snapshot["notes_review_requested"] = true;
    }
    return NotesPass{std::move(*payload), std::move(segments), guidance, fetched.more, fetched.seq};
}

void CheckpointScheduler::finish_notes_pass(const NotesPass& notes_pass,
                                            const AgentResult& result) {
    if (delivery_enabled_ && !notes_pass.guidance) {
        try {
            host_.advance_delivery_cursor("notes", notes_pass.delivery_seq);
        } catch (const std::exception& error) {
            scheduler_log.error_with(error, "Could not save note-taker transcript cursor");
            defer_notes_retry();
            return;
        }
        notes_cursor_seq_ = notes_pass.delivery_seq;
        notes_resume_pending_ = notes_pass.more_delivery;
    }
    scheduler_log.info("Note-taker pass {} done: {}/{} ops applied", notes_pass.payload.request_id,
                       detail::applied_count(result), result.op_results.size());
    if (notes_pass.guidance) {
        // A bounded review can skip older, still unprocessed speech: the automatic notes cursor
        // never moves past that backlog.
        return;
    }
    mark_notes_sent(notes_pass.segments);
    if (notes_pass.more_delivery) {
        wake_.set();
    }
}

void CheckpointScheduler::fail_notes_pass(const NotesPass& notes_pass, const AgentResult& result) {
    defer_notes_retry();
    if (notes_pass.guidance) {
        std::lock_guard lock(mutex_);
        notes_guidance_pending_ = true;
    }
    scheduler_log.warning("Note-taker pass {} failed: {}", notes_pass.payload.request_id,
                          or_unknown(result.error));
}

void CheckpointScheduler::defer_notes_retry() {
    const double not_before = mono() + pace_notes_min();
    std::lock_guard lock(mutex_);
    notes_retry_not_before_ = not_before;
}

} // namespace fv::meeting_agent
