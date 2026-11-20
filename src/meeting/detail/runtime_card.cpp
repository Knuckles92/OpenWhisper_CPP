// MeetingRuntime: the finalization card on the Meeting tab, Keep for later and background
// finishing (services/runtime/meeting.py _hydrate_finalization_card, _meeting_identity_fields,
// _persist_card_deferred, _hide_finalization_card, continue_in_background,
// _finish_background_meeting, defer_finalization_card, _file_leftover_card,
// _meeting_content_summary, _speaker_rerun_fields). The repository is never read on the UI
// actor: a step that persists runs on a worker and continues on the UI actor.

#include <exception>
#include <format>
#include <tuple>
#include <utility>

#include "meeting/detail/py_value.h"
#include "meeting/detail/runtime_internal.h"
#include "meeting/rerun_options.h"
#include "meeting/runtime.h"
#include "meeting/runtime_messages.h"
#include "meeting_state/content.h"
#include "meeting_state/schema.h"
#include "meeting_state/stored.h"
#include "pyport/text.h"

namespace fv::meeting {

namespace {

const log::Logger& logger = detail::runtime_logger;

} // namespace

bool MeetingRuntime::hydrate_finalization_card(const Json& meeting, bool reveal) {
    const std::string meeting_id = detail::text_of(meeting, "id");
    meeting_state::FinalizationState finalization;
    try {
        finalization = meeting_state::finalization_from_meeting_row(meeting);
    } catch (const std::exception& error) {
        logger.error_with(error,
                          std::format("Could not coerce finalization for meeting {}", meeting_id));
        return false;
    }
    if (finalization.card_deferred && !reveal) {
        return false;
    }
    // A revealed card shows in this session only; the stored flag keeps the next startup idle.
    finalization.card_deferred = false;
    if (finalization.status == "pending" && finalization.steps.empty()) {
        return false;
    }
    Json payload = finalization.to_json();
    payload["content_summary"] =
        meeting_content_summary(meeting_id, std::optional<Json>(std::in_place, meeting));
    {
        const std::lock_guard lock(mutex_);
        if (starting_ || is_active_locked() || controller_meeting_active_ || finalizing_ ||
            background_engines_.contains(meeting_id)) {
            return false;
        }
        background_ready_ = false;
        card_meeting_id_ = meeting_id;
        finalization_.emplace(payload);
        finalizing_ = false;
    }
    Json state = Json::object();
    state["active"] = false;
    state["meeting_id"] = meeting_id;
    const std::string status = detail::text_of(meeting, "status");
    state["status"] = status.empty() ? std::string("ended") : status;
    state["finalization"] = payload;
    state.update(meeting_identity_fields(meeting, &payload));
    meeting_state_changed.emit(state);
    return true;
}

Json MeetingRuntime::meeting_identity_fields(const Json& meeting, const Json* finalization) const {
    Json row = meeting.is_object() ? meeting : Json::object();
    if (finalization != nullptr && finalization->is_object() &&
        finalization->contains("content_summary")) {
        row["content_summary"] = finalization->at("content_summary");
    }
    const auto pill = meeting_state::meeting_insights_pill(row);
    Json fields = Json::object();
    fields["title"] = py::strip(detail::text_of(meeting, "title"));
    fields["display_title"] = meeting_state::fallback_meeting_title(meeting);
    fields["started_at"] = detail::get(meeting, "started_at");
    fields["ended_at"] = detail::get(meeting, "ended_at");
    fields["paused_total_s"] = detail::get(meeting, "paused_total_s");
    fields["insights_pill"] = pill ? pill->first : std::string();
    fields["insights_tone"] = pill ? pill->second : std::string();
    return fields;
}

bool MeetingRuntime::persist_card_deferred(const std::string& meeting_id, bool deferred) {
    std::shared_ptr<RuntimeEngine> engine;
    std::shared_ptr<RuntimeEngine> background;
    {
        const std::lock_guard lock(mutex_);
        engine = engine_;
        if (const auto found = background_engines_.find(meeting_id);
            found != background_engines_.end()) {
            background = found->second;
        }
    }
    if (!engine || engine->meeting_id() != meeting_id) {
        engine = background;
    }
    const auto store = engine ? engine->store() : nullptr;
    if (store && engine->meeting_id() == meeting_id) {
        try {
            auto [current, cloud, status] =
                store->with_state([](const meeting_state::MeetingState& state) {
                    return std::make_tuple(state.finalization, state.cloud_enabled, state.status);
                });
            Json payload = current.to_json();
            payload["card_deferred"] = deferred;
            const auto updated = meeting_state::FinalizationState::coerce(
                payload, cloud, status.empty() ? std::string("ended") : status);
            Json fields = Json::object();
            fields["finalization"] = updated.to_json();
            return store->update_runtime_fields(fields);
        } catch (const std::exception& error) {
            logger.error_with(error, "Could not persist card deferral on the live store");
            return false;
        }
    }
    try {
        const auto meeting = deps_.repository->get_meeting(meeting_id);
        if (!meeting) {
            return false;
        }
        // No live store owns this meeting, so anything the snapshot still marks as running
        // was interrupted.
        meeting_state::MeetingState state =
            meeting_state::load_state(*meeting, meeting_id, true, settings_snapshot(), clock_);
        state.finalization.card_deferred = deferred;
        deps_.repository->persist_state(meeting_id, state.to_json());
        return true;
    } catch (const std::exception& error) {
        logger.error_with(
            error, std::format("Could not persist card deferral for meeting {}", meeting_id));
        return false;
    }
}

void MeetingRuntime::hide_finalization_card() {
    {
        const std::lock_guard lock(mutex_);
        finalization_.reset();
        card_meeting_id_.reset();
    }
    Json state = Json::object();
    state["active"] = false;
    state["finalization"] = nullptr;
    state["meeting_id"] = nullptr;
    meeting_state_changed.emit(state);
    refresh_past_meetings();
}

void MeetingRuntime::continue_in_background() {
    // Detaches the ended meeting once its local transcription model is free.
    std::shared_ptr<RuntimeEngine> engine;
    std::string meeting_id;
    {
        const std::lock_guard lock(mutex_);
        engine = engine_;
        meeting_id = engine ? engine->meeting_id() : std::string();
        if (meeting_id.empty() || !finalizing_ || !background_ready_ || starting_ ||
            is_active_locked() || controller_meeting_active_) {
            return;
        }
    }
    spawn("meeting-background-handoff", [this, engine, meeting_id] {
        const bool persisted = persist_card_deferred(meeting_id, true);
        post_ui([this, engine, meeting_id, persisted] {
            if (!persisted) {
                meeting_error.emit(std::string(messages::kCouldNotMoveToBackground));
                return;
            }
            std::size_t count = 0;
            {
                const std::lock_guard lock(mutex_);
                if (engine_ != engine || !finalizing_ || !background_ready_ || starting_ ||
                    is_active_locked() || controller_meeting_active_) {
                    return;
                }
                background_engines_[meeting_id] = engine;
                if (host_url_ && !host_url_->empty()) {
                    background_host_urls_[meeting_id] = *host_url_;
                }
                engine_.reset();
                host_url_.reset();
                guest_url_.reset();
                finalizing_ = false;
                background_ready_ = false;
                background_workers_.insert(meeting_id);
                count = background_engines_.size();
            }
            sync_activity();
            hide_finalization_card();
            Json state = Json::object();
            state["background_available"] = false;
            state["dashboard_available"] = false;
            state["background_message"] = messages::background_message(count);
            meeting_state_changed.emit(state);
            spawn("meeting-background-" + meeting_id,
                  [this, meeting_id, engine] { finish_background_meeting(meeting_id, engine); });
        });
    });
}

void MeetingRuntime::finish_background_meeting(const std::string& meeting_id,
                                               std::shared_ptr<RuntimeEngine> engine) {
    std::string outcome(messages::kBackgroundFinished);
    try {
        // App exit cancels the wait; shutdown() below then finishes the meeting's End.
        while (!engine->wait_for_end(std::chrono::seconds(1))) {
            if (shutdown_.cancelled()) {
                break;
            }
        }
        // Reasserted after the engine's last write, including a progress update that raced
        // the first deferral.
        const bool deferred = persist_card_deferred(meeting_id, true);
        const auto store = engine->store();
        if (!store) {
            throw std::runtime_error("the background meeting has no state store");
        }
        const Json finalization = store->with_state(
            [](const meeting_state::MeetingState& state) { return state.finalization.to_json(); });
        const std::string status = detail::text_of(finalization, "status");
        bool failed_step = false;
        for (const Json& step : detail::list_or_empty(detail::get(finalization, "steps"))) {
            failed_step = failed_step || detail::get(step, "status") == Json("failed");
        }
        if (!deferred || (status != "completed" && status != "disabled") || failed_step) {
            outcome = messages::kBackgroundAttention;
        }
    } catch (const std::exception& error) {
        logger.error_with(error, std::format("Background meeting {} failed", meeting_id));
        outcome = messages::kBackgroundAttention;
    }
    cancel_engine_work(engine.get());
    try {
        engine->shutdown();
    } catch (const std::exception& error) {
        logger.error_with(error, std::format("Could not close background meeting {}", meeting_id));
    }
    Subscription listener;
    std::size_t count = 0;
    {
        const std::lock_guard lock(mutex_);
        background_engines_.erase(meeting_id);
        background_workers_.erase(meeting_id);
        background_host_urls_.erase(meeting_id);
        count = background_engines_.size();
        if (const auto found = listeners_.find(engine.get()); found != listeners_.end()) {
            listener = std::move(found->second);
            listeners_.erase(found);
        }
    }
    background_done_.notify_all();
    listener.disconnect();
    sync_activity();
    Json state = Json::object();
    state["background_message"] = messages::background_message(count, outcome);
    meeting_state_changed.emit(state);
    refresh_past_meetings();
}

void MeetingRuntime::defer_finalization_card() {
    // Keep for later: the meeting, transcript and audio stay in Past Meetings, and the card
    // stays hidden across restarts until that meeting is selected again.
    bool finalizing = false;
    bool busy = false;
    std::string status;
    std::optional<std::string> meeting_id;
    {
        const std::lock_guard lock(mutex_);
        finalizing = finalizing_;
        status = finalization_ ? detail::text_of(*finalization_, "status") : std::string();
        meeting_id = card_meeting_id_;
        busy = starting_ || controller_meeting_active_ || is_active_locked();
    }
    if (finalizing || status == "running") {
        meeting_status_update.emit(std::string(messages::kFinalInsightsPreparing));
        return;
    }
    if (busy) {
        meeting_status_update.emit(std::string(messages::kAlreadyInProgress));
        return;
    }
    if (!meeting_id || meeting_id->empty()) {
        logger.warning("Cannot defer finalization card: no meeting selected");
        return;
    }
    spawn("meeting-card-defer", [this, id = *meeting_id, status] {
        const bool persisted = persist_card_deferred(id, true);
        post_ui([this, persisted, status] {
            if (!persisted) {
                meeting_error.emit(std::string(messages::kCouldNotSaveForLater));
                return;
            }
            hide_finalization_card();
            meeting_status_update.emit(std::string(status == "completed" || status == "disabled"
                                                       ? messages::kSaved
                                                       : messages::kSavedForLater));
        });
    });
}

void MeetingRuntime::file_leftover_card(Task then) {
    std::optional<std::string> meeting_id;
    std::string status;
    {
        const std::lock_guard lock(mutex_);
        meeting_id = card_meeting_id_;
        status = finalization_ ? detail::text_of(*finalization_, "status") : std::string();
    }
    if (!meeting_id || meeting_id->empty() || status.empty() || status == "running") {
        then();
        return;
    }
    spawn("meeting-file-card", [this, id = *meeting_id, then = std::move(then)]() mutable {
        if (!persist_card_deferred(id, true)) {
            logger.warning("Could not persist leftover meeting card deferral for {}", id);
        }
        post_ui([this, then = std::move(then)] {
            hide_finalization_card();
            then();
        });
    });
}

Json MeetingRuntime::meeting_content_summary(const std::string& meeting_id,
                                             const std::optional<Json>& meeting) {
    if (meeting_id.empty()) {
        return Json::object();
    }
    Json summary = meeting_state::summarize_meeting_content(*deps_.repository, meeting_id);
    std::optional<Json> row = meeting;
    if (!row) {
        try {
            row = deps_.repository->get_meeting(meeting_id);
        } catch (const std::exception& error) {
            logger.error_with(error, "Could not read meeting status for content summary");
        }
    }
    summary["meeting_status"] = row ? detail::text_of(*row, "status") : std::string();
    summary.update(speaker_rerun_fields());
    return summary;
}

Json MeetingRuntime::speaker_rerun_fields() const {
    // Only the button's look depends on this; the retry applies the same gate before anything
    // is uploaded.
    Json fields = Json::object();
    try {
        const SpeakerPassGate gate = resolve_speaker_pass(settings_snapshot(), clock_);
        fields["speaker_rerun_offered"] = gate.offered;
        fields["speaker_rerun_reason"] = gate.ok ? std::string() : gate.reason;
    } catch (const std::exception& error) {
        logger.error_with(error, "Could not check whether speakers can be re-run");
        fields["speaker_rerun_offered"] = false;
        fields["speaker_rerun_reason"] = std::string(messages::kSpeakerIdUnavailable);
    }
    return fields;
}

} // namespace fv::meeting
