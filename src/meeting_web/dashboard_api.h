#pragma once

#include <chrono>
#include <cstddef>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "base/cancel.h"
#include "base/clock.h"
#include "base/executor.h"
#include "base/json.h"
#include "meeting_state/data_lifecycle.h"
#include "meeting_state/repository.h"
#include "meeting_state/sql_repository.h"
#include "meeting_state/store.h"
#include "meeting_web/dashboard_engine.h"
#include "net/http_server.h"

namespace fv {
class SettingsStore;
}

// meeting/web/api.py: the dashboard's REST routes, its page and its bundle's assets, over the
// engine surface (DashboardEngine) and the meeting repository. Every handled error is FastAPI's
// {"detail": <string>}; query parameters are validated as FastAPI validates them (422 with a
// list detail); a missing token is 401 "invalid token", and only /m/{token} answers 403.
//
// The handlers are pure: a request in, a DashboardResponse out (a status, headers and a body, or
// a file the server streams). install_dashboard_routes puts them on a net::HttpServer.
namespace fv::meeting_web {

class DashboardExtensions;
class MeetingWebServer;

// An answer of FastAPI's HTTPException: {"detail": detail}. what() is the detail as text.
class HttpError : public std::runtime_error {
public:
    HttpError(int status, Json detail);
    int status() const { return status_; }
    const Json& detail() const { return detail_; }

private:
    int status_;
    Json detail_;
};

struct DashboardResponse {
    // A file the server streams (FileResponse, StaticFiles): Range, If-Range, ETag,
    // Last-Modified and, with options.not_modified_check, 304.
    struct File {
        std::filesystem::path path;
        std::string content_type; // "": guessed from the path
        net::FileOptions options;
    };

    int status = 200;
    // Lower-case names in Starlette's order, without content-length (the server's).
    net::Headers headers;
    std::string body;
    std::optional<File> file; // set: the file is the answer and body is unused

    // JSONResponse: compact, non-ASCII kept, application/json. Throws py::ValueError for a NaN
    // or an infinity (allow_nan=False), which the route answers 500.
    static DashboardResponse json(const Json& body, int status = 200);
    // {"detail": detail}.
    static DashboardResponse error(int status, const Json& detail);
};

// The page of Past Meetings the History route reads (lead amendment 26: MeetingRepository does
// not grow; SqlMeetingRepository answers it).
// Affinity: implementations are thread-safe.
class PastMeetingPages {
public:
    virtual ~PastMeetingPages() = default;
    virtual std::vector<Json>
    list_past_meeting_summaries(const meeting_state::PastMeetingQuery& query) = 0;
};

// The adapter over the SQL repository.
std::shared_ptr<PastMeetingPages>
sql_past_meeting_pages(std::shared_ptr<meeting_state::SqlMeetingRepository> repository);

// A post-meeting pass of the same meeting is already running elsewhere (refinalize's
// FinalizationBusyError); the route answers 409 with what().
class PostMeetingPassBusy : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

// The re-run passes behind /reinsights and /respeakers, provided by the meeting runtime over
// meeting::rerun_finalization and meeting::rerun_speakers (this library does not depend on
// fv_meeting). They run on DashboardOptions::reruns with the meeting claimed. A pass throws
// PostMeetingPassBusy for 409 and py::ValueError for 400 (both with their message); anything
// else answers 500. An empty function answers 500.
struct PostMeetingPasses {
    // Retries the post-meeting steps from `from_step` ("failed": the earliest failed one) with
    // the meeting's rerun options and the engine's model lease; returns {ok, state, applied,
    // error, ...}. The route hands a returned "state" to the store that serves the meeting.
    std::function<Json(const std::string& meeting_id, const std::string& from_step,
                       const CancelToken&)>
        rerun_insights;
    // Relabels a past meeting's speakers through `store` (the live store of a just-ended current
    // meeting, its cached review store, or null: the pass opens its own); returns
    // run_speaker_pass's result plus "state".
    std::function<Json(const std::string& meeting_id,
                       std::shared_ptr<meeting_state::MeetingStateStore> store)>
        rerun_speakers;
    // Why the saved settings refuse the speaker pass now (resolve_speaker_pass's reason), or
    // nullopt when it may run. Empty: never refused.
    std::function<std::optional<std::string>(const Json& meeting)> speaker_gate_refusal;
    // Optional review of an ended meeting, using its single persisted-state writer.
    std::function<Json(std::shared_ptr<meeting_state::MeetingStateStore>)> review;
    std::function<Json(std::string_view)> semantic_search;
    // Claims a report and schedules its writer on the dashboard's post-meeting worker.
    std::function<Json(std::shared_ptr<meeting_state::MeetingStateStore>, const Json&,
                       std::string_view, Executor&, const CancelToken&)>
        report;
};

using CloudConsentFinished = std::function<void(std::optional<std::string>)>;

struct DashboardOptions {
    // The built bundle (index.html, assets/); empty: default_dist_root().
    std::filesystem::path dist_root;
    // Where a deleted meeting's spool must lie when the engine names no spool root.
    std::filesystem::path meetings_root;
    // Where the blocking handlers run (asyncio.to_thread's pool); several requests run at once,
    // so a one-wide strand would queue a page load behind a long export. Null: four threads of
    // the API's own.
    Executor* io = nullptr;
    // Where re-runs run, one at a time (the "meeting-reinsights" single-thread executor), so a
    // long agent pass never holds the threads every other request needs. Null: one thread of
    // the API's own.
    Executor* reruns = nullptr;
    // Told once a deleted meeting's rows are gone (delete_meeting_data's on_deleted).
    meeting_state::MeetingDeleted on_deleted;
    // Only the live desktop bridge can ask the local host to authorize a destination.
    std::function<void(std::string, CancelToken, CloudConsentFinished)> request_cloud_consent;
};

// One request as a handler sees it: a copy of the server's, with when it reached the server.
struct DashboardRequest {
    net::HttpRequest http;
    std::chrono::steady_clock::time_point received{};
};

// The routes' shared context (api.py _AppContext): the engine, the repository, the claims that
// keep two post-meeting passes off one meeting (insights_running) and the cached review stores
// that keep one writer per past meeting. The extensions (M8's review, report, note, AI toggle
// and semantic routes) reach them through the public members.
//
// The engine, the repository, the server and the settings must outlive the API; the API must
// outlive the server's routes (stop the server first), and options.io and options.reruns must
// run none of its tasks after it is destroyed (shut them down first). retitle_saved_meeting and
// refresh_saved_meeting_title are not called from options.io.
// Affinity: thread-safe.
class DashboardApi {
public:
    using Reply = std::function<void(DashboardResponse)>;
    using Handler = void (DashboardApi::*)(const DashboardRequest&, Reply);

    struct Route {
        std::string method;
        std::string pattern; // net::HttpServer's: "{name}" one segment, "{name:path}" the rest
        Handler handler;
        std::size_t max_body = 1 << 20; // bytes
    };

    DashboardApi(DashboardEngine& engine,
                 std::shared_ptr<meeting_state::MeetingRepository> repository,
                 std::shared_ptr<PastMeetingPages> past_meetings, MeetingWebServer& server,
                 PostMeetingPasses passes, DashboardOptions options, SettingsStore& settings,
                 const Clock& clock = system_clock());
    ~DashboardApi();
    DashboardApi(const DashboardApi&) = delete;
    DashboardApi& operator=(const DashboardApi&) = delete;

    // M8's routes; null (M7) answers them as described at each route.
    void set_extensions(std::shared_ptr<DashboardExtensions> extensions);
    std::shared_ptr<DashboardExtensions> extensions() const;

    // The rename a local caller (MCP, the desktop) asks for, run as the rename route runs it, on
    // options.io; waits at most 15 s. True when the rename applied. Throws HttpError (400 for a
    // title normalize_title refuses, 404 for an unknown meeting), std::runtime_error("Meeting
    // dashboard is unavailable.") while the server is not running, and on the timeout.
    bool retitle_saved_meeting(const std::string& meeting_id, const std::string& title);
    // Another owner renamed the meeting: the live store and the cached review store adopt the
    // row's title, and a changed live title makes every socket reconnect (4409, the token stays
    // valid). Nothing while the server is not running; waits at most 15 s.
    void refresh_saved_meeting_title(const std::string& meeting_id);

    // api.py's routes in its registration order (create_app 607-619), /assets first when the
    // bundle has an assets folder.
    std::vector<Route> routes() const;
    // Runs one route: blocking work on the calling thread, a re-run on options.reruns; `reply`
    // is called once, from either. HttpError answers its detail, anything else 500.
    void dispatch(const Route& route, DashboardRequest request, Reply reply);
    // Finds the route as the server would (a known path with another method is 405, an unknown
    // one 404) and waits for its answer. For tests and tools.
    DashboardResponse call(std::string_view method, std::string_view path,
                           const net::Query& query = {}, std::string body = {},
                           const net::Headers& headers = {});

    // ---- the shared context (api.py _AppContext) ----
    DashboardEngine& engine() { return engine_; }
    const std::shared_ptr<meeting_state::MeetingRepository>& repository() const {
        return repository_;
    }
    MeetingWebServer& server() { return server_; }
    SettingsStore& settings() { return settings_; }
    const Clock& clock() const { return clock_; }
    const std::filesystem::path& dist_root() const { return dist_root_; }
    // Where the blocking handlers run (options.io, or the API's own pool).
    Executor& io() { return *io_; }
    // The settings as one mapping (what load_state and the speaker gate read).
    Json settings_snapshot() const;

    bool is_current(std::string_view meeting_id) const;
    bool is_active_meeting(std::string_view meeting_id) const;
    // The engine's store when `meeting_id` is the current meeting.
    std::shared_ptr<meeting_state::MeetingStateStore> live_store(std::string_view meeting_id) const;
    // The engine's store for the current meeting, else the cached review store (may be null).
    std::shared_ptr<meeting_state::MeetingStateStore>
    meeting_store(std::string_view meeting_id) const;
    // The one store allowed to write the meeting: the live one, else the cached review store,
    // opened (historical) and cached on first use.
    std::shared_ptr<meeting_state::MeetingStateStore> owning_store(const std::string& meeting_id,
                                                                   const Json& meeting);
    void drop_review_store(std::string_view meeting_id);
    // The current meeting's row, or nullopt without one.
    std::optional<Json> current_meeting() const;
    // "host" or "guest" for the token, against the current meeting's tokens.
    std::optional<std::string> resolve(std::string_view token) const;
    // The role; HttpError 401 "invalid token", or 403 "host only".
    std::string require(std::string_view token, bool host_only = false) const;
    // require, then 403 "guest access is current meeting only" for a guest asking for another
    // meeting.
    std::string require_meeting(std::string_view token, std::string_view meeting_id) const;
    // The repository row; HttpError 404 "unknown meeting".
    Json saved_meeting(std::string_view meeting_id) const;
    // The live snapshot, else the cached review store's, else the stored document upgraded to
    // the current wire shape (historical).
    Json state_for(const std::string& meeting_id, const Json& meeting) const;
    // {items, next_cursor}: a keyset page of the meeting's segments (limit clamped to 1..1000);
    // HttpError 400 "invalid transcript cursor".
    Json transcript_page(const std::string& meeting_id, std::string_view cursor,
                         std::int64_t limit) const;
    // insights_running: true when this call claimed the meeting (false when it was claimed).
    bool claim(const std::string& meeting_id);
    void release(const std::string& meeting_id);
    bool is_claimed(std::string_view meeting_id) const;
    // The request's JSON body as an object; HttpError 400 "invalid JSON body" or "body must be
    // an object".
    static Json json_body(const net::HttpRequest& request);

private:
    struct Owned;

    // routes_session.cpp
    void page(const DashboardRequest&, Reply);
    void asset(const DashboardRequest&, Reply);
    void session(const DashboardRequest&, Reply);
    void transcript(const DashboardRequest&, Reply);
    // routes_history.cpp
    void meetings(const DashboardRequest&, Reply);
    void meeting_detail(const DashboardRequest&, Reply);
    void meeting_state(const DashboardRequest&, Reply);
    void search(const DashboardRequest&, Reply);
    void events(const DashboardRequest&, Reply);
    // routes_files.cpp
    void meeting_transcript(const DashboardRequest&, Reply);
    void segment(const DashboardRequest&, Reply);
    void audio(const DashboardRequest&, Reply);
    void export_meeting(const DashboardRequest&, Reply);
    void delete_report(const DashboardRequest&, Reply);
    void download_report(const DashboardRequest&, Reply);
    // routes_meeting.cpp
    void rename(const DashboardRequest&, Reply);
    void review(const DashboardRequest&, Reply);
    void rerun_insights(const DashboardRequest&, Reply);
    void rerun_speakers(const DashboardRequest&, Reply);
    void delete_meeting(const DashboardRequest&, Reply);
    void request_report(const DashboardRequest&, Reply);
    // routes_control.cpp
    void note_request(const DashboardRequest&, Reply);
    void end(const DashboardRequest&, Reply);
    void pause(const DashboardRequest&, Reply);
    void resume(const DashboardRequest&, Reply);
    void cloud(const DashboardRequest&, Reply);
    void cancel_cloud_prompt();
    void regenerate_tokens(const DashboardRequest&, Reply);

    // retitle_saved_meeting's work ({ok, title}), as the rename route runs it.
    Json retitle(const std::string& meeting_id, const Json& title);
    void refresh_title(const std::string& meeting_id);
    // Runs `fn` on options.io and waits at most 15 s for it.
    Json run_on_io(std::function<Json()> fn);
    // _refuse_rerun_while_busy: HttpError 409 while the store's finalization, insight review or
    // a report is running.
    static void refuse_rerun_while_busy(const meeting_state::MeetingStateStore* store);
    // run_post_meeting_pass: `job` on options.reruns with the meeting already claimed; `adopt`
    // sees the result first.
    void run_post_meeting_pass(const std::string& meeting_id, std::function<Json()> job,
                               std::function<void(const Json&)> adopt, Reply reply);

    DashboardEngine& engine_;
    std::shared_ptr<meeting_state::MeetingRepository> repository_;
    std::shared_ptr<PastMeetingPages> past_meetings_;
    MeetingWebServer& server_;
    PostMeetingPasses passes_;
    DashboardOptions options_;
    SettingsStore& settings_;
    const Clock& clock_;
    std::filesystem::path dist_root_;

    mutable std::mutex mutex_; // guards the members below
    std::shared_ptr<DashboardExtensions> extensions_;
    std::set<std::string, std::less<>> insights_running_;
    std::map<std::string, std::shared_ptr<meeting_state::MeetingStateStore>, std::less<>>
        review_stores_;

    std::optional<CancelSource> cloud_prompt_;
    CancelSource saved_pass_stop_;

    // Last: the executors of the API's own (when options give none) are joined first.
    std::unique_ptr<Owned> owned_;
    Executor* io_ = nullptr;
    Executor* reruns_ = nullptr;
    Executor* reports_ = nullptr;
};

// _filter_history_page: the public rows (public_meeting without a repository) of one History
// page that pass the digest filters: decisions > 0, action items > 0, insights_tone "warning".
Json filter_history_page(const std::vector<Json>& page, bool has_decisions, bool has_actions,
                         bool needs_attention, const Clock& clock = system_clock());

// Puts the dashboard's routes on `server` (MeetingWebServer's route installer): each handler is
// copied off the reactor onto options.io. The API must outlive the server's routes.
void install_dashboard_routes(net::HttpServer& server, DashboardApi& api);

} // namespace fv::meeting_web
