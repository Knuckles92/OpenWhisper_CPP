// api.py _register_history_routes and the search and events routes of
// _register_history_admin_routes.

#include <algorithm>
#include <string>
#include <utility>

#include "meeting_web/cursors.h"
#include "meeting_web/dashboard_api.h"
#include "meeting_web/detail/api_support.h"
#include "meeting_web/detail/py_value.h"
#include "meeting_web/extensions.h"
#include "meeting_web/public_meeting.h"
#include "pyport/text.h"
#include "settings/keys.h"
#include "settings/store.h"

namespace fv::meeting_web {

namespace {

// meeting/semantic_search.py: longest query and the keyword page of search_history.
constexpr std::int64_t kSemanticQueryMaxChars = 500;
constexpr std::int64_t kSemanticKeywordLimit = 50;
// MeetingRepository.search_transcripts's default page.
constexpr std::int64_t kKeywordSearchLimit = 200;

} // namespace

Json filter_history_page(const std::vector<Json>& page, bool has_decisions, bool has_actions,
                         bool needs_attention, const Clock& clock) {
    Json result = Json::array();
    for (const auto& row : page) {
        Json public_row = public_meeting(row, nullptr, clock);
        const Json& digest = detail::get(public_row, "digest");
        if (has_decisions && !detail::truthy(detail::get(digest, "decisions"))) {
            continue;
        }
        if (has_actions && !detail::truthy(detail::get(digest, "action_items"))) {
            continue;
        }
        if (needs_attention && detail::get(public_row, "insights_tone") != "warning") {
            continue;
        }
        result.push_back(std::move(public_row));
    }
    return result;
}

void DashboardApi::meetings(const DashboardRequest& request, Reply reply) {
    detail::QueryReader query(request.http.query);
    const std::string token = query.text("token");
    const std::string cursor = query.text("cursor");
    const std::int64_t limit = query.integer("limit", detail::kHistoryPageDefault);
    const std::string q = query.text("q");
    const std::string since = query.text("since");
    const bool has_decisions = query.boolean("has_decisions", false);
    const bool has_actions = query.boolean("has_actions", false);
    const bool needs_attention = query.boolean("needs_attention", false);
    query.finish();
    require(token, true);
    std::optional<HistoryCursor> position;
    try {
        position = decode_history_cursor(cursor);
    } catch (const CursorError& error) {
        throw HttpError(400, error.what());
    }
    const std::int64_t page_limit =
        std::max<std::int64_t>(1, std::min<std::int64_t>(limit, detail::kHistoryPageMax));
    meeting_state::PastMeetingQuery past;
    past.limit = page_limit + 1;
    past.query = q;
    if (position) {
        past.cursor_started_at = position->started_at;
        past.cursor_id = position->meeting_id;
    }
    past.include_running = true;
    past.started_after = since;
    auto rows = past_meetings_->list_past_meeting_summaries(past);
    const bool has_more = static_cast<std::int64_t>(rows.size()) > page_limit;
    if (has_more) {
        rows.resize(static_cast<std::size_t>(page_limit));
    }
    // The digest filters inspect one bounded window. A continuation is returned even for an
    // empty filtered page, so older matches remain reachable.
    Json meetings = filter_history_page(rows, has_decisions, has_actions, needs_attention, clock_);
    Json next = nullptr;
    if (has_more && !rows.empty()) {
        next = history_cursor(rows.back());
    }
    reply(DashboardResponse::json(Json{{"meetings", std::move(meetings)}, {"next_cursor", next}}));
}

void DashboardApi::meeting_detail(const DashboardRequest& request, Reply reply) {
    const std::string meeting_id = detail::path_param(request.http, "meeting_id");
    detail::QueryReader query(request.http.query);
    const std::string token = query.text("token");
    const bool include_transcript = query.boolean("include_transcript", true);
    query.finish();
    require(token, true);
    const Json meeting = saved_meeting(meeting_id);
    Json transcript = include_transcript
                          ? transcript_page(meeting_id, "", detail::kTranscriptPageDefault)
                          : Json{{"items", Json::array()}, {"next_cursor", nullptr}};
    Json public_row = public_meeting(meeting, repository_.get(), clock_);
    Json state = state_for(meeting_id, meeting);
    reply(DashboardResponse::json(Json{{"meeting", std::move(public_row)},
                                       {"state", std::move(state)},
                                       {"segments", std::move(transcript["items"])},
                                       {"transcript_next_cursor", transcript["next_cursor"]},
                                       {"transcript_included", include_transcript}}));
}

void DashboardApi::meeting_state(const DashboardRequest& request, Reply reply) {
    // Follows report and review progress without transcript or content queries.
    const std::string meeting_id = detail::path_param(request.http, "meeting_id");
    detail::QueryReader query(request.http.query);
    const std::string token = query.text("token");
    query.finish();
    require(token, true);
    const Json meeting = saved_meeting(meeting_id);
    reply(DashboardResponse::json(Json{{"state", state_for(meeting_id, meeting)}}));
}

void DashboardApi::search(const DashboardRequest& request, Reply reply) {
    detail::QueryReader query(request.http.query);
    const std::string token = query.text("token");
    const std::string q = query.text("q");
    const std::string mode = query.text("mode", "keyword");
    query.finish();
    require(token, true);
    if (mode != "keyword" && mode != "semantic") {
        throw HttpError(400, "invalid search mode");
    }
    if (mode == "keyword") {
        Json results = Json::array();
        for (auto& row : repository_->search_transcripts(q, std::nullopt, kKeywordSearchLimit)) {
            results.push_back(std::move(row));
        }
        reply(DashboardResponse::json(
            Json{{"results", std::move(results)}, {"mode", "keyword"}, {"message", ""}}));
        return;
    }
    if (const auto extensions = this->extensions()) {
        reply(DashboardResponse::json(extensions->semantic_search(*this, q)));
        return;
    }
    if (passes_.semantic_search) {
        reply(DashboardResponse::json(passes_.semantic_search(q)));
        return;
    }
    // search_history(semantic=True) without a judge to rank with: its keyword branch.
    Json fallback{{"results", Json::array()}, {"mode", "keyword"}, {"message", ""}};
    const std::string stripped = py::slice(py::strip(q), 0, kSemanticQueryMaxChars);
    if (stripped.empty()) {
        reply(DashboardResponse::json(fallback));
        return;
    }
    for (auto& row :
         repository_->search_transcripts(stripped, std::nullopt, kSemanticKeywordLimit)) {
        fallback["results"].push_back(std::move(row));
    }
    const Json settings = settings_snapshot();
    const bool enabled = keys::typesafe_enabled.read(settings) &&
                         keys::typesafe_semantic_search_enabled.read(settings);
    fallback["message"] =
        std::string(enabled ? detail::kSemanticUnavailable : detail::kSemanticDisabled);
    reply(DashboardResponse::json(fallback));
}

void DashboardApi::events(const DashboardRequest& request, Reply reply) {
    detail::QueryReader query(request.http.query);
    const std::string token = query.text("token");
    const auto before_seq = query.optional_integer("before_seq");
    const std::int64_t limit = query.integer("limit", 100);
    query.finish();
    require(token, true);
    const std::string meeting_id = engine_.meeting_id();
    Json events = Json::array();
    if (!meeting_id.empty()) {
        for (auto& row : repository_->list_events(meeting_id, before_seq, limit)) {
            events.push_back(std::move(row));
        }
    }
    reply(DashboardResponse::json(Json{{"events", std::move(events)}}));
}

} // namespace fv::meeting_web
