// api.py _register_title_and_review_routes, _register_insights_rerun_route,
// _register_speakers_rerun_route, the delete route of _register_history_admin_routes and the
// report request of _register_report_routes.

#include <array>
#include <format>
#include <stdexcept>
#include <string>
#include <utility>

#include "meeting_state/content.h"
#include "meeting_state/custom_report_ops.h"
#include "meeting_state/data_lifecycle.h"
#include "meeting_state/schema.h"
#include "meeting_web/dashboard_api.h"
#include "meeting_web/detail/api_support.h"
#include "meeting_web/detail/py_value.h"
#include "meeting_web/extensions.h"
#include "pyport/text.h"

namespace fv::meeting_web {

namespace {

// Insight review operations a client may send.
constexpr std::array<std::string_view, 4> kReviewActions = {"start", "review_answer", "review_skip",
                                                            "review_reopen"};

bool review_running(const meeting_state::MeetingStateStore* store) {
    if (store == nullptr) {
        return false;
    }
    const Json snapshot = store->snapshot();
    return detail::get(detail::get(snapshot, "insight_review"), "status") == "running";
}

} // namespace

void DashboardApi::rename(const DashboardRequest& request, Reply reply) {
    const std::string meeting_id = detail::path_param(request.http, "meeting_id");
    detail::QueryReader query(request.http.query);
    const std::string token = query.text("token");
    query.finish();
    require(token, true);
    const Json body = json_body(request.http);
    reply(DashboardResponse::json(retitle(meeting_id, detail::get(body, "title"))));
}

void DashboardApi::review(const DashboardRequest& request, Reply reply) {
    const std::string meeting_id = detail::path_param(request.http, "meeting_id");
    detail::QueryReader query(request.http.query);
    const std::string token = query.text("token");
    query.finish();
    require(token, true);
    const Json body = json_body(request.http);
    const Json& action = detail::get(body, "op");
    bool known = false;
    for (const auto name : kReviewActions) {
        known = known || (action.is_string() && action.get<std::string>() == name);
    }
    if (!known) {
        throw HttpError(400, "invalid review action");
    }
    if (const auto extensions = this->extensions()) {
        reply(extensions->review(*this, meeting_id, body));
        return;
    }
    if (!passes_.review) {
        throw HttpError(409, std::string(detail::kAiInsightsOff));
    }
    const auto meeting = saved_meeting(meeting_id);
    const auto store = owning_store(meeting_id, meeting);
    {
        std::lock_guard lock(mutex_);
        if (!insights_running_.insert(meeting_id).second) {
            throw HttpError(409, "Wait for final insights to finish");
        }
    }
    run_post_meeting_pass(
        meeting_id,
        [check = passes_.review, store, body, start = action == "start"] {
            Json result;
            if (start) {
                result = check(store);
            } else {
                const auto applied = store->apply("host", std::nullopt, Json::array({body}));
                const bool ok = !applied.empty() && applied.front().ok;
                result["ok"] = ok;
                result["error"] = ok ? Json()
                                  : applied.empty()
                                      ? Json("inactive")
                                      : Json(applied.front().reason.value_or("inactive"));
            }
            result["state"] = store->snapshot();
            return result;
        },
        {}, std::move(reply));
}

void DashboardApi::request_report(const DashboardRequest& request, Reply reply) {
    // A report written to the requester's own description.
    const std::string meeting_id = detail::path_param(request.http, "meeting_id");
    detail::QueryReader query(request.http.query);
    const std::string token = query.text("token");
    query.finish();
    require(token, true);
    const Json body = json_body(request.http);
    const Json& text = detail::get(body, "request");
    if (!text.is_string() || py::strip(text.get<std::string>()).empty() ||
        py::len(text.get<std::string>()) > meeting_state::MAX_REQUEST_CHARS) {
        throw HttpError(400, std::format("Describe the report you want, in 1 to {} characters.",
                                         meeting_state::MAX_REQUEST_CHARS));
    }
    if (const auto extensions = this->extensions()) {
        reply(extensions->request_report(*this, meeting_id, text.get<std::string>()));
        return;
    }
    if (!passes_.report) {
        throw HttpError(409, std::string(detail::kAiInsightsOff));
    }
    const auto meeting = saved_meeting(meeting_id);
    const auto store = owning_store(meeting_id, meeting);
    {
        std::lock_guard lock(mutex_);
        if (!insights_running_.insert(meeting_id).second) {
            throw HttpError(409, "Wait for the meeting's insights to finish");
        }
    }
    run_post_meeting_pass(
        meeting_id,
        [start = passes_.report, store, meeting, text = text.get<std::string>(), worker = reports_,
         cancel = saved_pass_stop_.token()] {
            Json result = start(store, meeting, text, *worker, cancel);
            if (detail::get(result, "ok") != true) {
                const auto& error = detail::get(result, "error");
                if (error == "report_running") {
                    throw HttpError(409, "A report is already being written for this meeting.");
                }
                if (error == "meeting_not_ready") {
                    throw HttpError(409, "Reports are written once the meeting has ended and its "
                                         "insights have finished.");
                }
                if (error == "report_limit_reached") {
                    throw HttpError(409, "This meeting is holding the maximum number of reports. "
                                         "Delete one to make room.");
                }
                throw HttpError(409, "The report could not be started.");
            }
            result["state"] = store->snapshot();
            return result;
        },
        {}, std::move(reply));
}

void DashboardApi::rerun_insights(const DashboardRequest& request, Reply reply) {
    // Retries failed post-meeting steps, redecode included when needed.
    const std::string meeting_id = detail::path_param(request.http, "meeting_id");
    detail::QueryReader query(request.http.query);
    const std::string token = query.text("token");
    query.finish();
    require(token, true);
    if (is_active_meeting(meeting_id)) {
        throw HttpError(409, "cannot re-run insights on the active meeting");
    }
    saved_meeting(meeting_id);
    if (!passes_.rerun_insights) {
        throw std::logic_error("The dashboard has no insights re-run");
    }
    const auto live = is_current(meeting_id) ? engine_.store() : nullptr;
    std::shared_ptr<meeting_state::MeetingStateStore> store;
    {
        // Check and claim at once: a double-click cannot start two passes writing the same past
        // meeting.
        std::lock_guard lock(mutex_);
        if (insights_running_.contains(meeting_id)) {
            throw HttpError(409, "insights are already running for this meeting");
        }
        store = live;
        if (!is_current(meeting_id)) {
            if (const auto found = review_stores_.find(meeting_id); found != review_stores_.end()) {
                store = found->second;
            }
        }
        refuse_rerun_while_busy(store.get());
        review_stores_.erase(meeting_id);
        insights_running_.insert(meeting_id);
    }
    const Clock* clock = &clock_;
    run_post_meeting_pass(
        meeting_id,
        [rerun = passes_.rerun_insights, meeting_id, cancel = saved_pass_stop_.token()] {
            return rerun(meeting_id, "failed", cancel);
        },
        [store, clock](const Json& result) {
            // The re-run's fresh document goes to the store that serves the meeting.
            const Json& state = detail::get(result, "state");
            if (store && detail::truthy(state)) {
                store->replace_document(meeting_state::MeetingState::from_json(state, *clock));
            }
        },
        std::move(reply));
}

void DashboardApi::rerun_speakers(const DashboardRequest& request, Reply reply) {
    // Relabels a past meeting's speakers from its system-audio recording.
    const std::string meeting_id = detail::path_param(request.http, "meeting_id");
    detail::QueryReader query(request.http.query);
    const std::string token = query.text("token");
    query.finish();
    require(token, true);
    if (is_active_meeting(meeting_id)) {
        throw HttpError(409, "cannot re-run speakers on the active meeting");
    }
    const Json meeting = saved_meeting(meeting_id);
    const Json content = meeting_state::summarize_meeting_content(*repository_, meeting_id);
    if (!detail::truthy(content.at("can_rerun_speakers"))) {
        throw HttpError(400, "no system-audio recording is available for speaker identification");
    }
    if (passes_.speaker_gate_refusal) {
        if (const auto refusal = passes_.speaker_gate_refusal(meeting)) {
            throw HttpError(400, *refusal);
        }
    }
    if (!passes_.rerun_speakers) {
        throw std::logic_error("The dashboard has no speaker re-run");
    }
    const auto live = is_current(meeting_id) ? engine_.store() : nullptr;
    std::shared_ptr<meeting_state::MeetingStateStore> store;
    {
        std::lock_guard lock(mutex_);
        if (insights_running_.contains(meeting_id)) {
            throw HttpError(409, "a post-meeting pass is already running for this meeting");
        }
        store = live;
        if (!is_current(meeting_id)) {
            if (const auto found = review_stores_.find(meeting_id); found != review_stores_.end()) {
                store = found->second;
            }
        }
        // Also covers a just-ended meeting whose own end passes still run.
        refuse_rerun_while_busy(store.get());
        review_stores_.erase(meeting_id);
        insights_running_.insert(meeting_id);
    }
    run_post_meeting_pass(
        meeting_id,
        [rerun = passes_.rerun_speakers, meeting_id, store] { return rerun(meeting_id, store); },
        {}, std::move(reply));
}

void DashboardApi::delete_meeting(const DashboardRequest& request, Reply reply) {
    const std::string meeting_id = detail::path_param(request.http, "meeting_id");
    detail::QueryReader query(request.http.query);
    const std::string token = query.text("token");
    query.finish();
    require(token, true);
    if (is_active_meeting(meeting_id)) {
        throw HttpError(409, "cannot delete the active meeting");
    }
    const auto store = meeting_store(meeting_id);
    if (is_claimed(meeting_id) || review_running(store.get())) {
        throw HttpError(409, "Wait for the meeting update to finish before deleting");
    }
    // A just-ended meeting's end passes, or a report, are still writing.
    refuse_rerun_while_busy(store.get());
    drop_review_store(meeting_id);
    saved_meeting(meeting_id);
    std::filesystem::path meetings_root = engine_.spool_root();
    if (meetings_root.empty()) {
        meetings_root = options_.meetings_root;
    }
    meeting_state::delete_meeting_data(*repository_, meeting_id, meetings_root, true,
                                       options_.on_deleted);
    reply(DashboardResponse::json(Json{{"ok", true}}));
}

} // namespace fv::meeting_web
