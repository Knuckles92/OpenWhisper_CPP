// api.py _register_meeting_file_routes, _register_export_routes and the report DELETE and
// download routes of _register_report_routes.

#include <string>
#include <utility>

#include "meeting_capture/playback.h"
#include "meeting_state/export_json.h"
#include "meeting_state/export_markdown.h"
#include "meeting_state/export_txt.h"
#include "meeting_web/dashboard_api.h"
#include "meeting_web/detail/api_support.h"
#include "meeting_web/detail/py_value.h"
#include "meeting_web/public_meeting.h"
#include "pyport/errors.h"

namespace fv::meeting_web {

void DashboardApi::meeting_transcript(const DashboardRequest& request, Reply reply) {
    const std::string meeting_id = detail::path_param(request.http, "meeting_id");
    detail::QueryReader query(request.http.query);
    const std::string token = query.text("token");
    const std::string cursor = query.text("cursor");
    const std::int64_t limit = query.integer("limit", detail::kTranscriptPageDefault);
    query.finish();
    require_meeting(token, meeting_id);
    saved_meeting(meeting_id);
    reply(DashboardResponse::json(transcript_page(meeting_id, cursor, limit)));
}

void DashboardApi::segment(const DashboardRequest& request, Reply reply) {
    const std::string meeting_id = detail::path_param(request.http, "meeting_id");
    const std::string segment_id = detail::path_param(request.http, "segment_id");
    detail::QueryReader query(request.http.query);
    const std::string token = query.text("token");
    query.finish();
    require_meeting(token, meeting_id);
    auto segment = repository_->get_segment(meeting_id, segment_id);
    if (!segment) {
        throw HttpError(404, "unknown segment");
    }
    reply(DashboardResponse::json(Json{{"segment", std::move(*segment)}}));
}

void DashboardApi::audio(const DashboardRequest& request, Reply reply) {
    const std::string meeting_id = detail::path_param(request.http, "meeting_id");
    detail::QueryReader query(request.http.query);
    const std::string token = query.text("token");
    query.finish();
    require_meeting(token, meeting_id);
    std::filesystem::path path;
    try {
        path = meeting::build_playback(*repository_, meeting_id);
    } catch (const py::ValueError& error) {
        throw HttpError(400, error.what());
    } catch (const py::PyError& error) {
        if (error.type_name() == "FileNotFoundError") {
            throw HttpError(404, error.what());
        }
        throw;
    }
    // content_type_for would say audio/x-wav (no registry read); Python names the type.
    net::FileOptions options;
    options.headers = {{"cache-control", "no-store"}};
    options.filename = "meeting-" + meeting_id + ".wav";
    options.content_disposition_type = "inline";
    DashboardResponse response;
    response.file = DashboardResponse::File{std::move(path), "audio/wav", std::move(options)};
    reply(std::move(response));
}

void DashboardApi::export_meeting(const DashboardRequest& request, Reply reply) {
    const std::string fmt = detail::path_param(request.http, "fmt");
    detail::QueryReader query(request.http.query);
    const std::string token = query.text("token");
    const std::string requested_id = query.text("meeting_id");
    query.finish();
    require(token, true);
    std::string media_type;
    if (fmt == "md") {
        media_type = "text/markdown";
    } else if (fmt == "json") {
        media_type = "application/json";
    } else if (fmt == "txt") {
        media_type = "text/plain";
    } else {
        throw HttpError(404, "unknown export format");
    }
    const std::string target_id = requested_id.empty() ? engine_.meeting_id() : requested_id;
    if (target_id.empty()) {
        throw HttpError(404, "no meeting");
    }
    const Json meeting = saved_meeting(target_id);
    const Json state = state_for(target_id, meeting);
    const auto segments = repository_->get_segments(target_id, -1.0, std::nullopt);
    // Host export metadata stays; the JSON exporter strips capabilities and volatile fields.
    Json merged = meeting;
    const auto summary = public_meeting(meeting, nullptr, clock_);
    for (const auto& [key, value] : summary.items()) {
        merged[key] = value;
    }
    std::string content;
    if (fmt == "md") {
        content = meeting_state::export_markdown(merged, state, segments, true, true, clock_);
    } else if (fmt == "json") {
        content = meeting_state::export_json(merged, state, segments);
    } else {
        content = meeting_state::export_transcript_txt(merged, state, segments, clock_);
    }
    DashboardResponse response;
    response.headers = {
        {"content-disposition", "attachment; filename=\"meeting-" + target_id + "." + fmt + "\""},
        {"content-type", detail::media_type_header(media_type)}};
    response.body = std::move(content);
    reply(std::move(response));
}

void DashboardApi::delete_report(const DashboardRequest& request, Reply reply) {
    // Discards a report, one stranded by an interrupted run included.
    const std::string meeting_id = detail::path_param(request.http, "meeting_id");
    const std::string report_id = detail::path_param(request.http, "report_id");
    detail::QueryReader query(request.http.query);
    const std::string token = query.text("token");
    query.finish();
    require(token, true);
    const Json meeting = saved_meeting(meeting_id);
    const auto store = owning_store(meeting_id, meeting);
    const auto results =
        store->apply("host", std::nullopt,
                     Json::array({Json{{"op", "remove_custom_report"}, {"report_id", report_id}}}));
    if (results.empty() || !results.front().ok) {
        const Json reason = results.empty()          ? Json("rejected")
                            : results.front().reason ? Json(*results.front().reason)
                                                     : Json();
        throw HttpError(reason == "unknown_report" ? 404 : 409, reason);
    }
    reply(DashboardResponse::json(Json{{"ok", true}, {"state", store->snapshot()}}));
}

void DashboardApi::download_report(const DashboardRequest& request, Reply reply) {
    const std::string meeting_id = detail::path_param(request.http, "meeting_id");
    const std::string report_id = detail::path_param(request.http, "report_id");
    detail::QueryReader query(request.http.query);
    const std::string token = query.text("token");
    query.finish();
    require(token, true);
    const Json meeting = saved_meeting(meeting_id);
    const Json state = state_for(meeting_id, meeting);
    const Json* report = nullptr;
    const Json& reports = detail::get(state, "custom_reports");
    if (reports.is_array()) {
        for (const auto& candidate : reports) {
            if (detail::get(candidate, "id") == report_id) {
                report = &candidate;
                break;
            }
        }
    }
    if (report == nullptr) {
        throw HttpError(404, "unknown report");
    }
    if (detail::get(*report, "status") != "ready") {
        throw HttpError(409, "report is not ready");
    }
    const Json& markdown = detail::get(*report, "markdown");
    DashboardResponse response;
    response.headers = {
        {"content-disposition", "attachment; filename=\"report-" + report_id + ".md\""},
        {"content-type", detail::media_type_header("text/markdown")}};
    response.body = markdown.is_string() ? markdown.get<std::string>() : std::string();
    reply(std::move(response));
}

} // namespace fv::meeting_web
