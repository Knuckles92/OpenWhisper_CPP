// api.py _register_page_routes, the /assets mount and _register_live_routes.

#include <format>
#include <string>
#include <system_error>

#include "base/identity.h"
#include "base/utf8.h"
#include "meeting_web/auth.h"
#include "meeting_web/dashboard_api.h"
#include "meeting_web/detail/api_support.h"
#include "meeting_web/public_meeting.h"
#include "meeting_web/server.h"

namespace fv::meeting_web {

namespace {

// _MISSING_BUNDLE_PAGE: served in place of the dashboard when webui/dist is missing (a build
// whose bundle copy was deleted).
std::string missing_bundle_page() {
    return std::format("<!doctype html><html lang=\"en\"><head><meta charset=\"utf-8\">"
                       "<title>{} Meeting</title></head><body>"
                       "<p>The dashboard bundle is missing; run "
                       "<code>npm run build --prefix webui</code>.</p></body></html>",
                       identity().product);
}

DashboardResponse not_found() {
    return DashboardResponse::error(404, "Not Found");
}

// StaticFiles.lookup_path: the file under `root` the request path names, or empty when it
// leaves the folder or is not a file.
std::filesystem::path static_file(const std::filesystem::path& root, std::string_view relative) {
    std::error_code error;
    const auto base = std::filesystem::weakly_canonical(root, error);
    if (error) {
        return {};
    }
    std::filesystem::path joined = base;
    std::size_t start = 0;
    while (start <= relative.size()) {
        const std::size_t end = std::min(relative.find('/', start), relative.size());
        const std::string_view part = relative.substr(start, end - start);
        // A part that is a path of its own (a drive, a root, a backslash) would replace the base.
        if (!part.empty() && part != ".") {
            const std::filesystem::path piece = path_from_utf8(part);
            if (piece.has_root_name() || piece.has_root_directory() ||
                part.find('\\') != std::string_view::npos) {
                return {};
            }
            joined /= piece;
        }
        start = end + 1;
    }
    const auto full = std::filesystem::weakly_canonical(joined, error);
    if (error) {
        return {};
    }
    const auto inside = full.lexically_relative(base);
    if (inside.empty() || *inside.begin() == "..") {
        return {};
    }
    if (!std::filesystem::is_regular_file(full, error)) {
        return {};
    }
    return full;
}

} // namespace

void DashboardApi::page(const DashboardRequest& request, Reply reply) {
    const std::string token = detail::path_param(request.http, "token");
    if (!resolve(token)) {
        throw HttpError(403, "invalid or expired link");
    }
    const auto index = dist_root_ / "index.html";
    std::error_code error;
    if (std::filesystem::is_regular_file(index, error)) {
        DashboardResponse response;
        response.file = DashboardResponse::File{index, "text/html", {}};
        reply(std::move(response));
        return;
    }
    DashboardResponse response;
    response.headers = {{"content-type", "text/html; charset=utf-8"}};
    response.body = missing_bundle_page();
    reply(std::move(response));
}

void DashboardApi::asset(const DashboardRequest& request, Reply reply) {
    const auto path = static_file(dist_root_ / "assets", detail::path_param(request.http, "path"));
    if (path.empty()) {
        reply(not_found());
        return;
    }
    DashboardResponse response;
    net::FileOptions options;
    options.not_modified_check = true;
    response.file = DashboardResponse::File{path, "", std::move(options)};
    reply(std::move(response));
}

void DashboardApi::session(const DashboardRequest& request, Reply reply) {
    detail::QueryReader query(request.http.query);
    const std::string token = query.text("token");
    query.finish();
    const std::string role = require(token);
    const auto meeting = current_meeting();
    const auto store = engine_.store();
    Json state = store ? store->snapshot() : Json::object();
    if (role != kRoleHost && state.is_object()) {
        // Same rule as the hello and the fan-out: tailored reports may draw on material outside
        // this meeting, so guests never receive them.
        state.erase("custom_reports");
    }
    Json body{{"role", role},
              {"meeting", public_meeting(meeting ? *meeting : Json(), nullptr, clock_)},
              {"state", std::move(state)},
              {"voice_commands", server_.voice_commands()}};
    reply(DashboardResponse::json(body));
}

void DashboardApi::transcript(const DashboardRequest& request, Reply reply) {
    detail::QueryReader query(request.http.query);
    const std::string token = query.text("token");
    const std::string cursor = query.text("cursor");
    const std::int64_t limit = query.integer("limit", detail::kTranscriptPageDefault);
    query.finish();
    require(token);
    const std::string meeting_id = engine_.meeting_id();
    if (meeting_id.empty()) {
        reply(DashboardResponse::json(Json{{"items", Json::array()}, {"next_cursor", nullptr}}));
        return;
    }
    reply(DashboardResponse::json(transcript_page(meeting_id, cursor, limit)));
}

} // namespace fv::meeting_web
