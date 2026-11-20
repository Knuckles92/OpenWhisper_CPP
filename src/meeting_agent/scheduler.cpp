#include "meeting_agent/scheduler.h"

#include <chrono>
#include <exception>
#include <set>
#include <stdexcept>
#include <utility>

#include "base/ids.h"
#include "meeting_agent/detail/py_value.h"
#include "meeting_agent/detail/scheduler_support.h"
#include "pyport/text.h"

namespace fv::meeting_agent {

using detail::scheduler_log;

Json ConsolidationOutcome::to_json() const {
    Json out = Json::object();
    out["status"] = status;
    out["message"] = message;
    return out;
}

CheckpointScheduler::CheckpointScheduler(SchedulerHost& host, std::shared_ptr<AgentCore> agent,
                                         const Clock& clock,
                                         std::function<void(bool, std::string)> on_health,
                                         TopicJudge topic_judge, SchedulerOptions options)
    : host_(host), agent_(std::move(agent)), clock_(clock), on_health_(std::move(on_health)),
      topic_judge_(std::move(topic_judge)), options_(std::move(options)),
      cadence_(agent_ ? agent_->cadence() : std::nullopt) {
    // The caller's interval, else the core's cadence, else the default.
    base_interval_s_ =
        options_.base_interval_s.value_or(cadence_ ? cadence_->base_s : BASE_INTERVAL_S);
    min_interval_s_ = options_.min_interval_s.value_or(cadence_ ? cadence_->min_s : MIN_INTERVAL_S);
    max_interval_s_ = options_.max_interval_s.value_or(cadence_ ? cadence_->max_s : MAX_INTERVAL_S);
    last_fire_mono_ = mono();
    started_mono_ = last_fire_mono_;
    delivery_enabled_ = host_.has_delivery_ledger();
    if (delivery_enabled_) {
        card_cursor_seq_ = host_.delivery_cursor("cards");
        notes_cursor_seq_ = host_.delivery_cursor("notes");
    }
}

CheckpointScheduler::~CheckpointScheduler() {
    stop_requested_ = true;
    cancel_note_requests();
    wake_.set();
    std::thread worker;
    {
        std::lock_guard lock(thread_mutex_);
        worker = std::move(thread_);
    }
    if (worker.joinable()) {
        if (worker.get_id() == std::this_thread::get_id()) {
            worker.detach();
        } else {
            worker.join();
        }
    }
}

double CheckpointScheduler::mono() const {
    return std::chrono::duration<double>(clock_.steady().time_since_epoch()).count();
}

double CheckpointScheduler::pace_polish_min() const {
    return cadence_ ? cadence_->polish_min_s : options_.polish_min_interval_s;
}

double CheckpointScheduler::pace_notes_min() const {
    return cadence_ ? cadence_->notes_min_s : options_.notes_min_interval_s;
}

int CheckpointScheduler::pace_polish_every() const {
    return cadence_ ? cadence_->polish_every : options_.polish_every;
}

int CheckpointScheduler::pace_notes_every() const {
    return cadence_ ? cadence_->notes_every : options_.notes_every;
}

void CheckpointScheduler::start() {
    {
        std::lock_guard lock(thread_mutex_);
        if (thread_.joinable() && !thread_exited_->is_set()) {
            return;
        }
    }
    // Query the durable backlog before starting: a failed database read must not leave an
    // orphan worker behind.
    const std::int64_t high_water = delivery_enabled_ ? host_.delivery_high_water() : 0;
    const bool has_backlog = high_water > card_cursor_seq_;
    notes_resume_pending_ = high_water > notes_cursor_seq_;
    stop_requested_ = false;
    last_fire_mono_ = mono();
    started_mono_ = last_fire_mono_;
    {
        std::lock_guard lock(thread_mutex_);
        if (thread_.joinable()) {
            thread_.join(); // a worker that already left its loop
        }
        auto exited = std::make_shared<agents::Event>();
        thread_exited_ = exited;
        thread_ = std::thread([this, exited] {
            detail::set_thread_name("fv-meeting-checkpoint");
            run_loop(exited);
        });
    }
    if (has_backlog) {
        notify_segments(1);
    } else if (notes_resume_pending_) {
        wake_.set();
    }
    scheduler_log.info("Checkpoint scheduler started (base={}s min={}s max={}s)",
                       detail::fixed(base_interval_s_, 0), detail::fixed(min_interval_s_, 0),
                       detail::fixed(max_interval_s_, 0));
}

bool CheckpointScheduler::join_worker(double timeout_s) {
    std::shared_ptr<agents::Event> exited;
    {
        std::lock_guard lock(thread_mutex_);
        if (!thread_.joinable()) {
            return true;
        }
        if (thread_.get_id() == std::this_thread::get_id()) {
            return false;
        }
        exited = thread_exited_;
    }
    if (!exited->wait(std::chrono::duration<double>(timeout_s))) {
        return false;
    }
    std::lock_guard lock(thread_mutex_);
    if (thread_.joinable() && thread_exited_ == exited) {
        thread_.join();
    }
    return true;
}

void CheckpointScheduler::stop() {
    stop_requested_ = true;
    cancel_note_requests();
    wake_.set();
    join_worker(options_.stop_join_s);
}

void CheckpointScheduler::prepare_for_end() {
    // Cancelling the agent here would delay capture shutdown and the user's End: an in-flight
    // pass is left to run_consolidation, after the meeting is already marked ended.
    consolidating_ = true;
    stop_requested_ = true;
    cancel_note_requests();
    wake_.set();
}

void CheckpointScheduler::notify_segments(int count) {
    if (count <= 0) {
        return;
    }
    {
        std::lock_guard lock(mutex_);
        pending_segments_ += count;
    }
    wake_.set();
}

void CheckpointScheduler::seed_sent_segments(const std::vector<Json>& segments) {
    mark_sent(segments);
    for (const auto& segment : segments) {
        const Json& id = detail::get(segment, "id");
        if (!detail::py_truthy(id)) {
            continue;
        }
        const double start_s = detail::row_start_s(segment);
        notes_sent_starts_[detail::py_str(id)] = start_s;
        if (start_s > notes_max_sent_start_s_) {
            notes_max_sent_start_s_ = start_s;
        }
    }
    prune_notes_sent();
    if (delivery_enabled_) {
        const std::int64_t high_water = host_.delivery_high_water();
        host_.advance_delivery_cursor("cards", high_water);
        host_.advance_delivery_cursor("notes", high_water);
        card_cursor_seq_ = std::max(card_cursor_seq_, high_water);
        notes_cursor_seq_ = std::max(notes_cursor_seq_, high_water);
        notes_resume_pending_ = false;
    }
}

void CheckpointScheduler::seed_sent_segments() {
    seed_sent_segments(host_.get_transcript(-1.0));
}

std::future<AgentResult> CheckpointScheduler::request_note_adjustment(std::string text) {
    auto promise = std::make_shared<std::promise<AgentResult>>();
    auto future = promise->get_future();
    request_note_adjustment(
        std::move(text), [promise](AgentResult result) { promise->set_value(std::move(result)); });
    return future;
}

namespace {

void complete_note_request(const std::function<void(AgentResult)>& completion, AgentResult value) {
    try {
        if (completion) {
            completion(std::move(value));
        }
    } catch (const std::exception&) {
        scheduler_log.warning("A note request completion callback failed.");
    }
}

} // namespace

void CheckpointScheduler::request_note_adjustment(std::string text,
                                                  std::function<void(AgentResult)> completion,
                                                  const CancelToken& permit) {
    const std::string stripped = py::strip(text);
    if (stripped.empty() || py::len(text) > NOTE_REQUEST_MAX_CHARS) {
        throw std::invalid_argument("Enter a note request of 1 to 4000 characters.");
    }
    {
        std::lock_guard lock(mutex_);
        bool alive = false;
        {
            std::lock_guard thread_lock(thread_mutex_);
            alive = thread_.joinable() && !thread_exited_->is_set();
        }
        if (consolidating_ || stop_requested_ || !alive || permit.cancelled()) {
            throw std::runtime_error("The note agent is not available.");
        }
        if (note_requests_.size() >= MAX_QUEUED_NOTE_REQUESTS) {
            throw std::runtime_error("The note agent already has several requests queued.");
        }
        note_requests_.push_back({stripped, std::move(completion), permit});
    }
    wake_.set();
}

void CheckpointScheduler::cancel_note_requests() {
    std::deque<NoteRequest> queued;
    {
        std::lock_guard lock(mutex_);
        queued.swap(note_requests_);
    }
    for (auto& request : queued) {
        AgentResult stopped;
        stopped.error = "Meeting agent stopped.";
        complete_note_request(request.completion, std::move(stopped));
    }
}

void CheckpointScheduler::fire_note_request() {
    NoteRequest request;
    {
        std::lock_guard lock(mutex_);
        if (note_requests_.empty()) {
            return;
        }
        request = std::move(note_requests_.front());
        note_requests_.pop_front();
    }
    AgentResult result;
    try {
        if (stop_requested_ || consolidating_ || request.permit.cancelled()) {
            throw std::runtime_error("Meeting agent stopped.");
        }
        if (!agent_->is_healthy()) {
            throw std::runtime_error("The note agent is offline.");
        }
        // Requests may target any existing block, including older notes: the automatic
        // transcript watermarks do not move for this pass.
        const auto segments = host_.get_transcript(-1.0);
        auto payload = build_payload(segments, false, false, true);
        if (!payload) {
            throw std::runtime_error("No active meeting notes.");
        }
        payload->state_snapshot["note_adjustment_request"] = request.text;
        if (request.permit.cancelled()) {
            throw std::runtime_error("Meeting agent stopped.");
        }
        result = agent_->checkpoint(*payload);
    } catch (const std::exception& error) {
        scheduler_log.error_with(error, "Note adjustment request failed");
        result = AgentResult{};
        result.error = error.what();
    } catch (...) {
        // Python catches every Exception here: the requester always hears back and the worker
        // keeps serving.
        scheduler_log.error("Note adjustment request failed");
        result = AgentResult{};
        result.error = "unknown error";
    }
    complete_note_request(request.completion, std::move(result));
}

void CheckpointScheduler::notify_guidance() {
    {
        std::lock_guard lock(mutex_);
        guidance_pending_ = true;
        notes_guidance_pending_ = true;
        ++guidance_generation_;
        // Fresh human input gets one immediate attempt even when an earlier automatic pass
        // entered provider backoff.
        retry_not_before_ = 0.0;
        notes_retry_not_before_ = 0.0;
        pending_segments_ += 1;
    }
    wake_.set();
}

double CheckpointScheduler::interval_for(int pending) const {
    if (pending >= PRESSURE_HIGH) {
        return min_interval_s_;
    }
    if (pending < PRESSURE_LOW) {
        return max_interval_s_;
    }
    return base_interval_s_;
}

void CheckpointScheduler::run_loop(std::shared_ptr<agents::Event> exited) {
    while (!stop_requested_) {
        wake_.wait(std::chrono::duration<double>(options_.tick_s));
        wake_.clear();
        if (stop_requested_) {
            break;
        }
        try {
            run_due();
        } catch (const std::exception& error) {
            // An uncaught error ends the worker, as it ends Python's thread.
            scheduler_log.error_with(error, "Checkpoint scheduler loop raised");
            break;
        } catch (...) {
            scheduler_log.error("Checkpoint scheduler loop raised");
            break;
        }
    }
    scheduler_log.debug("Checkpoint scheduler loop exited");
    exited->set();
}

void CheckpointScheduler::run_due() {
    if (stop_requested_ || consolidating_) {
        return;
    }
    fire_note_request();
    if (stop_requested_ || consolidating_) {
        return;
    }
    // Pending segments and guidance stay intact during the warm-up. The meeting clock excludes
    // pauses and survives scheduler restarts.
    if (meeting_elapsed_s() < INITIAL_CONTEXT_S) {
        return;
    }
    int pending = 0;
    {
        std::lock_guard lock(mutex_);
        if (!note_requests_.empty()) {
            wake_.set();
            return;
        }
        pending = pending_segments_;
    }
    if (pending <= 0) {
        maybe_fire_notes();
        maybe_fire_polish();
        return;
    }
    if (checkpoint_due(pending)) {
        fire();
    }
}

double CheckpointScheduler::meeting_elapsed_s() const {
    if (host_.has_meeting_clock()) {
        return host_.meeting_clock_s();
    }
    return mono() - started_mono_;
}

bool CheckpointScheduler::checkpoint_due(int pending) {
    const double elapsed = mono() - last_fire_mono_;
    bool guidance = false;
    {
        std::lock_guard lock(mutex_);
        if (mono() < retry_not_before_) {
            return false;
        }
        guidance = guidance_pending_;
    }
    if (guidance) {
        return true;
    }
    if (sent_starts_.empty()) {
        return true;
    }
    if (elapsed < min_interval_s_) {
        return false;
    }
    return elapsed >= interval_for(pending) || detect_topic_shift();
}

std::optional<CheckpointPayload>
CheckpointScheduler::build_payload(const std::vector<Json>& segments, bool is_consolidation,
                                   bool is_polish, bool is_notes) {
    const auto store = host_.store();
    if (!store) {
        return std::nullopt;
    }
    Json snapshot = store->snapshot();
    if (!(is_consolidation || is_polish || is_notes)) {
        // Exact earlier lines travel as context without advancing their delivery cursor again:
        // later speech can disambiguate a name the speech model misheard.
        std::set<std::string> supplied;
        for (const auto& segment : segments) {
            supplied.insert(detail::get(segment, "id").dump());
        }
        std::vector<Json> recent;
        try {
            recent = host_.get_transcript(fetch_cursor_s());
        } catch (const std::exception& error) {
            scheduler_log.error_with(error, "Recent transcript context fetch failed");
            recent.clear();
        }
        std::vector<Json> context;
        for (auto& segment : recent) {
            if (!supplied.contains(detail::get(segment, "id").dump())) {
                context.push_back(std::move(segment));
            }
        }
        Json tail = Json::array();
        const std::size_t first =
            context.size() > RECENT_CONTEXT_SEGMENTS ? context.size() - RECENT_CONTEXT_SEGMENTS : 0;
        for (std::size_t i = first; i < context.size(); ++i) {
            tail.push_back(std::move(context[i]));
        }
        snapshot["recent_transcript_context"] = std::move(tail);
    }
    CheckpointPayload payload;
    payload.request_id = uuid4_hex();
    payload.state_snapshot = std::move(snapshot);
    payload.new_segments = segments;
    payload.is_consolidation = is_consolidation;
    payload.is_polish = is_polish;
    payload.is_notes = is_notes;
    return payload;
}

void CheckpointScheduler::fire() {
    int claimed = 0;
    bool guidance = false;
    {
        std::lock_guard lock(mutex_);
        claimed = pending_segments_;
        pending_segments_ = 0;
        guidance = guidance_pending_;
        guidance_pending_ = false;
    }
    // The fire time is the start of the run, so work that becomes due while the checkpoint runs
    // fires right after it.
    last_fire_mono_ = mono();

    Fetch fetched;
    try {
        fetched = fetch_for_checkpoint(guidance);
    } catch (const std::exception& error) {
        scheduler_log.error_with(error, "Checkpoint transcript fetch failed");
        checkpoint_failed(claimed, guidance, error.what());
        return;
    }
    const auto segments = checkpoint_segments(fetched.segments, guidance);
    if (segments.empty() && !guidance) {
        return;
    }

    // Only the agent can judge which speech deserves a capture: seeding cards with transcript
    // snippets would promote filler into insights before the model read the context.
    auto payload = build_payload(segments, false);
    if (!payload) {
        if (guidance) {
            notify_guidance();
        }
        std::lock_guard lock(mutex_);
        pending_segments_ += claimed;
        return;
    }
    scheduler_log.info("Firing checkpoint {} ({} segments, {} notified, consecutive_failures={})",
                       payload->request_id, segments.size(), claimed, consecutive_failures_);
    const AgentResult result = run_agent_pass(*payload, "Agent checkpoint raised");
    if (!result.ok) {
        checkpoint_failed(claimed, guidance,
                          result.error.empty() ? std::string("checkpoint failed") : result.error,
                          payload->request_id);
        return;
    }

    if (delivery_enabled_ && !guidance) {
        try {
            host_.advance_delivery_cursor("cards", fetched.seq);
        } catch (const std::exception& error) {
            scheduler_log.error_with(error, "Could not save checkpoint transcript cursor");
            record_failure(claimed, error.what(), payload->request_id);
            return;
        }
        card_cursor_seq_ = fetched.seq;
    }
    record_checkpoint_success(payload->request_id, segments, result);
    maybe_fire_notes();
    maybe_fire_polish();
    if (fetched.more) {
        notify_segments(1);
    }
}

AgentResult CheckpointScheduler::run_agent_pass(const CheckpointPayload& payload,
                                                std::string_view failure_log) {
    try {
        return agent_->checkpoint(payload);
    } catch (const std::exception& error) {
        scheduler_log.error_with(error, failure_log);
        AgentResult failed;
        failed.error = error.what();
        return failed;
    } catch (...) {
        // Any exception is a failed pass, as in Python, never the end of the worker.
        scheduler_log.error("{}", failure_log);
        AgentResult failed;
        failed.error = "unknown error";
        return failed;
    }
}

bool CheckpointScheduler::background_blocked() const {
    return consolidating_ || stop_requested_ || interactive_pending();
}

bool CheckpointScheduler::interactive_pending() const {
    std::lock_guard lock(mutex_);
    return !note_requests_.empty() || guidance_pending_;
}

void CheckpointScheduler::set_online(bool online, const std::string& error) {
    if (online == online_.load()) {
        return;
    }
    online_ = online;
    if (online) {
        consecutive_failures_ = 0;
        std::lock_guard lock(mutex_);
        retry_not_before_ = 0.0;
    }
    if (on_health_) {
        try {
            on_health_(online, error);
        } catch (const std::exception& raised) {
            scheduler_log.error_with(raised, "Scheduler on_health callback raised");
        } catch (...) {
            scheduler_log.error("Scheduler on_health callback raised");
        }
    }
}

bool CheckpointScheduler::online() const {
    return online_;
}

} // namespace fv::meeting_agent
