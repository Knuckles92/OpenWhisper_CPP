#pragma once

// The meeting runtime's tests (tests/test_meeting_runtime.py): the runtime over a ManualExecutor
// UI actor and a ManualExecutor standing in for its worker threads, a fake engine factory, a
// counting model lease (the controller's release_local_engine / restore_local_engine), the SQL
// repository on a fresh database, and a probe into the runtime's private state, which Python's
// tests reach through underscore attributes.

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "appcore/activity.h"
#include "base/testing/fake_clock.h"
#include "base/testing/manual_executor.h"
#include "db/database.h"
#include "meeting/runtime.h"
#include "meeting_state/schema.h"
#include "meeting_state/sql_repository.h"
#include "meeting_state/store.h"
#include "meeting_web/dashboard_api.h"
#include "pyport/json.h"
#include "settings/store.h"
#include "testkit/testkit.h"

namespace fv::meeting {

// Python's test access to the runtime's internals.
class MeetingRuntimeProbe {
public:
    explicit MeetingRuntimeProbe(MeetingRuntime& runtime) : rt_(runtime) {}

    meeting_web::PostMeetingPasses post_meeting_passes() {
        return rt_.post_meeting_passes(rt_.model_lease_);
    }

    template <class F> auto locked(F&& fn) {
        const std::lock_guard lock(rt_.mutex_);
        return std::forward<F>(fn)(rt_);
    }

    void set_starting(bool value) {
        locked([&](MeetingRuntime& rt) { rt.starting_ = value; });
    }
    bool starting() {
        return locked([](MeetingRuntime& rt) { return rt.starting_; });
    }
    void set_finalizing(bool value) {
        locked([&](MeetingRuntime& rt) { rt.finalizing_ = value; });
    }
    void set_finalization(std::optional<Json> value) {
        locked([&](MeetingRuntime& rt) { rt.finalization_ = std::move(value); });
    }
    std::optional<Json> finalization() {
        return locked([](MeetingRuntime& rt) { return rt.finalization_; });
    }
    void set_card(std::optional<std::string> value) {
        locked([&](MeetingRuntime& rt) { rt.card_meeting_id_ = std::move(value); });
    }
    std::optional<std::string> card() {
        return locked([](MeetingRuntime& rt) { return rt.card_meeting_id_; });
    }
    void set_host_url(std::optional<std::string> value) {
        locked([&](MeetingRuntime& rt) { rt.host_url_ = std::move(value); });
    }
    std::optional<std::string> host_url() {
        return locked([](MeetingRuntime& rt) { return rt.host_url_; });
    }
    void set_guest_url(std::optional<std::string> value) {
        locked([&](MeetingRuntime& rt) { rt.guest_url_ = std::move(value); });
    }
    // Sets the current engine; with `listen` its events reach the runtime as a started
    // engine's do.
    void set_engine(std::shared_ptr<RuntimeEngine> engine, bool listen = false) {
        if (listen && engine) {
            rt_.listen(engine);
        }
        locked([&](MeetingRuntime& rt) { rt.engine_ = std::move(engine); });
    }
    std::shared_ptr<RuntimeEngine> engine() {
        return locked([](MeetingRuntime& rt) { return rt.engine_; });
    }
    void set_controller_active(bool value) {
        locked([&](MeetingRuntime& rt) { rt.controller_meeting_active_ = value; });
    }
    bool controller_active() {
        return locked([](MeetingRuntime& rt) { return rt.controller_meeting_active_; });
    }
    void set_background_ready(bool value) {
        locked([&](MeetingRuntime& rt) { rt.background_ready_ = value; });
    }
    std::vector<std::string> deferred_start_errors() {
        return locked([](MeetingRuntime& rt) { return rt.deferred_start_errors_; });
    }
    std::vector<std::string> background_ids() {
        return locked([](MeetingRuntime& rt) {
            std::vector<std::string> ids;
            for (const auto& entry : rt.background_engines_) {
                ids.push_back(entry.first);
            }
            return ids;
        });
    }
    std::shared_ptr<RuntimeArchive> archive() {
        return locked([](MeetingRuntime& rt) { return rt.archive_; });
    }
    void set_archive(std::shared_ptr<RuntimeArchive> archive) {
        locked([&](MeetingRuntime& rt) { rt.archive_ = std::move(archive); });
    }
    void add_background(const std::string& meeting_id, std::shared_ptr<RuntimeEngine> engine) {
        locked([&](MeetingRuntime& rt) { rt.background_engines_[meeting_id] = std::move(engine); });
    }
    void set_publish_hook(std::function<void(const std::string&, const Json&)> hook) {
        rt_.publish_hook_ = std::move(hook);
    }

    // The private steps Python's tests call directly.
    void on_engine_event(const std::string& kind, const Json& payload) {
        rt_.on_engine_event(engine(), kind, payload);
    }
    // _route_engine_event(engine, kind, payload) with the current generation and no claim.
    void route(const std::shared_ptr<RuntimeEngine>& engine, const std::string& kind,
               const Json& payload) {
        MeetingRuntime::QueuedEvent event;
        event.engine = engine;
        event.kind = kind;
        event.payload = payload;
        event.generation = locked([](MeetingRuntime& rt) { return rt.retry_generation_; });
        rt_.route_engine_event(event);
    }
    // _queue_engine_event: captured now, routed on the UI actor.
    void queue(const std::shared_ptr<RuntimeEngine>& engine, const std::string& kind,
               const Json& payload) {
        rt_.queue_engine_event(engine, kind, payload);
    }
    bool hydrate(const Json& meeting, bool reveal) {
        return rt_.hydrate_finalization_card(meeting, reveal);
    }
    MeetingEngineOptions build_options(bool cloud, bool demo = false) {
        return rt_.make_options(cloud, demo, "auto");
    }
    void launch(bool cloud, bool demo = false, std::string policy = "auto") {
        rt_.launch(cloud, demo, std::move(policy));
    }
    void start_worker(bool cloud, bool demo = false) { rt_.start_worker(cloud, demo, "auto", {}); }
    void recovery_scan_worker() { rt_.recovery_scan_worker(); }
    void restore_last_finalization_worker() { rt_.restore_last_finalization_worker(); }
    void finalize_recovered_worker(const std::string& id) { rt_.finalize_recovered_worker(id); }
    void discard_recovered_worker(const std::string& id) { rt_.discard_recovered_worker(id); }
    void delete_past_meeting_worker(const std::string& id, bool recordings = true) {
        rt_.delete_past_meeting_worker(id, recordings);
    }
    void clear_past_meetings_worker(bool recordings) { rt_.clear_past_meetings_worker(recordings); }
    void open_past_meeting_worker(const std::string& id, const std::string& view = "") {
        rt_.open_past_meeting_worker(id, view);
    }
    Json speaker_rerun_fields() { return rt_.speaker_rerun_fields(); }
    void refresh_past_meetings() { rt_.refresh_past_meetings(); }
    // The dashboard's request to turn AI insights on (DashboardOptions::request_cloud_consent).
    void request_browser_insights(std::string meeting_id, CancelToken permit,
                                  std::function<void(std::optional<std::string>)> finish) {
        rt_.request_browser_insights(std::move(meeting_id), std::move(permit), std::move(finish));
    }

private:
    MeetingRuntime& rt_;
};

namespace runtime_test {

using namespace std::chrono_literals;

// The engine surface the runtime drives, scripted per test.
class FakeEngine final : public RuntimeEngine {
public:
    explicit FakeEngine(std::string id = "m_test") : id_(std::move(id)) {}

    std::string meeting_id() const override {
        const std::lock_guard lock(mutex_);
        return id_;
    }
    void set_meeting_id(std::string id) {
        const std::lock_guard lock(mutex_);
        id_ = std::move(id);
    }
    bool is_active() const override { return active.load(); }
    Subscription add_listener(Executor& executor,
                              std::function<void(const MeetingEvent&)> fn) override {
        auto entry = std::make_shared<Listener>(
            Listener{&executor, std::move(fn), std::make_shared<CallbackGuard>()});
        {
            const std::lock_guard lock(mutex_);
            listeners_.push_back(entry);
        }
        return Subscription(entry->guard, [] {});
    }
    // An event as the engine emits it, from the calling thread.
    void emit(const std::string& kind, const Json& payload = Json::object()) {
        std::vector<std::shared_ptr<Listener>> listeners;
        {
            const std::lock_guard lock(mutex_);
            listeners = listeners_;
        }
        const MeetingEvent event{kind, payload};
        for (const auto& listener : listeners) {
            listener->guard->run([&] { listener->fn(event); });
        }
    }
    Json start() override {
        ++start_calls;
        if (on_start) {
            return on_start();
        }
        return Json{{"meeting_id", meeting_id()}, {"host_url", "http://127.0.0.1:8765/host"}};
    }
    void abort_start() override { ++abort_calls; }
    bool processing_stopped() const override { return stopped.load(); }
    void pause() override {
        ++pause_calls;
        if (on_pause) {
            on_pause();
        }
    }
    void resume() override {
        ++resume_calls;
        if (on_resume) {
            on_resume();
        }
    }
    void end() override {
        ++end_calls;
        if (on_end) {
            on_end();
        }
    }
    bool wait_for_end(std::chrono::milliseconds timeout) override {
        if (on_wait) {
            return on_wait(timeout);
        }
        return true;
    }
    void shutdown() override {
        ++shutdown_calls;
        if (on_shutdown) {
            on_shutdown();
        }
    }
    bool publish_finalization(const Json& finalization) override {
        {
            const std::lock_guard lock(mutex_);
            published_.push_back(finalization);
        }
        if (on_publish) {
            on_publish(finalization);
        }
        return true;
    }
    std::vector<Json> published() const {
        const std::lock_guard lock(mutex_);
        return published_;
    }
    std::shared_ptr<meeting_state::MeetingStateStore> store() const override {
        const std::lock_guard lock(mutex_);
        return store_;
    }
    void set_store(std::shared_ptr<meeting_state::MeetingStateStore> store) {
        const std::lock_guard lock(mutex_);
        store_ = std::move(store);
    }
    // A store without a repository whose finalization is `finalization`.
    void set_finalization(const Json& finalization) {
        meeting_state::MeetingState state;
        state.meeting_id = meeting_id();
        state.status = "ended";
        state.finalization = meeting_state::FinalizationState::coerce(finalization, false, "ended");
        set_store(std::make_shared<meeting_state::MeetingStateStore>(std::move(state)));
    }
    double meeting_time_s() const override { return time_s.load(); }
    void allow_agent_writes() override { ++allow_writes; }
    void revoke_agent_writes() override { ++revoke_writes; }
    std::shared_ptr<DashboardTitles> dashboard() const override { return titles; }
    // Where a live meeting's AI insights would go now; none unless the test sets one.
    std::optional<meeting_agent::MeetingAgentRoute> live_intelligence_destination() const override {
        const std::lock_guard lock(mutex_);
        return destination_;
    }
    void set_destination(std::optional<meeting_agent::MeetingAgentRoute> route) {
        const std::lock_guard lock(mutex_);
        destination_ = std::move(route);
    }
    // Recorded, then applied to the store as the engine does: an approval turns AI insights on,
    // none turns them off.
    void set_live_intelligence(const std::optional<LiveIntelligenceApproval>& approval,
                               const CancelToken&) override {
        std::shared_ptr<meeting_state::MeetingStateStore> store;
        {
            const std::lock_guard lock(mutex_);
            live_requests_.push_back(approval);
            store = store_;
        }
        if (on_live_intelligence) {
            on_live_intelligence(approval);
        }
        if (store) {
            store->update_runtime_fields(Json{{"cloud_enabled", approval.has_value()}});
        }
    }
    std::vector<std::optional<LiveIntelligenceApproval>> live_requests() const {
        const std::lock_guard lock(mutex_);
        return live_requests_;
    }

    std::atomic<bool> active{true};
    std::atomic<bool> stopped{false};
    std::atomic<double> time_s{0.0};
    std::atomic<int> start_calls{0}, abort_calls{0}, pause_calls{0}, resume_calls{0}, end_calls{0},
        shutdown_calls{0}, allow_writes{0}, revoke_writes{0};
    std::function<Json()> on_start;
    std::function<void()> on_pause;
    std::function<void()> on_resume;
    std::function<void()> on_shutdown;
    std::function<void()> on_end;
    std::function<bool(std::chrono::milliseconds)> on_wait;
    std::function<void(const Json&)> on_publish;
    // May throw, as an engine that refuses the change.
    std::function<void(const std::optional<LiveIntelligenceApproval>&)> on_live_intelligence;
    std::shared_ptr<DashboardTitles> titles;

private:
    struct Listener {
        Executor* executor;
        std::function<void(const MeetingEvent&)> fn;
        std::shared_ptr<CallbackGuard> guard;
    };
    mutable std::mutex mutex_;
    std::string id_;
    std::vector<std::shared_ptr<Listener>> listeners_;
    std::vector<Json> published_;
    std::shared_ptr<meeting_state::MeetingStateStore> store_;
    std::optional<meeting_agent::MeetingAgentRoute> destination_;
    std::vector<std::optional<LiveIntelligenceApproval>> live_requests_;
};

// The controller's engine lease: "release" on acquire, "restore" on release.
class CountingLease final : public ModelLease {
public:
    bool acquire() override {
        std::function<void()> hook;
        {
            const std::lock_guard lock(mutex_);
            calls_.push_back("release");
            hook = hook_;
        }
        if (hook) {
            hook();
        }
        return true;
    }
    void release() override {
        const std::lock_guard lock(mutex_);
        calls_.push_back("restore");
    }
    std::vector<std::string> calls() const {
        const std::lock_guard lock(mutex_);
        return calls_;
    }
    void on_acquire(std::function<void()> fn) {
        const std::lock_guard lock(mutex_);
        hook_ = std::move(fn);
    }

private:
    mutable std::mutex mutex_;
    std::vector<std::string> calls_;
    std::function<void()> hook_;
};

// A past meeting's dashboard after a restart, without a server.
class FakeArchive final : public RuntimeArchive {
public:
    FakeArchive(std::string id, std::string url) : id_(std::move(id)), url_(std::move(url)) {}
    std::string meeting_id() const override { return id_; }
    bool is_running() const override { return running.load(); }
    std::string host_url() const override { return running.load() ? url_ : std::string(); }
    void shutdown() override {
        running = false;
        ++shutdown_calls;
    }
    std::shared_ptr<DashboardTitles> dashboard() const override { return titles; }

    std::atomic<bool> running{true};
    std::atomic<int> shutdown_calls{0};
    std::shared_ptr<DashboardTitles> titles;

private:
    std::string id_;
    std::string url_;
};

// Every signal of the runtime, recorded on the UI actor.
struct Recorded {
    std::vector<Json> states;
    std::vector<std::string> statuses;
    std::vector<std::string> errors;
    std::vector<std::string> main_statuses;
    std::vector<Json> servers;
    std::vector<std::vector<Json>> found;
    std::vector<std::string> scan_failures;
    std::vector<std::string> guest_links;
    std::vector<std::string> playback_stops;
    std::vector<std::pair<std::string, std::string>> engine_events; // (meeting id, kind)
    int refreshes = 0;
    int platform_acks = 0;
};

// The runtime fixture: Python's `runtime` fixture, over the SQL repository.
class RuntimeRig {
public:
    explicit RuntimeRig(const std::function<void(MeetingRuntimeDeps&)>& configure = {})
        : clock(std::chrono::sys_days{std::chrono::year{2026} / 11 / 17} + 9h),
          database(std::make_unique<Database>(Database::open_or_create(root.path() / "fv.db"))),
          repo(std::make_shared<meeting_state::SqlMeetingRepository>(*database, clock)),
          settings(root.path() / "settings.json"), lease(std::make_shared<CountingLease>()) {
        MeetingRuntimeDeps deps;
        deps.ui = &ui;
        deps.settings = &settings;
        deps.repository = repo;
        deps.meetings_root = root.path() / "meetings";
        deps.clock = &clock;
        deps.activity = &activity;
        deps.model_lease = lease;
        deps.open_url = [this](std::string_view url) {
            const std::lock_guard lock(mutex_);
            opened.emplace_back(url);
            return true;
        };
        deps.spawn = [this](std::string name, Task task) {
            {
                const std::lock_guard lock(mutex_);
                spawned.push_back(name);
            }
            if (divert && divert(name, task)) {
                return;
            }
            workers.post(std::move(task));
        };
        deps.engine_factory =
            [this](MeetingEngineOptions options) -> std::shared_ptr<RuntimeEngine> {
            std::shared_ptr<FakeEngine> engine;
            {
                const std::lock_guard lock(mutex_);
                built.push_back(options);
                engine = make_engine ? make_engine(options) : std::make_shared<FakeEngine>();
                engines.push_back(engine);
            }
            return engine;
        };
        deps.archive_factory = [this](const Json& meeting, const std::string&,
                                      std::uint16_t port) -> std::shared_ptr<RuntimeArchive> {
            archive_ports.push_back(port);
            auto archive = std::make_shared<FakeArchive>(meeting.value("id", std::string()),
                                                         "http://127.0.0.1:9100/m/archive-token");
            archives.push_back(archive);
            return archive;
        };
        deps.rerun = [this](const std::string& meeting_id, const RerunRequest& request) {
            if (!rerun) {
                FAIL("no re-run pipeline was expected");
            }
            return rerun(meeting_id, request);
        };
        if (configure) {
            configure(deps);
        }
        deps_ = deps;
        rt = std::make_unique<MeetingRuntime>(std::move(deps));
        connect();
    }
    ~RuntimeRig() {
        subscriptions.clear();
        rt.reset();
    }

    // An app restart over the same database and settings: a new runtime, nothing recorded yet.
    void restart() {
        subscriptions.clear();
        rt.reset();
        got = Recorded{};
        rt = std::make_unique<MeetingRuntime>(deps_);
        connect();
    }

    MeetingRuntimeProbe probe() { return MeetingRuntimeProbe(*rt); }

    // Runs the workers and the UI actor until neither has anything left.
    void drain() {
        while (workers.run_all() + ui.run_all() > 0) {
        }
    }

    std::vector<std::string> spawned_names() {
        const std::lock_guard lock(mutex_);
        return spawned;
    }
    std::vector<std::string> opened_urls() {
        const std::lock_guard lock(mutex_);
        return opened;
    }

    // A meeting row (OpenWhisper's make_meeting), ended unless `status` says otherwise.
    void make_meeting(const std::string& id, const Json& extra = Json::object(),
                      const std::string& status = "ended") {
        Json fields{{"id", id},
                    {"title", ""},
                    {"status", status},
                    {"started_at", "2026-11-17T09:00:00"},
                    {"host_token", "host-" + id},
                    {"guest_token", "guest-" + id},
                    {"cloud_enabled", false},
                    {"spool_dir", ""}};
        for (auto it = extra.begin(); it != extra.end(); ++it) {
            fields[it.key()] = it.value();
        }
        repo->create_meeting(fields);
    }

    // Python's _ended_meeting: an ended meeting with AI insights on and a finalization.
    void make_ended_meeting(const std::string& id, bool deferred = false,
                            const std::string& status = "failed",
                            const std::string& started_at = "2026-08-19T18:00:00",
                            const Json& extra = Json::object()) {
        meeting_state::MeetingState state;
        state.meeting_id = id;
        state.status = "ended";
        state.cloud_enabled = true;
        state.finalization.status = status;
        state.finalization.message = "boom";
        state.finalization.card_deferred = deferred;
        Json fields{{"started_at", started_at},
                    {"cloud_enabled", true},
                    {"state_json", py::dumps(state.to_json())}};
        for (auto it = extra.begin(); it != extra.end(); ++it) {
            fields[it.key()] = it.value();
        }
        make_meeting(id, fields);
    }

    Json saved_finalization(const std::string& id) {
        const Json row = *repo->get_meeting(id);
        return py::loads(row["state_json"].get<std::string>())["finalization"];
    }

    testkit::TempDataRoot root;
    FakeClock clock;
    std::unique_ptr<Database> database;
    std::shared_ptr<meeting_state::SqlMeetingRepository> repo;
    SettingsStore settings;
    ActivityState activity;
    ManualExecutor ui;
    ManualExecutor workers;
    std::shared_ptr<CountingLease> lease;
    std::function<std::shared_ptr<FakeEngine>(const MeetingEngineOptions&)> make_engine;
    std::function<Json(const std::string&, const RerunRequest&)> rerun;
    // Takes a worker's task instead of the manual worker queue when it returns true (a test
    // that needs a worker on a real thread).
    std::function<bool(const std::string& name, Task& task)> divert;
    std::vector<MeetingEngineOptions> built;
    std::vector<std::shared_ptr<FakeEngine>> engines;
    std::vector<std::shared_ptr<FakeArchive>> archives;
    std::vector<std::uint16_t> archive_ports;
    Recorded got;
    std::unique_ptr<MeetingRuntime> rt;
    std::vector<Subscription> subscriptions;

private:
    void connect() {
        auto& r = *rt;
        subscriptions.push_back(
            r.meeting_state_changed.connect(ui, [this](Json s) { got.states.push_back(s); }));
        subscriptions.push_back(r.meeting_status_update.connect(
            ui, [this](std::string s) { got.statuses.push_back(s); }));
        subscriptions.push_back(
            r.meeting_error.connect(ui, [this](std::string s) { got.errors.push_back(s); }));
        subscriptions.push_back(
            r.status_update.connect(ui, [this](std::string s) { got.main_statuses.push_back(s); }));
        subscriptions.push_back(
            r.meeting_server_started.connect(ui, [this](Json s) { got.servers.push_back(s); }));
        subscriptions.push_back(r.meeting_recovery_found.connect(
            ui, [this](std::vector<Json> m) { got.found.push_back(m); }));
        subscriptions.push_back(r.meeting_recovery_scan_failed.connect(
            ui, [this](std::string s) { got.scan_failures.push_back(s); }));
        subscriptions.push_back(r.meeting_guest_link_ready.connect(
            ui, [this](std::string s) { got.guest_links.push_back(s); }));
        subscriptions.push_back(r.playback_stop_requested.connect(
            ui, [this](std::string s) { got.playback_stops.push_back(s); }));
        subscriptions.push_back(
            r.past_meetings_refresh_requested.connect(ui, [this] { ++got.refreshes; }));
        subscriptions.push_back(
            r.meeting_platform_ack_requested.connect(ui, [this] { ++got.platform_acks; }));
        subscriptions.push_back(
            r.meeting_engine_event.connect(ui, [this](std::string id, std::string kind, Json) {
                got.engine_events.emplace_back(id, kind);
            }));
    }

    std::mutex mutex_;
    std::vector<std::string> spawned;
    std::vector<std::string> opened;
    MeetingRuntimeDeps deps_;
};

// Runs `fn` as the UI actor (a public method's caller).
template <class F> void on_ui(ManualExecutor& ui, F&& fn) {
    ui.post(std::forward<F>(fn));
    ui.run_all();
}

} // namespace runtime_test
} // namespace fv::meeting
