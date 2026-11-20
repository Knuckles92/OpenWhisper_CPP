// MeetingEngine: pause, resume, End and shutdown (meeting/engine.py pause, resume, end,
// _await_start, _begin_end, _end_worker, _drain_end_capture, _persist_terminal_status,
// _run_offline_final_pass, _run_cloud_speaker_pass, _finish_failed_end, shutdown,
// _interrupt_finalization_on_shutdown).

#include <exception>
#include <format>
#include <utility>

#include "base/utf8.h"
#include "engine/error.h"
#include "meeting/detail/engine_internal.h"
#include "meeting/detail/py_value.h"
#include "meeting/engine.h"
#include "meeting/finalization.h"
#include "meeting/refinalize.h"
#include "meeting_state/repair.h"
#include "pyport/text.h"

namespace fv::meeting {

namespace {

const log::Logger& logger = detail::engine_logger;

std::chrono::milliseconds seconds(double value) {
    return std::chrono::milliseconds(static_cast<std::int64_t>(value * 1000.0));
}

} // namespace

// ---- pause and resume ----

void MeetingEngine::pause() {
    {
        const std::lock_guard lock(lifecycle_mutex_);
        // Once End claimed the meeting a terminal status must not turn into "paused".
        const auto current = store();
        if (!active_.load() || end_claimed_ || !current) {
            return;
        }
        meeting_clock_.pause();
        if (!current->update_runtime_fields(Json{{"status", "paused"}})) {
            meeting_clock_.resume();
            throw EngineError("Could not persist paused meeting state");
        }
    }
    emit_status();
}

void MeetingEngine::resume() {
    {
        const std::lock_guard lock(lifecycle_mutex_);
        const auto current = store();
        if (!active_.load() || end_claimed_ || !current) {
            return;
        }
        meeting_clock_.resume();
        if (!current->update_runtime_fields(Json{{"status", "active"}})) {
            meeting_clock_.pause();
            throw EngineError("Could not persist resumed meeting state");
        }
    }
    emit_status();
}

// ---- End ----

void MeetingEngine::end() {
    begin_end(END_DRAIN_TIMEOUT_S);
}

// start() claims the meeting before it builds anything, so an End arriving meanwhile would
// unwind a half-built pipeline while start() kept bringing it up. A start on the calling thread
// is never waited for (it would wait for itself).
void MeetingEngine::await_start(std::chrono::milliseconds timeout) {
    std::unique_lock lock(lifecycle_mutex_);
    if (!starting_ || start_thread_ == std::this_thread::get_id()) {
        return;
    }
    const bool finished =
        lifecycle_changed_.wait_for(lock, timeout, [this] { return start_complete_; });
    lock.unlock();
    if (!finished) {
        logger.warning("Meeting start did not finish within {:.0f}s; unwinding a partially "
                       "started pipeline",
                       std::chrono::duration<double>(timeout).count());
    }
}

void MeetingEngine::begin_end(double drain_timeout_s) {
    await_start(seconds(START_WAIT_TIMEOUT_S));
    {
        const std::lock_guard lock(lifecycle_mutex_);
        if (end_claimed_ || !active_.load()) {
            return;
        }
        processing_stop_ = true;
        end_claimed_ = true;
        end_.done = false;
        end_.started = true;
        end_.thread = std::thread([this, drain_timeout_s] {
            detail::name_engine_thread("fv-meeting-end");
            end_worker(drain_timeout_s);
            set_worker_done(end_);
        });
        end_.id = end_.thread.get_id();
    }
    // Outside the lock: a cancelled model load wakes on this thread.
    processing_cancel_.cancel();
}

bool MeetingEngine::wait_for_end(std::chrono::milliseconds timeout) {
    {
        const std::lock_guard lock(lifecycle_mutex_);
        if (!end_.started || end_.id == std::this_thread::get_id()) {
            return true;
        }
    }
    return wait_worker(end_, timeout);
}

void MeetingEngine::end_worker(double drain_timeout_s) {
    std::optional<std::string> terminal_status;
    bool ended_emitted = false;
    try {
        // Off "Live" at once: the drain and the passes can take a while, and an active status
        // would look hung.
        mark_ending();
        // engine.py hands End the live scheduler and core; here the drain shuts the live agent
        // down, so whether it was online and healthy is read first.
        const LiveIntelligenceAtEnd live = live_intelligence_at_end();
        const bool drained = drain_end_capture(drain_timeout_s);
        const std::int64_t unfinished = unfinished_chunks_after_drain();
        const std::optional<std::string> capture_error = capture_->integrity_error();
        const bool complete = drained && unfinished == 0 && !capture_error;

        EndRun::Effects effects;
        effects.set_finalization = [this](const FinalizationUpdate& update) {
            return set_finalization(update);
        };
        effects.redecode = [this](const ProgressCallback& progress) {
            return run_offline_final_pass(progress);
        };
        effects.redecode_failure = [this] {
            const std::lock_guard lock(state_mutex_);
            return offline_failure_detail_;
        };
        effects.identify_speakers = [this](const ProgressCallback& progress,
                                           const std::function<void()>& on_start) {
            return run_cloud_speaker_pass(progress, on_start, false);
        };
        effects.allow_agent_writes = [this] {
            allow_agent_writes();
        };
        effects.revoke_agent_writes = [this] {
            revoke_agent_writes();
        };
        // With the live agent online, each final pass runs through a fresh core of the provider
        // (Python reuses the live core; EndRun plans no pass without it).
        const auto context_for = [this](ProgressCallback progress) {
            RerunContext context;
            context.meeting_id = meeting_id();
            context.store = store();
            context.repository = deps_.repository;
            context.meeting = deps_.repository->get_meeting(context.meeting_id).value_or(Json());
            context.state = context.store->snapshot();
            context.segments =
                deps_.repository->get_segments(context.meeting_id, -1.0, std::nullopt);
            context.progress = std::move(progress);
            {
                const std::lock_guard lock(state_mutex_);
                context.approved_route = live_approved_route_;
            }
            context.writes_allowed = [this] {
                return agent_writes_allowed();
            };
            return context;
        };
        effects.polish = [this, context_for](double timeout, const ProgressCallback& progress) {
            auto context = context_for(progress);
            context.options.timeout_s = timeout;
            return intelligence_->polish(context, final_insights_cancel().token());
        };
        effects.consolidate = [this, context_for](const auto& progress) {
            const auto context =
                context_for([progress](const std::string& line, int, int) { progress(line); });
            const Json outcome =
                intelligence_->consolidate(context, final_insights_cancel().token());
            repair_after_consolidation(*context.store, context.segments, outcome);
            return outcome;
        };
        effects.core_is_healthy = [healthy = live.healthy] {
            return healthy;
        };
        effects.offline_message = [this] {
            return intelligence_offline_message();
        };
        effects.summary_stats = [this] {
            return finalization_summary_stats();
        };
        EndOptions end_options;
        end_options.meeting_id = meeting_id();
        end_options.complete = complete;
        end_options.unfinished = unfinished;
        end_options.has_scheduler = live.online;
        end_options.will_offline = will_redecode_at_end();
        end_options.will_speaker_id = will_identify_speakers_at_end();
        EndRun run(std::move(effects), std::move(end_options));

        // The live draft is repaired at once so the dashboard is useful the moment capture ends;
        // the offline pass may replace it.
        repair_live_draft();
        meeting_clock_.pause();
        const std::string status = complete ? "ended" : "needs_recovery";
        persist_terminal_status(status);
        terminal_status = status;
        stop_heartbeat();

        const auto current = store();
        const bool cloud_enabled =
            current && current->with_state([](const meeting_state::MeetingState& state) {
                return state.cloud_enabled;
            });
        run.plan(cloud_enabled, options_.end_polish, options_.end_report);
        run.announce();

        active_ = false;
        broadcast(Json{{"type", "meeting_ended"}, {"status", status}});
        emit_status();
        emit("ended", Json{{"meeting_id", meeting_id()},
                           {"canceled", false},
                           {"status", status},
                           {"unfinished_chunks", unfinished},
                           {"capture_error", capture_error ? Json(*capture_error) : Json()}});
        ended_emitted = true;

        run.redecode();
        // The remaining passes use saved audio and text: the local model is freed first, so
        // another meeting can own it.
        if (stop_asr("end")) {
            emit("background_ready", Json{{"meeting_id", meeting_id()}});
        }
        run.identify_speakers();
        run.insights();
        wrap_up_end(run);
    } catch (const std::exception& error) {
        logger.error_with(error, "Meeting end failed");
        active_ = false;
        revoke_agent_writes();
        emit("error", Json{{"code", "end_failed"}, {"message", error.what()}});
        finish_failed_end(error.what(), terminal_status, ended_emitted);
    }
}

void MeetingEngine::mark_ending() {
    const auto current = store();
    if (!current) {
        return;
    }
    bool persisted = false;
    try {
        current->update_runtime_fields(Json{{"status", "ending"}});
        persisted = true;
    } catch (const std::exception& error) {
        logger.error_with(error, "Could not mark meeting as ending");
    }
    if (persisted) {
        emit_status();
    }
}

bool MeetingEngine::drain_end_capture(double drain_timeout_s) {
    if (hooks_.before_drain) {
        hooks_.before_drain();
    }
    capture_->stop();
    stop_fast_features();
    stop_live_intelligence();
    if (hooks_.before_flush) {
        hooks_.before_flush();
    }
    capture_->flush();
    bool ready = false;
    {
        const std::lock_guard lock(state_mutex_);
        ready = asr_ready_;
    }
    if (!ready) {
        // A loading model is cancelled; unfinished durable chunks stay pending for recovery
        // instead of waiting for a model at End.
        stop_asr("end during processing startup");
    }
    bool drained = true;
    if (const auto current = asr()) {
        try {
            drained = current->drain(seconds(drain_timeout_s));
        } catch (const std::exception& error) {
            logger.error_with(error, "ASR drain failed at meeting end");
            drained = false;
        }
    }
    return drained;
}

std::int64_t MeetingEngine::unfinished_chunks_after_drain() {
    try {
        return deps_.repository->count_unfinished_chunks(meeting_id());
    } catch (const std::exception& error) {
        logger.error_with(error, "Could not verify unfinished ASR chunks");
        return 1;
    }
}

bool MeetingEngine::will_redecode_at_end() const {
    return options_.end_redecode && asr() != nullptr;
}

bool MeetingEngine::will_identify_speakers_at_end() const {
    return !options_.demo_mode && options_.speaker_id_backend == "openai";
}

void MeetingEngine::repair_live_draft() {
    const auto current = store();
    if (!current) {
        return;
    }
    try {
        meeting_state::repair_meeting_state(*current, get_transcript());
    } catch (const std::exception& error) {
        logger.error_with(error, "Immediate end-state repair failed");
    }
}

void MeetingEngine::persist_terminal_status(const std::string& status) {
    if (const auto current = store()) {
        if (!current->update_runtime_fields(Json{{"status", status}})) {
            throw EngineError("Could not persist terminal meeting state");
        }
    }
    try {
        deps_.repository->update_meeting(meeting_id(),
                                         Json{{"status", status},
                                              {"ended_at", utc_now()},
                                              {"paused_total_s", meeting_clock_.paused_total_s()}});
    } catch (const std::exception& error) {
        logger.error_with(error, "Failed to persist terminal meeting metadata");
        throw;
    }
}

std::string MeetingEngine::intelligence_offline_message() const {
    std::string failure;
    {
        const std::lock_guard lock(state_mutex_);
        failure = intelligence_failure_;
    }
    return py::strip(std::format("{} {}", kIntelligenceOfflineMessage, failure));
}

Json MeetingEngine::finalization_summary_stats() {
    double duration_s = 0.0;
    try {
        duration_s = meeting_clock_.now_s();
    } catch (const std::exception& error) {
        logger.error_with(error, "Could not read meeting duration for summary");
    }
    Json cards = Json::object();
    Json questions = Json::array();
    if (const auto current = store()) {
        try {
            const Json snapshot = current->snapshot();
            cards = detail::get(snapshot, "cards");
            questions = detail::get(snapshot, "questions");
        } catch (const std::exception& error) {
            logger.error_with(error, "Could not collect card stats for summary");
        }
    }
    std::vector<Json> transcript;
    try {
        transcript = get_transcript();
    } catch (const std::exception& error) {
        logger.error_with(error, "Could not collect transcript stats for summary");
    }
    return summary_stats(cards, questions, transcript, duration_s);
}

// Re-decodes the session audio and replaces the draft transcript: the same rerun_redecode a
// retry runs, fed by the meeting's own ASR and diarizer while they are still loaded.
bool MeetingEngine::run_offline_final_pass(const ProgressCallback& progress) {
    {
        const std::lock_guard lock(state_mutex_);
        offline_failure_detail_.clear();
    }
    const auto current = asr();
    const std::string id = meeting_id();
    if (!current || id.empty() || !options_.end_redecode) {
        return false;
    }
    RedecodeRequest request;
    request.store = store();
    request.transcribe = [current](const std::filesystem::path& spool_dir,
                                   const std::vector<Json>& chunks, const OfflineProgress& report) {
        return current->transcribe_offline_session(spool_dir, chunks, report, CancelToken());
    };
    request.progress = progress;
    request.redecode_coverage_guard = options_.redecode_coverage_guard;
    request.speaker_id_backend = options_.speaker_id_backend;
    request.diarizer = std::optional<diarize::Diarizer*>(diarizer().get());
    request.clock = &clock_;
    const Json result = rerun_redecode(deps_.repository, id, request);
    if (!detail::truthy(detail::get(result, "ok"))) {
        if (detail::truthy(detail::get(result, "kept_draft"))) {
            const std::lock_guard lock(state_mutex_);
            offline_failure_detail_ = detail::str_or_empty(detail::get(result, "error"));
        }
        return false;
    }
    const Json rows = detail::list_or_empty(detail::get(result, "rows"));
    const Json removed = detail::list_or_empty(detail::get(result, "removed_ids"));
    emit("segments", Json{{"items", rows}, {"removed_ids", removed}});
    broadcast(Json{{"type", "segments"}, {"items", rows}, {"removed_ids", removed}});
    logger.info("Offline ASR replaced transcript: {} segments ({} removed)", rows.size(),
                removed.size());
    return true;
}

// The OpenAI speaker pass; never throws. `injected_decoder` stands for Python's transcribe_fn:
// a decoder that needs no key.
Json MeetingEngine::run_cloud_speaker_pass(const ProgressCallback& progress,
                                           const std::function<void()>& on_start,
                                           bool injected_decoder) {
    std::function<bool()> key_present;
    if (!injected_decoder) {
        key_present = deps_.openai_key_present ? deps_.openai_key_present
                                               : std::function<bool()>([] { return false; });
    }
    const SpeakerPassGate gate = speaker_pass_gate(
        options_.speaker_id_backend, options_.speaker_id_audio_consent, key_present, clock_);
    const auto current = store();
    const std::string id = meeting_id();
    if (gate.ok && (id.empty() || !current)) {
        return Json{{"ok", false},
                    {"skipped", false},
                    {"applied", 0},
                    {"error", "Meeting is not ready for speaker identification."}};
    }
    if (!current) {
        return Json{{"ok", false},  {"skipped", true}, {"applied", 0},
                    {"created", 0}, {"windows", 0},    {"error", gate.reason}};
    }
    std::filesystem::path spool;
    {
        const std::lock_guard lock(state_mutex_);
        spool = spool_dir_;
    }
    return run_speaker_pass(*deps_.repository, id, *current, path_to_utf8(spool), gate, progress,
                            on_start);
}

void MeetingEngine::wrap_up_end(EndRun& run) {
    // A meeting awaiting recovery takes no final highlights pass, so only a complete one asks.
    if (run.complete()) {
        finalize_highlights();
        if (hooks_.final_highlights) {
            hooks_.final_highlights();
        }
        review_final_insights();
    }
}

// Unwinds after end_worker threw, still reporting `ended`: the desktop leaves exclusive meeting
// mode only on it. Best effort; never throws. A terminal status already saved is kept and only
// the finalization is marked failed.
void MeetingEngine::finish_failed_end(const std::string& error,
                                      const std::optional<std::string>& terminal,
                                      bool ended_emitted) {
    stop_asr("failed end");
    revoke_agent_writes();
    // _shutdown_agent_core: a failure before the drain would leave the live agent running.
    try {
        stop_live_intelligence();
    } catch (const std::exception& failure) {
        logger.error_with(failure, "Live agent shutdown failed after a failed end");
    }
    try {
        meeting_clock_.pause();
    } catch (const std::exception& failure) {
        logger.error_with(failure, "Clock pause failed after a failed end");
    }
    stop_heartbeat();
    const std::string status = terminal.value_or("needs_recovery");
    if (!terminal) {
        try {
            deps_.repository->update_meeting(meeting_id(),
                                             Json{{"status", status}, {"ended_at", utc_now()}});
        } catch (const std::exception& failure) {
            logger.error_with(failure, "Failed to persist failed end status");
        }
    }
    if (store()) {
        record_failed_end_state(error, terminal, status);
    }
    if (!ended_emitted) {
        broadcast(Json{{"type", "meeting_ended"}, {"status", status}});
    }
    try {
        emit_status();
    } catch (const std::exception& failure) {
        logger.error_with(failure, "Status emit failed after a failed end");
    }
    if (!ended_emitted) {
        emit("ended", Json{{"meeting_id", meeting_id()},
                           {"canceled", false},
                           {"status", status},
                           {"error", error}});
    }
}

void MeetingEngine::record_failed_end_state(const std::string& error,
                                            const std::optional<std::string>& terminal,
                                            const std::string& status) {
    const auto current = store();
    if (!terminal) {
        try {
            current->update_runtime_fields(Json{{"status", status}});
        } catch (const std::exception& failure) {
            logger.error_with(failure, "Failed to update state status after a failed end");
        }
    }
    try {
        set_finalization({.status = "failed",
                          .message = std::format(
                              "Meeting end failed before final insights could finish: {}", error),
                          .emit = false});
    } catch (const std::exception& failure) {
        logger.error_with(failure, "Failed to persist finalization after a failed end");
    }
}

// ---- shutdown ----

void MeetingEngine::shutdown() {
    {
        const std::lock_guard transition(intelligence_toggle_mutex_);
        processing_stop_ = true;
    }
    final_insights_cancel().cancel();
    final_highlights_cancel_.cancel();
    processing_cancel_.cancel();
    if (active_.load()) {
        begin_end(SHUTDOWN_DRAIN_TIMEOUT_S);
    }
    wait_for_end(
        seconds(SHUTDOWN_DRAIN_TIMEOUT_S + kPolishTimeoutS + CONSOLIDATION_TIMEOUT_S + 60.0));
    // Best effort: a durable finalization "running" must never outlive the app.
    interrupt_finalization_on_shutdown();
    revoke_agent_writes();
    std::shared_ptr<DashboardTransport> transport;
    {
        const std::lock_guard lock(state_mutex_);
        transport = std::move(server_);
    }
    if (transport) {
        try {
            transport->stop();
        } catch (const std::exception& error) {
            logger.error_with(error, "Meeting web server stop failed");
        }
    }
    wait_worker(processing_, std::chrono::seconds(5));
    wait_worker(diarizer_init_, std::chrono::seconds(5));
    stop_heartbeat();
}

void MeetingEngine::interrupt_finalization_on_shutdown() {
    const auto current = store();
    if (!current) {
        return;
    }
    std::string status;
    bool cloud_enabled = false;
    try {
        std::tie(status, cloud_enabled) =
            current->with_state([](const meeting_state::MeetingState& state) {
                return std::pair(state.finalization.status, state.cloud_enabled);
            });
    } catch (const std::exception& error) {
        logger.error_with(error, "Could not read finalization during shutdown");
        return;
    }
    if (status != "running" && status != "pending") {
        return;
    }
    if (!cloud_enabled) {
        set_finalization({.status = "disabled",
                          .message = "AI insights are off for this meeting.",
                          .emit = false});
        return;
    }
    if (status == "running") {
        set_finalization({.status = "failed",
                          .message = "Final insights were interrupted by application shutdown.",
                          .emit = false});
    } else {
        set_finalization({.status = "unavailable",
                          .message = "Final insights did not run before shutdown.",
                          .emit = false});
    }
}

} // namespace fv::meeting
