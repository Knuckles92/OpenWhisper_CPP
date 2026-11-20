#include "meeting_web/dashboard_api.h"

#include <algorithm>
#include <chrono>
#include <exception>
#include <format>
#include <future>
#include <utility>

#include "base/log.h"
#include "history/titles.h"
#include "meeting_state/stored.h"
#include "meeting_web/auth.h"
#include "meeting_web/cursors.h"
#include "meeting_web/dashboard_root.h"
#include "meeting_web/detail/api_support.h"
#include "meeting_web/detail/py_value.h"
#include "meeting_web/detail/text_codecs.h"
#include "meeting_web/server.h"
#include "pyport/errors.h"
#include "pyport/json.h"
#include "settings/store.h"

namespace fv::meeting_web {

namespace {

constexpr log::Logger logger("meeting.web.api");

// How long a local caller waits for a dashboard action (server.py's future.result(timeout=15)).
constexpr std::chrono::seconds kCrossThreadWait{15};

constexpr std::string_view kInternalError = "Internal Server Error";

DashboardResponse internal_error() {
    DashboardResponse response;
    response.status = 500;
    response.headers = {{"content-type", "text/plain; charset=utf-8"}};
    response.body = std::string(kInternalError);
    return response;
}

// Answers a request exactly once; a reply never given answers 500, as an unhandled exception in
// a Starlette handler does.
class ReplyOnce {
public:
    explicit ReplyOnce(DashboardApi::Reply reply) : reply_(std::move(reply)) {}
    ~ReplyOnce() {
        if (!sent_) {
            send(internal_error());
        }
    }
    ReplyOnce(const ReplyOnce&) = delete;
    ReplyOnce& operator=(const ReplyOnce&) = delete;

    void send(DashboardResponse response) {
        std::lock_guard lock(mutex_);
        if (sent_) {
            return;
        }
        sent_ = true;
        try {
            reply_(std::move(response));
        } catch (const std::exception& error) {
            logger.error_with(error, "Could not send a dashboard response");
        }
    }

private:
    std::mutex mutex_;
    DashboardApi::Reply reply_;
    bool sent_ = false;
};

class SqlPastMeetingPages final : public PastMeetingPages {
public:
    explicit SqlPastMeetingPages(std::shared_ptr<meeting_state::SqlMeetingRepository> repository)
        : repository_(std::move(repository)) {}

    std::vector<Json>
    list_past_meeting_summaries(const meeting_state::PastMeetingQuery& query) override {
        return repository_->list_past_meeting_summaries(query);
    }

private:
    std::shared_ptr<meeting_state::SqlMeetingRepository> repository_;
};

std::string detail_text(const Json& detail) {
    return detail.is_string() ? detail.get<std::string>() : detail.dump();
}

// Starlette's path matching for call(): "{name}" one non-empty segment, "{name:path}" the rest.
bool match_route(std::string_view pattern, std::string_view path,
                 std::vector<std::pair<std::string, std::string>>& params) {
    params.clear();
    std::size_t p = 0;
    std::size_t s = 0;
    while (p < pattern.size()) {
        if (pattern[p] == '{') {
            const std::size_t close = pattern.find('}', p);
            std::string name(pattern.substr(p + 1, close - p - 1));
            p = close + 1;
            if (name.ends_with(":path")) {
                name.resize(name.size() - 5);
                params.emplace_back(std::move(name), std::string(path.substr(s)));
                s = path.size();
                continue;
            }
            const std::size_t end = std::min(path.find('/', s), path.size());
            if (end == s) {
                return false;
            }
            params.emplace_back(std::move(name), std::string(path.substr(s, end - s)));
            s = end;
            continue;
        }
        if (s >= path.size() || path[s] != pattern[p]) {
            return false;
        }
        ++p;
        ++s;
    }
    return s == path.size();
}

} // namespace

struct DashboardApi::Owned {
    // Destroyed in reverse order: io first, so that no handler still running on it posts a
    // re-run to a pool that is already gone (the re-run routes post from io to reruns).
    std::unique_ptr<ThreadPool> reports;
    std::unique_ptr<ThreadPool> reruns;
    std::unique_ptr<ThreadPool> io;
};

HttpError::HttpError(int status, Json detail)
    : std::runtime_error(detail_text(detail)), status_(status), detail_(std::move(detail)) {}

DashboardResponse DashboardResponse::json(const Json& body, int status) {
    DashboardResponse response;
    response.status = status;
    response.headers = {{"content-type", "application/json"}};
    response.body = detail::render_json(body);
    return response;
}

DashboardResponse DashboardResponse::error(int status, const Json& detail) {
    return json(Json{{"detail", detail}}, status);
}

std::shared_ptr<PastMeetingPages>
sql_past_meeting_pages(std::shared_ptr<meeting_state::SqlMeetingRepository> repository) {
    return std::make_shared<SqlPastMeetingPages>(std::move(repository));
}

DashboardApi::DashboardApi(DashboardEngine& engine,
                           std::shared_ptr<meeting_state::MeetingRepository> repository,
                           std::shared_ptr<PastMeetingPages> past_meetings,
                           MeetingWebServer& server, PostMeetingPasses passes,
                           DashboardOptions options, SettingsStore& settings, const Clock& clock)
    : engine_(engine), repository_(std::move(repository)), past_meetings_(std::move(past_meetings)),
      server_(server), passes_(std::move(passes)), options_(std::move(options)),
      settings_(settings), clock_(clock),
      dist_root_(options_.dist_root.empty() ? default_dist_root() : options_.dist_root),
      owned_(std::make_unique<Owned>()) {
    io_ = options_.io;
    if (io_ == nullptr) {
        owned_->io = std::make_unique<ThreadPool>("meeting-web", 4);
        io_ = owned_->io.get();
    }
    reruns_ = options_.reruns;
    if (reruns_ == nullptr) {
        owned_->reruns = std::make_unique<ThreadPool>("meeting-reinsights", 1);
        reruns_ = owned_->reruns.get();
    }
    if (passes_.report) {
        owned_->reports = std::make_unique<ThreadPool>("meeting-custom-report", 1);
        reports_ = owned_->reports.get();
    }
}

DashboardApi::~DashboardApi() {
    cancel_cloud_prompt();
    saved_pass_stop_.cancel();
}

void DashboardApi::set_extensions(std::shared_ptr<DashboardExtensions> extensions) {
    std::lock_guard lock(mutex_);
    extensions_ = std::move(extensions);
}

std::shared_ptr<DashboardExtensions> DashboardApi::extensions() const {
    std::lock_guard lock(mutex_);
    return extensions_;
}

Json DashboardApi::settings_snapshot() const {
    return *settings_.snapshot();
}

bool DashboardApi::is_current(std::string_view meeting_id) const {
    const std::string current = engine_.meeting_id();
    return !current.empty() && current == meeting_id;
}

bool DashboardApi::is_active_meeting(std::string_view meeting_id) const {
    return is_current(meeting_id) && engine_.is_active();
}

std::shared_ptr<meeting_state::MeetingStateStore>
DashboardApi::live_store(std::string_view meeting_id) const {
    auto store = engine_.store();
    if (store && is_current(meeting_id)) {
        return store;
    }
    return nullptr;
}

std::shared_ptr<meeting_state::MeetingStateStore>
DashboardApi::meeting_store(std::string_view meeting_id) const {
    if (is_current(meeting_id)) {
        return engine_.store();
    }
    std::lock_guard lock(mutex_);
    const auto found = review_stores_.find(meeting_id);
    return found == review_stores_.end() ? nullptr : found->second;
}

std::shared_ptr<meeting_state::MeetingStateStore>
DashboardApi::owning_store(const std::string& meeting_id, const Json& meeting) {
    if (auto store = live_store(meeting_id)) {
        return store;
    }
    std::lock_guard lock(mutex_);
    auto& cached = review_stores_[meeting_id];
    if (!cached) {
        try {
            cached = meeting_state::open_store(repository_, meeting_id, meeting, true,
                                               settings_snapshot(), clock_);
        } catch (...) {
            review_stores_.erase(meeting_id);
            throw;
        }
    }
    return cached;
}

void DashboardApi::drop_review_store(std::string_view meeting_id) {
    std::lock_guard lock(mutex_);
    if (const auto found = review_stores_.find(meeting_id); found != review_stores_.end()) {
        review_stores_.erase(found);
    }
}

std::optional<Json> DashboardApi::current_meeting() const {
    const std::string meeting_id = engine_.meeting_id();
    if (meeting_id.empty()) {
        return std::nullopt;
    }
    return repository_->get_meeting(meeting_id);
}

std::optional<std::string> DashboardApi::resolve(std::string_view token) const {
    const auto meeting = current_meeting();
    if (!meeting || !detail::truthy(*meeting)) {
        return std::nullopt;
    }
    return resolve_stored_role(token, detail::get(*meeting, "host_token"),
                               detail::get(*meeting, "guest_token"));
}

std::string DashboardApi::require(std::string_view token, bool host_only) const {
    const auto role = resolve(token);
    if (!role) {
        throw HttpError(401, "invalid token");
    }
    if (host_only && *role != kRoleHost) {
        throw HttpError(403, "host only");
    }
    return *role;
}

std::string DashboardApi::require_meeting(std::string_view token,
                                          std::string_view meeting_id) const {
    std::string role = require(token);
    if (role == kRoleGuest && meeting_id != engine_.meeting_id()) {
        throw HttpError(403, "guest access is current meeting only");
    }
    return role;
}

Json DashboardApi::saved_meeting(std::string_view meeting_id) const {
    auto meeting = repository_->get_meeting(meeting_id);
    if (!meeting) {
        throw HttpError(404, "unknown meeting");
    }
    return std::move(*meeting);
}

Json DashboardApi::state_for(const std::string& meeting_id, const Json& meeting) const {
    if (const auto store = live_store(meeting_id)) {
        return store->snapshot();
    }
    std::shared_ptr<meeting_state::MeetingStateStore> cached;
    {
        std::lock_guard lock(mutex_);
        if (const auto found = review_stores_.find(meeting_id); found != review_stores_.end()) {
            cached = found->second;
        }
    }
    if (cached) {
        return cached->snapshot();
    }
    return meeting_state::load_state(meeting, meeting_id, true, settings_snapshot(), clock_)
        .to_json();
}

Json DashboardApi::transcript_page(const std::string& meeting_id, std::string_view cursor,
                                   std::int64_t limit) const {
    std::optional<TranscriptCursor> position;
    try {
        position = decode_cursor(cursor);
    } catch (const CursorError& error) {
        throw HttpError(400, error.what());
    }
    const std::int64_t page_limit =
        std::max<std::int64_t>(1, std::min<std::int64_t>(limit, detail::kTranscriptPageMax));
    std::optional<double> start_s;
    std::optional<std::string> segment_id;
    if (position) {
        start_s = position->start_s;
        segment_id = position->segment_id;
    }
    auto rows = repository_->get_segments_page(meeting_id, start_s, segment_id, page_limit + 1);
    const bool has_more = static_cast<std::int64_t>(rows.size()) > page_limit;
    if (has_more) {
        rows.resize(static_cast<std::size_t>(page_limit));
    }
    Json items = Json::array();
    for (auto& row : rows) {
        items.push_back(std::move(row));
    }
    Json next = nullptr;
    if (has_more && !items.empty()) {
        next = encode_cursor(items.back());
    }
    return Json{{"items", std::move(items)}, {"next_cursor", std::move(next)}};
}

bool DashboardApi::claim(const std::string& meeting_id) {
    std::lock_guard lock(mutex_);
    return insights_running_.insert(meeting_id).second;
}

void DashboardApi::release(const std::string& meeting_id) {
    std::lock_guard lock(mutex_);
    insights_running_.erase(meeting_id);
}

bool DashboardApi::is_claimed(std::string_view meeting_id) const {
    std::lock_guard lock(mutex_);
    return insights_running_.contains(meeting_id);
}

Json DashboardApi::json_body(const net::HttpRequest& request) {
    Json body;
    try {
        if (request.body_too_large) {
            throw py::ValueError("body too large");
        }
        const auto text = detail::json_bytes_to_text(request.body);
        if (!text) {
            throw py::ValueError("undecodable body");
        }
        body = py::loads(*text);
    } catch (const std::exception&) {
        throw HttpError(400, "invalid JSON body");
    }
    if (!body.is_object()) {
        throw HttpError(400, "body must be an object");
    }
    return body;
}

void DashboardApi::refuse_rerun_while_busy(const meeting_state::MeetingStateStore* store) {
    if (store == nullptr) {
        return;
    }
    const Json snapshot = store->snapshot();
    const auto status_of = [&](std::string_view key) {
        // `(snapshot.get(key) or {}).get("status")`
        const Json& status = detail::get(detail::get(snapshot, key), "status");
        return status.is_string() ? status.get<std::string>() : std::string();
    };
    if (status_of("finalization") == "running") {
        throw HttpError(409, "Wait for the post-meeting steps to finish");
    }
    if (status_of("insight_review") == "running") {
        throw HttpError(409, "Wait for insight review to finish");
    }
    const Json& reports = detail::get(snapshot, "custom_reports");
    if (reports.is_array()) {
        for (const auto& report : reports) {
            if (report.is_object() && detail::get(report, "status") == "running") {
                throw HttpError(409, "Wait for the report being written to finish");
            }
        }
    }
}

void DashboardApi::run_post_meeting_pass(const std::string& meeting_id, std::function<Json()> job,
                                         std::function<void(const Json&)> adopt, Reply reply) {
    // Releases the claim once, before the answer goes out (Python's finally), or when the task is
    // dropped unrun.
    class Claim {
    public:
        Claim(DashboardApi* api, std::string meeting_id)
            : api_(api), meeting_id_(std::move(meeting_id)) {}
        ~Claim() { release(); }
        Claim(const Claim&) = delete;
        Claim& operator=(const Claim&) = delete;
        void release() {
            if (std::exchange(held_, false)) {
                api_->release(meeting_id_);
            }
        }

    private:
        DashboardApi* api_;
        std::string meeting_id_;
        bool held_ = true;
    };
    auto claim = std::make_shared<Claim>(this, meeting_id);
    reruns_->post([claim, job = std::move(job), adopt = std::move(adopt),
                   reply = std::move(reply)]() mutable {
        DashboardResponse response;
        try {
            Json result = job();
            if (adopt) {
                adopt(result);
            }
            response = DashboardResponse::json(result);
        } catch (const PostMeetingPassBusy& error) {
            response = DashboardResponse::error(409, error.what());
        } catch (const py::ValueError& error) {
            response = DashboardResponse::error(400, error.what());
        } catch (const HttpError& error) {
            response = DashboardResponse::error(error.status(), error.detail());
        } catch (const std::exception& error) {
            logger.error_with(error, "A post-meeting pass of the dashboard failed");
            response = internal_error();
        }
        claim->release();
        reply(std::move(response));
    });
}

Json DashboardApi::retitle(const std::string& meeting_id, const Json& title_value) {
    std::string title;
    try {
        title = normalize_title(title_value);
    } catch (const py::ValueError& error) {
        throw HttpError(400, error.what());
    }
    const Json set_title{{"op", "set_title"}, {"text", title}};
    const auto first_ok = [](const std::vector<meeting_state::OpResult>& results) {
        return !results.empty() && results.front().ok;
    };
    if (live_store(meeting_id)) {
        const auto results = engine_.apply_client_action("host", std::nullopt, set_title);
        return Json{{"ok", first_ok(results)}, {"title", title}};
    }
    saved_meeting(meeting_id);
    std::shared_ptr<meeting_state::MeetingStateStore> cached;
    {
        std::lock_guard lock(mutex_);
        if (const auto found = review_stores_.find(meeting_id); found != review_stores_.end()) {
            cached = found->second;
        }
    }
    if (cached) {
        const auto results = cached->apply("host", std::nullopt, Json::array({set_title}));
        return Json{{"ok", first_ok(results)}, {"title", title}};
    }
    repository_->rename_meeting(meeting_id, title);
    return Json{{"ok", true}, {"title", title}};
}

void DashboardApi::refresh_title(const std::string& meeting_id) {
    if (const auto store = live_store(meeting_id)) {
        if (store->refresh_title()) {
            // A reconnect sends a full hello without inventing an audit seq.
            server_.invalidate_connections(true);
        }
    }
    std::shared_ptr<meeting_state::MeetingStateStore> cached;
    {
        std::lock_guard lock(mutex_);
        if (const auto found = review_stores_.find(meeting_id); found != review_stores_.end()) {
            cached = found->second;
        }
    }
    if (cached) {
        cached->refresh_title();
    }
}

Json DashboardApi::run_on_io(std::function<Json()> fn) {
    // The task owns everything it uses: after a timeout it may still run.
    auto done = std::make_shared<std::promise<Json>>();
    auto future = done->get_future();
    io_->post([fn = std::move(fn), done] {
        try {
            done->set_value(fn());
        } catch (...) {
            done->set_exception(std::current_exception());
        }
    });
    if (future.wait_for(kCrossThreadWait) != std::future_status::ready) {
        throw std::runtime_error("The meeting dashboard did not answer within 15 seconds.");
    }
    return future.get();
}

bool DashboardApi::retitle_saved_meeting(const std::string& meeting_id, const std::string& title) {
    if (!server_.is_running()) {
        throw std::runtime_error("Meeting dashboard is unavailable.");
    }
    const Json result = run_on_io([this, meeting_id, title] { return retitle(meeting_id, title); });
    return detail::truthy(detail::get(result, "ok"));
}

void DashboardApi::refresh_saved_meeting_title(const std::string& meeting_id) {
    if (!server_.is_running()) {
        return;
    }
    run_on_io([this, meeting_id] {
        refresh_title(meeting_id);
        return Json();
    });
}

std::vector<DashboardApi::Route> DashboardApi::routes() const {
    std::vector<Route> table;
    std::error_code error;
    if (std::filesystem::is_directory(dist_root_ / "assets", error)) {
        table.push_back({"GET", "/assets/{path:path}", &DashboardApi::asset});
    }
    const auto add = [&table](std::string method, std::string pattern, Handler handler,
                              std::size_t max_body = std::size_t{1} << 20) {
        table.push_back({std::move(method), std::move(pattern), handler, max_body});
    };
    const std::size_t body = detail::kMaxJsonBody;
    // _register_page_routes (the WebSocket is the server's own)
    add("GET", "/m/{token}", &DashboardApi::page);
    // _register_live_routes
    add("GET", "/api/session", &DashboardApi::session);
    add("GET", "/api/transcript", &DashboardApi::transcript);
    // _register_history_routes
    add("GET", "/api/meetings", &DashboardApi::meetings);
    add("GET", "/api/meetings/{meeting_id}", &DashboardApi::meeting_detail);
    add("GET", "/api/meetings/{meeting_id}/state", &DashboardApi::meeting_state);
    // _register_meeting_file_routes
    add("GET", "/api/meetings/{meeting_id}/transcript", &DashboardApi::meeting_transcript);
    add("GET", "/api/meetings/{meeting_id}/segments/{segment_id}", &DashboardApi::segment);
    add("GET", "/api/meetings/{meeting_id}/audio", &DashboardApi::audio);
    // _register_title_and_review_routes
    add("POST", "/api/meetings/{meeting_id}/rename", &DashboardApi::rename, body);
    add("POST", "/api/meetings/{meeting_id}/review", &DashboardApi::review, body);
    // _register_insights_rerun_route, _register_speakers_rerun_route
    add("POST", "/api/meetings/{meeting_id}/reinsights", &DashboardApi::rerun_insights);
    add("POST", "/api/meetings/{meeting_id}/respeakers", &DashboardApi::rerun_speakers);
    // _register_history_admin_routes
    add("DELETE", "/api/meetings/{meeting_id}", &DashboardApi::delete_meeting);
    add("GET", "/api/search", &DashboardApi::search);
    add("GET", "/api/events", &DashboardApi::events);
    // _register_export_routes
    add("GET", "/api/export/{fmt}", &DashboardApi::export_meeting);
    // _register_report_routes
    add("POST", "/api/meetings/{meeting_id}/reports", &DashboardApi::request_report, body);
    add("DELETE", "/api/meetings/{meeting_id}/reports/{report_id}", &DashboardApi::delete_report);
    add("GET", "/api/meetings/{meeting_id}/reports/{report_id}/download",
        &DashboardApi::download_report);
    // _register_note_routes
    add("POST", "/api/meeting/notes/request", &DashboardApi::note_request, body);
    add("POST", "/api/meeting/end", &DashboardApi::end);
    // _register_meeting_control_routes
    add("POST", "/api/meeting/pause", &DashboardApi::pause);
    add("POST", "/api/meeting/resume", &DashboardApi::resume);
    add("POST", "/api/meeting/cloud", &DashboardApi::cloud, body);
    add("POST", "/api/meeting/tokens/regenerate", &DashboardApi::regenerate_tokens);
    return table;
}

void DashboardApi::dispatch(const Route& route, DashboardRequest request, Reply reply) {
    auto once = std::make_shared<ReplyOnce>(std::move(reply));
    try {
        (this->*route.handler)(
            request, [once](DashboardResponse response) { once->send(std::move(response)); });
    } catch (const HttpError& error) {
        once->send(DashboardResponse::error(error.status(), error.detail()));
    } catch (const std::exception& error) {
        // The route, never the path: a path can hold a capability token.
        logger.error_with(
            error, std::format("Dashboard request {} {} failed", route.method, route.pattern));
        once->send(internal_error());
    }
}

DashboardResponse DashboardApi::call(std::string_view method, std::string_view path,
                                     const net::Query& query, std::string body,
                                     const net::Headers& headers) {
    DashboardRequest request;
    request.http.method = std::string(method);
    request.http.path = std::string(path);
    request.http.query = query;
    request.http.headers = headers;
    request.http.body = std::move(body);
    request.http.peer = {"127.0.0.1", 0};
    request.http.tls = false;
    request.received = clock_.steady();
    const auto table = routes();
    const Route* found = nullptr;
    std::vector<std::string> allowed;
    for (const auto& route : table) {
        std::vector<std::pair<std::string, std::string>> params;
        if (!match_route(route.pattern, path, params)) {
            continue;
        }
        const bool same = route.method == method || (method == "HEAD" && route.method == "GET");
        if (same && found == nullptr) {
            found = &route;
            request.http.params = std::move(params);
        } else if (!same) {
            allowed.push_back(route.method);
        }
    }
    if (found == nullptr) {
        if (!allowed.empty()) {
            DashboardResponse response = DashboardResponse::error(405, "Method Not Allowed");
            std::string allow;
            for (const auto& name : allowed) {
                allow += (allow.empty() ? "" : ", ") + name;
            }
            response.headers.insert(response.headers.begin(), {"allow", allow});
            return response;
        }
        return DashboardResponse::error(404, "Not Found");
    }
    auto promise = std::make_shared<std::promise<DashboardResponse>>();
    auto future = promise->get_future();
    dispatch(*found, std::move(request),
             [promise](DashboardResponse response) { promise->set_value(std::move(response)); });
    return future.get();
}

void install_dashboard_routes(net::HttpServer& server, DashboardApi& api) {
    for (const auto& route : api.routes()) {
        net::RouteOptions options;
        options.max_body = route.max_body;
        // On the reactor only long enough to copy the request and note when it arrived (the End
        // log tells a busy pool from a slow engine.end()); the handler runs on the io executor.
        server.route(
            route.method, route.pattern, options,
            [&api, route](const net::HttpRequest& http, std::shared_ptr<net::Responder> responder) {
                DashboardRequest request{http, api.clock().steady()};
                api.io().post([&api, route, request = std::move(request),
                               responder = std::move(responder)]() mutable {
                    api.dispatch(
                        route, std::move(request), [responder](DashboardResponse response) {
                            if (response.file) {
                                responder->send_file(std::move(response.file->path),
                                                     std::move(response.file->content_type),
                                                     std::nullopt,
                                                     std::move(response.file->options));
                            } else {
                                responder->send(response.status, std::move(response.headers),
                                                std::move(response.body));
                            }
                        });
                });
            });
    }
}

} // namespace fv::meeting_web
