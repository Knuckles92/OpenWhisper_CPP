#include "meeting/engine.h"

#include <algorithm>
#include <regex>
#include <stdexcept>
#include <utility>

#include "meeting/detail/engine_internal.h"
#include "meeting/detail/py_value.h"
#include "meeting_agent/interfaces.h"
#include "meeting_agent/scheduler.h"
#include "meeting_agent/typesafe_signals.h"
#include "meeting_intel/intel_settings.h"
#include "pyport/text.h"

namespace fv::meeting {
namespace {

Json destination_fields(const meeting_agent::MeetingAgentRoute& route) {
    return {
        {"agent_provider", route.provider},
        {"agent_model", route.model},
        {"agent_endpoint_json", route.endpoint.is_null() ? Json() : Json(route.endpoint.dump())}};
}

std::string agent_failure_text(const std::string& error) {
    static const std::regex rejected(R"(\b40[13]\b)");
    std::smatch match;
    if (std::regex_search(error, match, rejected)) {
        return "The AI provider rejected the API key (" + match.str() +
               "). Check it in Settings → Meeting Mode → Intelligence.";
    }
    return error.empty() ? std::string{} : "Last error: " + py::slice(error, 0, 200);
}

// engine.py set_cloud_enabled once End claimed the meeting, or outside active and paused.
constexpr std::string_view kInsightsChangeRefused =
    "AI insights can only be changed while the meeting is active or paused.";
// engine.py _report_intelligence_unusable: a core that initialized without raising and still
// cannot take checkpoints (no API key, the agent already gone).
constexpr std::string_view kIntelligenceUnusable =
    "The meeting-intelligence backend is unavailable (no API key or backend); continuing "
    "transcript-only.";

class UnusableCore final : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

void require_healthy_core(const std::shared_ptr<meeting_agent::AgentCore>& core) {
    if (!core) {
        throw std::runtime_error("The meeting agent is not available.");
    }
    if (!core->is_healthy()) {
        // Released, so a later retry builds a fresh one instead of reusing the dead instance.
        core->shutdown();
        throw UnusableCore(std::string(kIntelligenceUnusable));
    }
}

// What a failed start publishes: Python's str(exc) (or the unusable core's reason) and the
// finalization card's sentence (engine.py _unwind_intelligence_start,
// _report_intelligence_unusable).
struct StartFailure {
    std::string error;
    std::string card;
    bool unusable = false;
};

StartFailure start_failure(const std::exception& error) {
    const std::string text = error.what();
    if (dynamic_cast<const UnusableCore*>(&error) != nullptr) {
        return {text, text, true};
    }
    return {text, "Meeting intelligence failed to start: " + text, false};
}

} // namespace

namespace detail {

class LiveMeetingAgent final : public meeting_agent::SchedulerHost {
public:
    LiveMeetingAgent(MeetingEngine& engine, std::shared_ptr<meeting_agent::AgentCore> core,
                     const Clock& clock, std::function<void(bool, std::string)> health,
                     meeting_agent::TopicJudge topic_judge)
        : engine_(engine), core_(std::move(core)), topic_judge_(topic_judge),
          scheduler_(*this, core_, clock, std::move(health), std::move(topic_judge)) {}

    void start() { scheduler_.start(); }

    bool core_healthy() const { return core_->is_healthy(); }
    // The judge handed to the scheduler (engine.py passes it as topic_judge=).
    const meeting_agent::TopicJudge& topic_judge() const { return topic_judge_; }

    void stop() {
        scheduler_.prepare_for_end();
        try {
            core_->cancel();
        } catch (const std::exception&) {
            engine_logger.warning("Could not cancel the live meeting agent.");
        }
        scheduler_.stop();
        core_->set_activity_callback({});
        try {
            core_->shutdown();
        } catch (const std::exception&) {
            engine_logger.warning("Could not shut down the live meeting agent.");
        }
    }

    void notify_segments(int count) { scheduler_.notify_segments(count); }
    void notify_guidance() { scheduler_.notify_guidance(); }
    void request_note_adjustment(std::string text,
                                 std::function<void(meeting_agent::AgentResult)> completion,
                                 const CancelToken& permit) {
        scheduler_.request_note_adjustment(std::move(text), std::move(completion), permit);
    }
    void stop_accepting_work() { scheduler_.prepare_for_end(); }

    std::shared_ptr<meeting_state::MeetingStateStore> store() override { return engine_.store(); }
    std::vector<Json> get_transcript(double after) override {
        return engine_.get_transcript(after);
    }
    double meeting_clock_s() const override { return engine_.clock().now_s(); }
    std::vector<Json> delivery_page(std::int64_t after, int limit) override {
        return engine_.get_transcript_delivery_page(after, limit);
    }
    std::int64_t delivery_cursor(std::string_view consumer) override {
        return engine_.get_transcript_delivery_cursor(consumer);
    }
    void advance_delivery_cursor(std::string_view consumer, std::int64_t seq) override {
        engine_.advance_transcript_delivery_cursor(consumer, seq);
    }
    std::int64_t delivery_high_water() override {
        return engine_.get_transcript_delivery_high_water();
    }
    void revoke_agent_writes() override { engine_.revoke_agent_writes(); }

private:
    MeetingEngine& engine_;
    std::shared_ptr<meeting_agent::AgentCore> core_;
    // Before scheduler_, which is built from a copy of it.
    meeting_agent::TopicJudge topic_judge_;
    meeting_agent::CheckpointScheduler scheduler_;
};

} // namespace detail

void MeetingEngine::start_live_intelligence(
    std::optional<meeting_agent::MeetingAgentRoute> takeover, const CancelToken& permit,
    std::string previous_failure) {
    const auto current = store();
    if (!current || !intelligence_->available() ||
        !current->with_state([](const auto& state) { return state.cloud_enabled; })) {
        return;
    }
    CancelToken token;
    std::optional<meeting_agent::MeetingAgentRoute> route;
    {
        const std::lock_guard lock(state_mutex_);
        if (live_intelligence_ || starting_live_intelligence_ ||
            live_intelligence_cancel_.cancelled() || permit.cancelled() ||
            processing_stop_.load()) {
            return;
        }
        starting_live_intelligence_ = true;
        token = live_intelligence_cancel_.token();
        route = takeover ? takeover : live_approved_route_;
    }
    // A start that could not bring a usable core up: the meeting goes on transcript-only.
    const auto report_failed = [&](const StartFailure& failure) {
        {
            const std::lock_guard lock(state_mutex_);
            if (!token.cancelled()) {
                starting_live_intelligence_ = false;
            }
        }
        if (processing_stop_.load() || token.cancelled()) {
            return;
        }
        IntelligenceReport report;
        report.error = failure.error;
        if (previous_failure.empty()) {
            report.card = failure.card;
            report.note = failure.unusable ? failure.error : std::string();
        } else {
            report.card = py::strip(previous_failure + " The backup agent could not take over. " +
                                    failure.card);
            // End's offline message keeps naming why insights left the first agent.
            report.failure = previous_failure;
        }
        report_live_intelligence(std::move(report), token);
    };
    try {
        RerunContext context;
        context.meeting_id = meeting_id();
        context.repository = deps_.repository;
        context.store = store();
        context.meeting = context.repository->get_meeting(context.meeting_id).value_or(Json());
        context.approved_route = route;
        context.writes_allowed = [this, token] {
            return !token.cancelled() && agent_writes_allowed();
        };
        // engine.py _switch_to_backup_agent: re-runs and reports stay with the agent that wrote
        // the record, so the backup is recorded before it is tried.
        const auto record_backup = [&](const meeting_agent::MeetingAgentRoute& backup) {
            try {
                deps_.repository->update_meeting(context.meeting_id, destination_fields(backup));
            } catch (const std::exception& error) {
                detail::engine_logger.error_with(error, "Failed to record the backup agent");
            }
        };
        if (takeover) {
            record_backup(*takeover);
        }
        std::shared_ptr<meeting_agent::AgentCore> core;
        StartFailure primary_failure{"The meeting agent is not available.",
                                     "Meeting intelligence failed to start: The meeting agent is "
                                     "not available."};
        try {
            core = intelligence_->create_live_core(context, token);
            if (!token.cancelled()) {
                require_healthy_core(core);
            }
        } catch (const std::exception& error) {
            core.reset();
            primary_failure = start_failure(error);
        }
        auto backup = takeover;
        if (!core) {
            const std::lock_guard lock(state_mutex_);
            if (!token.cancelled() && !processing_stop_.load() && !tried_backup_ && route &&
                approved_backup_ && approved_backup_->installed() && approved_backup_ != route) {
                tried_backup_ = true;
                backup = approved_backup_;
            }
        }
        if (backup && !takeover) {
            previous_failure = primary_failure.error;
            record_backup(*backup);
            context.approved_route = backup;
            core = intelligence_->create_live_core(context, token);
            if (!token.cancelled()) {
                require_healthy_core(core);
            }
        }
        if (core && backup) {
            const std::lock_guard lock(state_mutex_);
            if (!token.cancelled() && !processing_stop_.load()) {
                live_approved_route_ = backup;
            }
        }
        if (!core) {
            // A cancelled start reports nothing: its meeting ended or its consent was revoked.
            report_failed(primary_failure);
            return;
        }
        if (token.cancelled()) {
            core->shutdown();
            return;
        }
        core->set_activity_callback([this, token](const meeting_agent::AgentActivity& activity) {
            auto message = activity.to_json();
            if (activity.kind.empty()) {
                message["kind"] = "update";
            }
            if (activity.ts.empty()) {
                message["ts"] = utc_now();
            }
            {
                const std::lock_guard lock(state_mutex_);
                if (token.cancelled()) {
                    return;
                }
                // engine.py AGENT_ACTIVITY_MIN_INTERVAL_S: one tick per (kind, tool) a second.
                constexpr auto kMinInterval = std::chrono::seconds(1);
                const auto now = clock_.steady();
                const std::pair key{message["kind"].get<std::string>(), activity.tool};
                const auto last = agent_activity_last_.find(key);
                if (last != agent_activity_last_.end() && now - last->second < kMinInterval) {
                    return;
                }
                agent_activity_last_[key] = now;
                agent_activity_.push_back(message);
                // engine.py AGENT_ACTIVITY_HISTORY: a host hello carries the last 50 ticks.
                constexpr std::size_t kRecentActivity = 50;
                if (agent_activity_.size() > kRecentActivity) {
                    agent_activity_.pop_front();
                }
            }
            message["type"] = "agent_activity";
            broadcast(message, true);
        });
        auto topic_judge = [this, token](const std::string& previous,
                                         const std::string& window) -> std::optional<double> {
            if (token.cancelled() || !deps_.settings || !deps_.local_fast_judge ||
                !meeting_intel::resolve_typesafe_topic_shift_enabled(deps_.settings())) {
                return std::nullopt;
            }
            const auto judge = deps_.local_fast_judge();
            if (!judge || !judge->is_local()) {
                return std::nullopt;
            }
            const auto result =
                meeting_agent::topic_shift_probability(*judge, previous, window, token);
            // A result arriving after topic shifts were switched off is discarded (engine.py
            // _typesafe_topic_judge).
            if (token.cancelled() ||
                !meeting_intel::resolve_typesafe_topic_shift_enabled(deps_.settings())) {
                return std::nullopt;
            }
            return result;
        };
        auto session = std::make_shared<detail::LiveMeetingAgent>(
            *this, std::move(core), clock_,
            [this, token](bool online, std::string error) {
                if (!processing_stop_.load() && !token.cancelled()) {
                    const std::string failure = agent_failure_text(error);
                    IntelligenceReport report;
                    report.online = online;
                    if (!online) {
                        report.error = py::strip(
                            "Checkpoints failed repeatedly; continuing transcript-only. " +
                            failure);
                        report.failure = failure;
                    }
                    report_live_intelligence(std::move(report), token);
                    if (!online) {
                        schedule_backup_after_checkpoint_failure(token, failure);
                    }
                }
            },
            std::move(topic_judge));
        session->start();
        bool adopted = false;
        {
            const std::lock_guard lock(state_mutex_);
            if (!processing_stop_.load() && !token.cancelled()) {
                starting_live_intelligence_ = false;
                live_intelligence_ = session;
                adopted = true;
            }
        }
        if (!adopted) {
            session->stop();
        } else {
            // The live ledger may have received a row after start() checked its backlog and
            // before ASR could see the new scheduler. A nudge rechecks the durable ledger.
            session->notify_segments(1);
            const std::string message =
                backup ? py::strip("Your backup agent took over AI insights. " + previous_failure)
                       : std::string{};
            report_live_intelligence({.online = true, .card = message, .note = message}, token);
        }
    } catch (const std::exception& error) {
        report_failed(start_failure(error));
    }
}

void MeetingEngine::schedule_backup_after_checkpoint_failure(const CancelToken& current,
                                                             std::string failure) {
    const std::scoped_lock lock(lifecycle_mutex_, state_mutex_);
    if (current.cancelled() || !active_.load() || processing_stop_.load() || tried_backup_ ||
        !live_intelligence_ || !approved_backup_ || !approved_backup_->installed() ||
        !live_approved_route_ || approved_backup_ == live_approved_route_) {
        return;
    }
    const auto backup = *approved_backup_;
    // This callback runs on the failing scheduler. A separate worker can retire and join it.
    backup_switch_.thread = std::thread([this, backup, current, failure = std::move(failure)] {
        try {
            std::shared_ptr<detail::LiveMeetingAgent> retired;
            CancelSource previous;
            CancelToken permit;
            {
                const std::lock_guard transition(intelligence_toggle_mutex_);
                const std::lock_guard lock(state_mutex_);
                if (!current.cancelled() && !processing_stop_.load()) {
                    retired = std::move(live_intelligence_);
                    previous = live_intelligence_cancel_;
                    live_intelligence_cancel_ = CancelSource(processing_cancel_.token());
                    permit = live_intelligence_cancel_.token();
                }
            }
            if (retired) {
                previous.cancel();
                retired->stop();
                // Both schedulers use the same committed-delivery ledger. Only rows that the
                // primary successfully delivered remain behind its cursors.
                start_live_intelligence(backup, permit, failure);
            }
        } catch (const std::exception&) {
            detail::engine_logger.warning("Could not start the approved backup meeting agent.");
        }
        set_worker_done(backup_switch_);
    });
    tried_backup_ = true;
    backup_switch_.started = true;
    backup_switch_.done = false;
    backup_switch_.id = backup_switch_.thread.get_id();
}

MeetingEngine::TopicJudgeFn MeetingEngine::attached_topic_judge() const {
    const std::lock_guard lock(state_mutex_);
    return live_intelligence_ ? live_intelligence_->topic_judge() : TopicJudgeFn{};
}

MeetingEngine::LiveIntelligenceAtEnd MeetingEngine::live_intelligence_at_end() const {
    std::shared_ptr<detail::LiveMeetingAgent> session;
    {
        const std::lock_guard lock(state_mutex_);
        session = live_intelligence_;
    }
    // No adopted session: the agent never came online, its start was still running when End
    // cancelled it, or a backup could not take over a failed one.
    if (!session) {
        return {};
    }
    try {
        return {.online = true, .healthy = session->core_healthy()};
    } catch (const std::exception&) {
        detail::engine_logger.warning("Could not check the live meeting agent before End.");
        return {.online = true, .healthy = false};
    }
}

void MeetingEngine::stop_live_intelligence() {
    std::shared_ptr<detail::LiveMeetingAgent> session;
    {
        const std::lock_guard lock(state_mutex_);
        session = std::move(live_intelligence_);
    }
    if (session) {
        session->stop();
        report_live_intelligence({});
    }
}

void MeetingEngine::notify_intelligence_segments(int count) {
    std::shared_ptr<detail::LiveMeetingAgent> session;
    {
        const std::lock_guard lock(state_mutex_);
        session = live_intelligence_;
    }
    if (session) {
        session->notify_segments(count);
    }
}

void MeetingEngine::notify_intelligence_guidance(const std::vector<meeting_state::OpResult>& ops) {
    if (std::ranges::none_of(ops, [](const auto& op) {
            return op.ok && (detail::get(op.effect, "entity") == "intent" ||
                             detail::get(detail::get(op.effect, "item"), "card") == "user_notes");
        })) {
        return;
    }
    std::shared_ptr<detail::LiveMeetingAgent> session;
    {
        const std::lock_guard lock(state_mutex_);
        session = live_intelligence_;
    }
    if (session) {
        ++guidance_notices_;
        session->notify_guidance();
    }
}

void MeetingEngine::report_live_intelligence(IntelligenceReport report, const CancelToken& token) {
    const bool online = report.online;
    {
        const std::lock_guard lock(state_mutex_);
        if (token.cancelled()) {
            return;
        }
        if (online) {
            intelligence_failure_.clear();
        } else if (report.failure) {
            intelligence_failure_ = *report.failure;
        } else if (!report.error.empty()) {
            intelligence_failure_ = report.error;
        }
        if (store_) {
            try {
                Json fields{{"intelligence_online", online}};
                const std::string& card =
                    online || !report.card.empty() ? report.card : report.error;
                if (active_.load() && !processing_stop_.load() && (online || !card.empty())) {
                    fields["finalization"] =
                        Json{{"status", online ? "pending" : "unavailable"}, {"message", card}};
                }
                store_->update_runtime_fields(fields);
            } catch (const std::exception&) {
                detail::engine_logger.warning("Could not save meeting intelligence status.");
            }
        }
    }
    emit("intelligence", Json{{"online", online}, {"error", std::move(report.error)}});
    emit_status(report.note);
}

Json MeetingEngine::recent_agent_activity() const {
    const std::lock_guard lock(state_mutex_);
    Json result = Json::array();
    for (const auto& item : agent_activity_) {
        result.push_back(item);
    }
    return result;
}

std::optional<meeting_agent::MeetingAgentRoute>
MeetingEngine::live_intelligence_destination() const {
    if (!active_.load() || processing_stop_.load()) {
        return std::nullopt;
    }
    return intelligence_->live_destination();
}

CancelSource MeetingEngine::final_insights_cancel() const {
    const std::lock_guard lock(state_mutex_);
    return final_insights_cancel_;
}

void MeetingEngine::enable_live_intelligence(const meeting_agent::MeetingAgentRoute& route,
                                             std::optional<meeting_agent::MeetingAgentRoute> backup,
                                             const CancelToken& permit) {
    {
        const std::lock_guard transition(intelligence_toggle_mutex_);
        if (permit.cancelled()) {
            throw meeting_web::EngineRefusal("The AI insights request was stopped.");
        }
        if (!intelligence_->authorize_live_destination(route)) {
            throw meeting_web::EngineRefusal("Approve the current AI insights destination first.");
        }
        const std::lock_guard lifecycle(lifecycle_mutex_);
        const auto current = store();
        if (!current || !active_.load() || processing_stop_.load()) {
            throw meeting_web::EngineRefusal(std::string(kInsightsChangeRefused));
        }
        if (current->with_state([](const auto& state) { return state.cloud_enabled; })) {
            return;
        }
        // Resume automatic checkpoints after this point in the delivery ledger. Existing
        // transcript remains available to the newly authorized agent's explicit read tools.
        const auto cursor = get_transcript_delivery_high_water();
        advance_transcript_delivery_cursor("cards", cursor);
        advance_transcript_delivery_cursor("notes", cursor);
        deps_.repository->update_meeting(current->meeting_id(), destination_fields(route));
        {
            const std::lock_guard lock(state_mutex_);
            if (!current->update_runtime_fields(
                    Json{{"cloud_enabled", true}, {"finalization", {{"status", "pending"}}}})) {
                throw meeting_web::EngineRefusal("Could not save the AI insights setting.");
            }
            live_approved_route_ = route;
            approved_backup_ = std::move(backup);
            live_intelligence_cancel_ = CancelSource(processing_cancel_.token());
            final_insights_cancel_ = CancelSource();
            starting_live_intelligence_ = false;
            allow_agent_writes();
        }
    }
    start_live_intelligence();
    emit_status();
}

void MeetingEngine::request_note_adjustment(std::string text,
                                            std::function<void(Json)> completion) {
    std::shared_ptr<detail::LiveMeetingAgent> session;
    CancelToken permit;
    {
        const std::lock_guard lock(state_mutex_);
        session = live_intelligence_;
        permit = live_intelligence_cancel_.token();
    }
    const auto current = store();
    if (!session || permit.cancelled() || !active_.load() || processing_stop_.load() || !current ||
        !current->with_state([](const auto& state) { return state.cloud_enabled; })) {
        throw meeting_web::EngineRefusal("Turn on AI insights in an active meeting first.");
    }
    try {
        session->request_note_adjustment(
            std::move(text),
            [permit, completion = std::move(completion)](meeting_agent::AgentResult result) {
                if (!completion) {
                    return;
                }
                const auto accepted =
                    std::ranges::count_if(result.op_results, [](const auto& op) { return op.ok; });
                const auto applied = permit.cancelled() ? 0 : accepted;
                const bool ok = !permit.cancelled() && result.ok;
                Json error;
                if (!ok) {
                    error = permit.cancelled() ? "Meeting agent stopped."
                                               : "The note agent could not finish the request.";
                }
                completion(
                    Json{{"ok", ok},
                         {"applied", applied},
                         {"rejected", result.op_results.size() - static_cast<std::size_t>(applied)},
                         {"error", error}});
            },
            permit);
    } catch (const std::runtime_error& error) {
        throw meeting_web::EngineRefusal(error.what());
    }
}

void MeetingEngine::set_cloud_enabled(bool enabled) {
    const auto current = store();
    if (!current) {
        return;
    }
    {
        const std::lock_guard lifecycle(lifecycle_mutex_);
        if (end_claimed_ || !active_.load()) {
            throw meeting_web::EngineRefusal(std::string(kInsightsChangeRefused));
        }
    }
    const auto status = current->with_state([](const auto& state) { return state.status; });
    if (status != "active" && status != "paused") {
        throw meeting_web::EngineRefusal(std::string(kInsightsChangeRefused));
    }
    if (enabled) {
        throw meeting_web::EngineRefusal(
            "Start the meeting with AI insights to authorize its destination.");
    }
    const std::lock_guard transition(intelligence_toggle_mutex_);
    std::optional<std::string> me;
    {
        const std::lock_guard lock(state_mutex_);
        me = me_participant_id_;
    }
    // A host op, as engine.py: the change is audited and a refusal names its reason.
    const auto results = current->apply(
        "host", me, Json::array({Json{{"op", "set_cloud_enabled"}, {"enabled", false}}}));
    if (results.empty() || !results.front().ok) {
        throw meeting_web::EngineRefusal(
            results.empty() ? std::string("cloud state update failed")
                            : results.front().reason.value_or("cloud state update failed"));
    }
    CancelSource live;
    CancelSource final;
    std::shared_ptr<detail::LiveMeetingAgent> session;
    {
        const std::lock_guard lock(state_mutex_);
        live = live_intelligence_cancel_;
        final = final_insights_cancel_;
        session = live_intelligence_;
    }
    live.cancel();
    if (session) {
        session->stop_accepting_work();
    }
    final.cancel();
    revoke_agent_writes();
    {
        const std::lock_guard lock(state_mutex_);
        agent_activity_.clear();
    }
    stop_live_intelligence();
    // engine.py _stop_intelligence: the card of a live meeting says so at once.
    if (!current->update_runtime_fields(
            Json{{"intelligence_online", false},
                 {"finalization",
                  {{"status", "disabled"}, {"message", std::string(kInsightsOffMessage)}}}})) {
        throw meeting_web::EngineRefusal(
            "AI insights stopped, but the setting could not be saved.");
    }
    deps_.repository->update_meeting(current->meeting_id(), Json{{"cloud_enabled", false}});
    // _stop_intelligence emits the status (and the highlights status refreshes with it).
    emit_status();
}

} // namespace fv::meeting
