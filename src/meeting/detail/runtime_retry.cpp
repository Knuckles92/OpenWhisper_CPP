// MeetingRuntime: retrying a meeting's post-meeting steps from the desktop, and the re-run passes
// the dashboards call (services/runtime/meeting.py retry_finalization, _retry_refused,
// _claim_retry, _newest_meeting_id, _retry_worker, _rerun_finalization, _finish_retry,
// _stored_finalization, _publish_finalization; meeting/web/api.py's rerun jobs).
//
// Each retry takes a generation and a FinalizationClaim. Progress is published only while the
// claim is active, a stale generation's events are dropped, and the claim is held until the
// outcome is published, so an old retry can never finish a new one.

#include <algorithm>
#include <exception>
#include <format>
#include <future>
#include <utility>

#include "appcore/activity.h"
#include "llm/profiles.h"
#include "meeting/detail/py_value.h"
#include "meeting/detail/runtime_internal.h"
#include "meeting/rerun_options.h"
#include "meeting/runtime.h"
#include "meeting/runtime_messages.h"
#include "meeting_intel/history_source.h"
#include "meeting_intel/insight_review.h"
#include "meeting_intel/intel_settings.h"
#include "meeting_intel/semantic_search.h"
#include "meeting_state/schema.h"
#include "meeting_state/stored.h"
#include "pyport/errors.h"
#include "pyport/text.h"
#include "settings/store.h"

namespace fv::meeting {

namespace {

const log::Logger& logger = detail::runtime_logger;

Json running_retry() {
    Json finalization = Json::object();
    finalization["status"] = "running";
    finalization["message"] = std::string(messages::kRetrying);
    return finalization;
}

Json failed_finalization(const std::string& message) {
    Json finalization = Json::object();
    finalization["status"] = "failed";
    finalization["message"] = message;
    return finalization;
}

} // namespace

// One retry as its worker sees it.
struct MeetingRuntime::Retry {
    std::shared_ptr<RuntimeEngine> engine;
    std::string meeting_id;
    std::string step_key;
    std::uint64_t generation = 0;
    std::shared_ptr<FinalizationClaim> claim;
    std::optional<SavedAgentPermit> permit;
    // Where the outcome is published; null once another run owns the meeting.
    std::shared_ptr<RuntimeEngine> publish_engine;
};

void MeetingRuntime::retry_step(std::string step_id, std::optional<std::string> meeting_id) {
    std::string step_key = py::strip(step_id);
    if (step_key.empty()) {
        step_key = "failed";
    }
    if (retry_refused(step_key)) {
        return;
    }
    std::string target = meeting_id.value_or("");
    if (target.empty()) {
        const std::lock_guard lock(mutex_);
        if (engine_) {
            target = engine_->meeting_id();
        }
        if (target.empty() && card_meeting_id_) {
            target = *card_meeting_id_;
        }
    }
    if (!target.empty()) {
        prepare_retry(std::move(step_key), std::move(target));
        return;
    }
    // The newest meeting is a repository read, which never runs on the UI actor.
    spawn("meeting-retry-lookup", [this, step_key = std::move(step_key)] {
        std::string newest;
        try {
            const std::vector<Json> recent = deps_.repository->list_meetings();
            if (!recent.empty()) {
                newest = detail::text_of(recent.front(), "id");
            }
        } catch (const std::exception& error) {
            logger.error_with(error, "Could not find the newest meeting to retry");
        }
        post_ui([this, step_key, newest] {
            if (newest.empty()) {
                logger.warning("Cannot retry finalization: no meeting found");
                return;
            }
            prepare_retry(step_key, newest);
        });
    });
}

bool MeetingRuntime::retry_refused(const std::string& step_key) {
    if (deps_.activity != nullptr && deps_.activity->snapshot().backup_in_progress) {
        meeting_status_update.emit(std::string(messages::kBackupBusy));
        return true;
    }
    if (step_key == "speaker_id") {
        // The retry skips a refused speaker pass anyway; refusing here also spares re-running
        // the steps after it for nothing.
        const std::string reason = detail::text_of(speaker_rerun_fields(), "speaker_rerun_reason");
        if (!reason.empty()) {
            logger.info("Speaker re-run refused: {}", reason);
            meeting_status_update.emit(reason);
            return true;
        }
    }
    return false;
}

bool MeetingRuntime::retry_needs_agent(const Json& meeting, const std::string& step) {
    const auto state = meeting_state::load_state(meeting, false, settings_snapshot(), clock_);
    if (!state.cloud_enabled) {
        return false;
    }
    const bool audio = detail::truthy(detail::get(meeting, "spool_dir"));
    const auto ids = run_ids(steps_from_state(state.finalization.steps, step, true, audio), step);
    return std::ranges::any_of(
        ids, [](const auto& id) { return id == "polish" || id == "consolidation"; });
}

std::optional<MeetingRuntime::SavedAgentPermit>
MeetingRuntime::approve_saved_agent(const Json& meeting, bool report, const CancelToken& cancel) {
    if (cancel.cancelled() || shutdown_.cancelled() ||
        detail::text_of(meeting, "status") != "ended") {
        return std::nullopt;
    }
    const auto id = detail::text_of(meeting, "id");
    const auto settings = settings_snapshot();
    const auto route =
        meeting_agent::resolve_meeting_agent_route(settings, std::optional<Json>(meeting));
    const bool needs_prompt =
        !report || route.installed() ||
        !llm::profile_from_agent_config(route.provider, route.endpoint, settings).is_local();
    if (needs_prompt) {
        if (!deps_.confirm_saved_insights) {
            return std::nullopt;
        }
        auto promise = std::make_shared<std::promise<bool>>();
        auto answer = promise->get_future();
        post_ui([confirm = deps_.confirm_saved_insights, promise, cancel,
                 stopping = shutdown_.token(), meeting, route, report] {
            bool approved = false;
            try {
                approved =
                    !cancel.cancelled() && !stopping.cancelled() && confirm(meeting, route, report);
            } catch (const std::exception&) {
                // Configuration and dialog failures leave the destination unapproved.
            }
            promise->set_value(approved);
        });
        while (answer.wait_for(std::chrono::milliseconds(25)) != std::future_status::ready) {
            if (cancel.cancelled() || shutdown_.cancelled()) {
                return std::nullopt;
            }
        }
        if (!answer.get()) {
            return std::nullopt;
        }
    }
    const auto allowed = [repo = deps_.repository, settings = deps_.settings,
                          shutdown = shutdown_.token(), meeting, id, route, cancel] {
        if (cancel.cancelled() || shutdown.cancelled()) {
            return false;
        }
        try {
            const auto current = repo->get_meeting(id);
            if (!current || detail::text_of(*current, "status") != "ended") {
                return false;
            }
            for (const auto field : {"id", "started_at", "title", "agent_provider", "agent_model",
                                     "agent_endpoint_json"}) {
                if (detail::get(*current, field) != detail::get(meeting, field)) {
                    return false;
                }
            }
            const Json current_settings = settings ? *settings->snapshot() : Json::object();
            return meeting_agent::resolve_meeting_agent_route(current_settings, current) == route;
        } catch (const std::exception&) {
            return false;
        }
    };
    if (!allowed()) {
        return std::nullopt;
    }
    return SavedAgentPermit{route, allowed};
}

void MeetingRuntime::prepare_retry(std::string step_key, std::string meeting_id) {
    if (!deps_.confirm_saved_insights) {
        claim_and_run_retry(std::move(step_key), std::move(meeting_id));
        return;
    }
    spawn("meeting-retry-consent", [this, step_key = std::move(step_key),
                                    meeting_id = std::move(meeting_id)] {
        std::optional<SavedAgentPermit> permit;
        try {
            const auto meeting = deps_.repository->get_meeting(meeting_id);
            if (!meeting) {
                return;
            }
            if (retry_needs_agent(*meeting, step_key)) {
                permit = approve_saved_agent(*meeting, false, shutdown_.token());
                if (!permit) {
                    return;
                }
            }
        } catch (const std::exception&) {
            post_ui([this] {
                meeting_status_update.emit("Could not read this meeting's insights destination.");
            });
            return;
        }
        post_ui([this, step_key, meeting_id, permit = std::move(permit)]() mutable {
            if (!shutdown_.cancelled()) {
                claim_and_run_retry(step_key, meeting_id, std::move(permit));
            }
        });
    });
}

void MeetingRuntime::claim_and_run_retry(std::string step_key, std::string meeting_id,
                                         std::optional<SavedAgentPermit> permit) {
    const auto retry = claim_retry(meeting_id, step_key);
    if (!retry) {
        return;
    }
    if ((step_key == "failed" || step_key == "redecode") && deps_.ensure_local_model_available) {
        try {
            deps_.ensure_local_model_available();
        } catch (const std::exception& error) {
            logger.error_with(error, "Could not ensure the local speech model");
        }
    }
    retry->permit = std::move(permit);
    meeting_status_update.emit(std::string(messages::kRetrying));
    spawn("meeting-retry-finalization", [this, retry] { retry_worker(retry); });
}

std::shared_ptr<MeetingRuntime::Retry> MeetingRuntime::claim_retry(const std::string& meeting_id,
                                                                   const std::string& step_key) {
    const bool backup = deps_.activity != nullptr && deps_.activity->snapshot().backup_in_progress;
    std::shared_ptr<Retry> retry;
    std::string refusal;
    {
        const std::lock_guard lock(mutex_);
        if (starting_ || controller_meeting_active_ || is_active_locked() || finalizing_ ||
            backup) {
            logger.warning("Cannot retry finalization: meeting is active, starting, or already "
                           "finalizing");
            return nullptr;
        }
        if (background_engines_.contains(meeting_id)) {
            refusal = messages::kStillInBackground;
        } else if (is_running(meeting_id)) {
            // The dashboard (or recovery) holds the one-run claim; a second run would only fail
            // and overwrite its progress.
            refusal = messages::kRetryBusy;
        } else {
            card_meeting_id_ = meeting_id;
            ++retry_generation_;
            retry_meeting_id_ = meeting_id;
            background_ready_ = false;
            finalizing_ = true;
            finalization_.emplace(running_retry());
            retry = std::make_shared<Retry>();
            retry->engine = engine_;
            retry->meeting_id = meeting_id;
            retry->step_key = step_key;
            retry->generation = retry_generation_;
            retry->publish_engine = engine_;
        }
    }
    if (!retry) {
        meeting_status_update.emit(refusal);
        return nullptr;
    }
    sync_activity();
    Json state = Json::object();
    state["background_available"] = false;
    meeting_state_changed.emit(state);
    return retry;
}

void MeetingRuntime::retry_worker(const std::shared_ptr<Retry>& retry) {
    const std::string& meeting_id = retry->meeting_id;
    std::string message(messages::kRetryFailed);
    Json finalization = failed_finalization(message);
    try {
        retry->claim = std::make_shared<FinalizationClaim>(meeting_id);
        {
            const std::lock_guard lock(mutex_);
            retry_claim_ = retry->claim;
        }
        publish_finalization(meeting_id, running_retry(), retry->engine, retry->generation);
        finalization = rerun_finalization(retry);
        message = detail::text_of(finalization, "message");
    } catch (const FinalizationBusyError&) {
        // Another run won admission: never publish an unowned snapshot over its progress.
        message = messages::kRetryBusy;
        finalization = stored_finalization(meeting_id);
        retry->publish_engine.reset();
    } catch (const std::exception& error) {
        logger.error_with(
            error, std::format("Retry finalization worker raised for meeting {}", meeting_id));
        message = messages::retry_failed(error.what());
        finalization = failed_finalization(message);
    }
    if (retry->claim && retry->engine && retry->engine->meeting_id() == meeting_id) {
        try {
            retry->engine->revoke_agent_writes();
        } catch (const std::exception& error) {
            logger.error_with(error, "Could not revoke agent writes after finalization retry");
        }
    }
    finish_retry(retry, std::move(finalization), message);
}

RerunRequest MeetingRuntime::rerun_request(const Json& meeting, const std::string& from_step,
                                           ModelLease* lease) const {
    const Json settings = settings_snapshot();
    RerunRequest request;
    request.repository = deps_.repository;
    request.options = rerun_options(meeting, settings, clock_);
    request.from_step = from_step;
    request.settings = settings;
    request.intelligence = intelligence_;
    request.asr = deps_.asr;
    if (deps_.diarizer) {
        request.diarizer = deps_.diarizer;
    } else {
        request.diarizer = speaker_model_diarizer(
            deps_.sources.speaker_model_path ? deps_.sources.speaker_model_path() : std::nullopt,
            deps_.sources.onnxruntime_path ? deps_.sources.onnxruntime_path() : std::nullopt);
    }
    // Only the redecode step loads a model, and only it takes the lease: a polish-only retry
    // never touches the dictation engine.
    request.model_lease = lease;
    request.clock = &clock_;
    request.cancel = shutdown_.token();
    return request;
}

Json MeetingRuntime::rerun_finalization(const std::shared_ptr<Retry>& retry) {
    const std::string& meeting_id = retry->meeting_id;
    const Json meeting = deps_.repository->get_meeting(meeting_id).value_or(Json::object());
    std::shared_ptr<meeting_state::MeetingStateStore> store;
    if (retry->engine && retry->engine->meeting_id() == meeting_id) {
        store = retry->engine->store();
        try {
            retry->engine->allow_agent_writes();
        } catch (const std::exception& error) {
            logger.error_with(error, "Could not allow agent writes for finalization retry");
        }
    }
    RerunRequest request = rerun_request(meeting, retry->step_key, model_lease_.get());
    request.store = store;
    if (retry->permit) {
        request.approved_route = retry->permit->route;
        request.saved_destination_allowed = retry->permit->allowed;
    } else if (deps_.confirm_saved_insights) {
        request.saved_destination_allowed = [] {
            return false;
        };
    }
    request.claim = retry->claim.get();
    request.progress = [this, retry](const Json& snapshot) {
        if (!retry->claim->is_active()) {
            return;
        }
        publish_finalization(retry->meeting_id, snapshot, retry->engine, retry->generation);
    };
    const Json result = deps_.rerun(meeting_id, request);
    const Json& reported = detail::get(result, "finalization");
    Json finalization = reported.is_object() ? reported : Json::object();
    std::string status = detail::text_of(finalization, "status");
    if (status.empty()) {
        status = detail::truthy(detail::get(result, "ok")) ? "completed" : "failed";
    }
    std::string message = detail::text_of(finalization, "message");
    if (message.empty()) {
        message = detail::text_of(result, "error");
    }
    if (message.empty()) {
        message = messages::kRetryFailed;
    }
    finalization["status"] = status;
    finalization["message"] = message;
    return finalization;
}

void MeetingRuntime::finish_retry(const std::shared_ptr<Retry>& retry, Json finalization,
                                  const std::string& message) {
    const std::string& meeting_id = retry->meeting_id;
    try {
        if (retry->claim) {
            publish_finalization(meeting_id, finalization, retry->publish_engine,
                                 retry->generation);
        }
        Json status = Json::object();
        status["message"] = message;
        status["meeting_id"] = meeting_id;
        queue_engine_event(retry->engine, "retry_status", std::move(status), retry->generation);
        refresh_past_meetings();
    } catch (const std::exception& error) {
        logger.error_with(error, std::format("Could not publish retry outcome for {}", meeting_id));
        if (retry->claim) {
            Json card = finalization;
            card["content_summary"] = Json::object();
            Json event = Json::object();
            event["active"] = false;
            event["meeting_id"] = meeting_id;
            event["finalization"] = std::move(card);
            queue_engine_event(retry->engine, "retry_finalization", std::move(event),
                               retry->generation);
        }
    }
    // A terminal progress callback is not the worker's completion: ownership is kept until the
    // cleanup and the publication above have finished.
    {
        const std::lock_guard lock(mutex_);
        if (retry->generation == retry_generation_) {
            finalization_.emplace(std::move(finalization));
            card_meeting_id_ = meeting_id;
            retry_meeting_id_.reset();
            retry_claim_.reset();
            finalizing_ = false;
        }
    }
    sync_activity();
    if (retry->claim) {
        retry->claim->release();
    }
}

Json MeetingRuntime::stored_finalization(const std::string& meeting_id) {
    try {
        const Json meeting = deps_.repository->get_meeting(meeting_id).value_or(Json::object());
        const Json data = meeting_state::stored_state_dict(meeting);
        const std::string status = detail::text_of(meeting, "status");
        return meeting_state::FinalizationState::coerce(
                   detail::get(data, "finalization"),
                   detail::truthy(detail::get(data, "cloud_enabled")),
                   status.empty() ? std::string("ended") : status)
            .to_json();
    } catch (const std::exception& error) {
        logger.error_with(error,
                          std::format("Could not read stored finalization for {}", meeting_id));
        Json finalization = Json::object();
        finalization["status"] = "running";
        finalization["message"] = std::string(messages::kRetryBusy);
        return finalization;
    }
}

void MeetingRuntime::publish_finalization(const std::string& meeting_id, const Json& finalization,
                                          const std::shared_ptr<RuntimeEngine>& engine,
                                          std::optional<std::uint64_t> retry_generation) {
    if (publish_hook_) {
        publish_hook_(meeting_id, finalization);
    }
    {
        const std::lock_guard lock(mutex_);
        if (retry_generation && *retry_generation != retry_generation_) {
            return;
        }
    }
    Json payload = finalization.is_object() ? finalization : Json::object();
    payload["content_summary"] = meeting_content_summary(meeting_id);
    if (engine && engine->meeting_id() == meeting_id) {
        try {
            engine->publish_finalization(payload);
            return;
        } catch (const std::exception& error) {
            logger.error_with(error, "Could not publish finalization on the live engine");
        }
    }
    Json event = Json::object();
    event["active"] = false;
    event["meeting_id"] = meeting_id;
    event["finalization"] = payload;
    std::optional<Json> meeting;
    try {
        meeting = deps_.repository->get_meeting(meeting_id);
    } catch (const std::exception& error) {
        logger.error_with(
            error, std::format("Could not read meeting {} to publish finalization", meeting_id));
    }
    if (meeting && detail::truthy(*meeting)) {
        const std::string status = detail::text_of(*meeting, "status");
        event["status"] = status.empty() ? std::string("ended") : status;
        event.update(meeting_identity_fields(*meeting, &payload));
    }
    queue_engine_event(engine, "retry_finalization", std::move(event), retry_generation);
}

meeting_web::PostMeetingPasses
MeetingRuntime::post_meeting_passes(std::shared_ptr<ModelLease> lease) {
    meeting_web::PostMeetingPasses passes;
    passes.rerun_insights = [this, lease](const std::string& meeting_id,
                                          const std::string& from_step, const CancelToken& cancel) {
        const auto meeting = deps_.repository->get_meeting(meeting_id);
        if (!meeting) {
            throw py::ValueError("unknown meeting");
        }
        RerunRequest request = rerun_request(*meeting, from_step, lease.get());
        if (deps_.confirm_saved_insights && retry_needs_agent(*meeting, from_step)) {
            auto permit = approve_saved_agent(*meeting, false, cancel);
            if (!permit) {
                throw meeting_web::HttpError(403,
                                             "This meeting's insights retry was not approved.");
            }
            request.approved_route = std::move(permit->route);
            request.saved_destination_allowed = std::move(permit->allowed);
        } else if (deps_.confirm_saved_insights) {
            request.saved_destination_allowed = [] {
                return false;
            };
        }
        CancelSource stop(shutdown_.token());
        const auto disconnect = cancel.on_cancel([stop]() mutable { stop.cancel(); });
        request.cancel = stop.token();
        if (request.cancel.cancelled()) {
            throw meeting_web::HttpError(409, "This meeting's insights retry was stopped.");
        }
        try {
            return deps_.rerun(meeting_id, request);
        } catch (const FinalizationBusyError& error) {
            throw meeting_web::PostMeetingPassBusy(error.what());
        }
    };
    passes.rerun_speakers = [this, lease](const std::string& meeting_id,
                                          std::shared_ptr<meeting_state::MeetingStateStore> store) {
        const auto meeting = deps_.repository->get_meeting(meeting_id);
        if (!meeting) {
            throw py::ValueError("unknown meeting");
        }
        RerunRequest request = rerun_request(*meeting, "speaker_id", lease.get());
        request.store = std::move(store);
        try {
            return rerun_speakers(meeting_id, request);
        } catch (const FinalizationBusyError& error) {
            throw meeting_web::PostMeetingPassBusy(error.what());
        }
    };
    passes.speaker_gate_refusal = [this](const Json&) -> std::optional<std::string> {
        const SpeakerPassGate gate = resolve_speaker_pass(settings_snapshot(), clock_);
        if (gate.ok) {
            return std::nullopt;
        }
        return gate.reason;
    };
    if (deps_.start_saved_report) {
        passes.report = [this](std::shared_ptr<meeting_state::MeetingStateStore> store,
                               const Json& meeting, std::string_view text, Executor& worker,
                               const CancelToken& cancel) {
            auto permit = approve_saved_agent(meeting, true, cancel);
            if (!permit) {
                throw meeting_web::HttpError(403, "This report destination was not approved.");
            }
            return deps_.start_saved_report(std::move(store), permit->route, text, worker, cancel,
                                            std::move(permit->allowed));
        };
    }
    if (deps_.local_review_judge || deps_.local_fast_judge) {
        passes.review = [this](std::shared_ptr<meeting_state::MeetingStateStore> store) {
            const auto allowed = [this] {
                return meeting_intel::resolve_meeting_insight_review(settings_snapshot())
                    .value("enabled", false);
            };
            auto judge = deps_.local_review_judge;
            if (!judge) {
                judge = [fast = deps_.local_fast_judge](double) {
                    return fast();
                };
            }
            return meeting_intel::run_insight_review(
                *store, *deps_.repository, {std::move(judge), allowed, &clock_}, shutdown_.token());
        };
    }
    if (const auto sql =
            std::dynamic_pointer_cast<meeting_state::SqlMeetingRepository>(deps_.repository);
        sql && deps_.local_fast_judge) {
        passes.semantic_search = [this, sql](std::string_view query) {
            meeting_intel::RepositoryHistorySource source(*sql);
            const auto local = [this] {
                auto judge = deps_.local_fast_judge();
                return judge && judge->is_local() ? judge : std::shared_ptr<llm::Judge>{};
            };
            return meeting_intel::search_history(
                source, [this] { return settings_snapshot(); }, query, true, std::nullopt, 10,
                local, clock_, shutdown_.token());
        };
    }
    return passes;
}

} // namespace fv::meeting
