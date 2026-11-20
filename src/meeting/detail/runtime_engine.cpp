// MeetingRuntime: the app's meeting engine, archive dashboard and redecode ASR, each with the
// dashboard's REST API (meeting_web::DashboardApi) installed on its server.

#include <exception>
#include <mutex>
#include <stdexcept>
#include <utility>

#include "meeting/detail/runtime_internal.h"
#include "meeting/runtime.h"
#include "meeting_capture/asr_engine.h"
#include "meeting_web/archive.h"
#include "meeting_web/dashboard_api.h"
#include "meeting_web/server.h"
#include "net/reactor.h"
#include "settings/store.h"

namespace fv::meeting {

namespace {

// A running dashboard's titles while `guard` is open; once it closes, the server and the API
// they reach may be gone: the calls then do nothing (a rename says the dashboard is unavailable).
class GuardedTitles final : public DashboardTitles {
public:
    GuardedTitles(std::shared_ptr<DashboardTitles> inner, std::shared_ptr<CallbackGuard> guard)
        : inner_(std::move(inner)), guard_(std::move(guard)) {}

    bool is_running() const override {
        bool running = false;
        guard_->run([&] { running = inner_->is_running(); });
        return running;
    }
    bool retitle_saved_meeting(const std::string& meeting_id, const std::string& title) override {
        bool applied = false;
        if (!guard_->run([&] { applied = inner_->retitle_saved_meeting(meeting_id, title); })) {
            throw std::runtime_error("Meeting dashboard is unavailable.");
        }
        return applied;
    }
    void refresh_saved_meeting_title(const std::string& meeting_id) override {
        guard_->run([&] { inner_->refresh_saved_meeting_title(meeting_id); });
    }

private:
    std::shared_ptr<DashboardTitles> inner_;
    std::shared_ptr<CallbackGuard> guard_;
};

// A dashboard server with its REST API, filled in when the server installs its routes.
struct DashboardSite {
    std::mutex mutex;
    std::unique_ptr<meeting_web::DashboardApi> api;
    std::shared_ptr<DashboardTitles> titles;
    // Open while the server and the API exist for title calls (retire closes it).
    std::shared_ptr<CallbackGuard> guard = std::make_shared<CallbackGuard>();

    std::shared_ptr<DashboardTitles> current() {
        const std::lock_guard lock(mutex);
        return titles;
    }
    void install(std::unique_ptr<meeting_web::DashboardApi> installed,
                 std::shared_ptr<DashboardTitles> installed_titles) {
        const std::lock_guard lock(mutex);
        titles = std::make_shared<GuardedTitles>(std::move(installed_titles), guard);
        api = std::move(installed);
    }
    // Before the server goes: no title call reaches it any more, and the ones in flight have
    // returned (a rename waits at most 15 s).
    void retire() {
        guard->close();
        const std::lock_guard lock(mutex);
        titles.reset();
    }
    // Stops the API's own threads while what they call into still exists.
    void release() {
        retire();
        std::unique_ptr<meeting_web::DashboardApi> released;
        {
            const std::lock_guard lock(mutex);
            released = std::move(api);
        }
    }
};

// Past Meetings pages for a runtime without the SQL repository's adapter.
class NoPastMeetings final : public meeting_web::PastMeetingPages {
public:
    std::vector<Json> list_past_meeting_summaries(const meeting_state::PastMeetingQuery&) override {
        return {};
    }
};

class EngineAdapter final : public RuntimeEngine {
public:
    EngineAdapter(std::shared_ptr<DashboardSite> site, std::unique_ptr<MeetingEngine> engine)
        : site_(std::move(site)), engine_(std::move(engine)) {}
    // On one of the engine's own threads (an inline listener held the last reference), the
    // shutdown and both destructions move to a thread of their own: ~MeetingEngine joins them.
    ~EngineAdapter() override {
        const MeetingEngine& engine = *engine_;
        auto parts = std::make_shared<Parts>(Parts{std::move(site_), std::move(engine_)});
        release_off_engine_threads(engine, [parts] {
            try {
                shutdown(*parts->site, *parts->engine);
            } catch (const std::exception& error) {
                detail::runtime_logger.warning("Error shutting down meeting engine: {}",
                                               error.what());
            }
            // The engine (and its server) goes before the API that serves it.
            parts->engine.reset();
            parts->site.reset();
        });
    }
    EngineAdapter(const EngineAdapter&) = delete;
    EngineAdapter& operator=(const EngineAdapter&) = delete;

    std::string meeting_id() const override { return engine_->meeting_id(); }
    bool is_active() const override { return engine_->is_active(); }
    Subscription add_listener(Executor& executor,
                              std::function<void(const MeetingEvent&)> fn) override {
        return engine_->add_listener(executor, std::move(fn));
    }
    Json start() override { return engine_->start(); }
    // MeetingEngine unwinds a start it could not finish itself; a start that finished without
    // a dashboard link is shut down.
    void abort_start() override { shutdown(); }
    // End claims the meeting before anything else, so a meeting no longer active after start()
    // returned was ended by its host.
    bool processing_stopped() const override { return !engine_->is_active(); }
    void pause() override { engine_->pause(); }
    void resume() override { engine_->resume(); }
    void end() override { engine_->end(); }
    std::optional<meeting_agent::MeetingAgentRoute> live_intelligence_destination() const override {
        return engine_->live_intelligence_destination();
    }
    void set_live_intelligence(const std::optional<LiveIntelligenceApproval>& approval,
                               const CancelToken& permit) override {
        if (approval) {
            engine_->enable_live_intelligence(approval->primary, approval->backup, permit);
        } else {
            engine_->set_cloud_enabled(false);
        }
    }
    bool wait_for_end(std::chrono::milliseconds timeout) override {
        return engine_->wait_for_end(timeout);
    }
    // The engine's shutdown stops its dashboard server and destroys it, so the title calls that
    // reach the server end first; the API, which serves the engine's routes, goes once the
    // server has stopped. The engine both call into outlives them.
    void shutdown() override { shutdown(*site_, *engine_); }
    bool publish_finalization(const Json& finalization) override {
        return engine_->publish_finalization(finalization);
    }
    std::shared_ptr<meeting_state::MeetingStateStore> store() const override {
        return engine_->store();
    }
    double meeting_time_s() const override { return engine_->clock().now_s(); }
    void allow_agent_writes() override { engine_->allow_agent_writes(); }
    void revoke_agent_writes() override { engine_->revoke_agent_writes(); }
    std::shared_ptr<DashboardTitles> dashboard() const override { return site_->current(); }

private:
    struct Parts {
        std::shared_ptr<DashboardSite> site;
        std::unique_ptr<MeetingEngine> engine;
    };

    static void shutdown(DashboardSite& site, MeetingEngine& engine) {
        site.retire();
        engine.shutdown();
        site.release();
    }

    // Declared first: the engine (and its server) goes before the API that serves it.
    std::shared_ptr<DashboardSite> site_;
    std::unique_ptr<MeetingEngine> engine_;
};

class ArchiveAdapter final : public RuntimeArchive {
public:
    ArchiveAdapter(std::shared_ptr<DashboardSite> site,
                   std::unique_ptr<meeting_web::ArchivedMeetingDashboard> archive,
                   std::unique_ptr<meeting_web::MeetingWebServer> server)
        : site_(std::move(site)), archive_(std::move(archive)), server_(std::move(server)) {}
    ~ArchiveAdapter() override {
        try {
            shutdown();
        } catch (const std::exception& error) {
            detail::runtime_logger.debug("Error shutting down archive dashboard: {}", error.what());
        }
        site_->release();
    }

    std::string meeting_id() const override { return archive_->meeting_id(); }
    bool is_running() const override { return archive_->is_running(); }
    std::string host_url() const override { return archive_->host_url(); }
    std::shared_ptr<meeting_state::MeetingStateStore> store() const override {
        return archive_->store();
    }
    void shutdown() override { archive_->shutdown(); }
    std::shared_ptr<DashboardTitles> dashboard() const override { return site_->current(); }

private:
    std::shared_ptr<DashboardSite> site_;
    std::unique_ptr<meeting_web::ArchivedMeetingDashboard> archive_;
    std::unique_ptr<meeting_web::MeetingWebServer> server_;
};

} // namespace

EngineFactory MeetingRuntime::default_engine_factory() {
    return [this](MeetingEngineOptions options) -> std::shared_ptr<RuntimeEngine> {
        if (deps_.settings == nullptr) {
            throw std::runtime_error("Meeting Mode has no settings to start from.");
        }
        auto site = std::make_shared<DashboardSite>();
        auto pages = deps_.past_meetings ? deps_.past_meetings : std::make_shared<NoPastMeetings>();
        meeting_web::DashboardOptions dashboard;
        dashboard.meetings_root = deps_.meetings_root;
        dashboard.on_deleted = [this](std::string id, bool has_origin) {
            forget_deleted(id, has_origin);
        };
        if (deps_.confirm_browser_insights) {
            dashboard.request_cloud_consent = [this](std::string id, CancelToken permit,
                                                     auto finish) {
                request_browser_insights(std::move(id), std::move(permit), std::move(finish));
            };
        }
        const auto passes = post_meeting_passes(model_lease_);
        EngineDeps engine_deps;
        engine_deps.repository = deps_.repository;
        engine_deps.speech = deps_.speech;
        if (options.asr_remote) {
            if (!deps_.live_remote_backend)
                throw EngineError("Paired meeting speech is unavailable.");
            auto pinned = std::make_shared<Json>(*options.asr_remote);
            auto service = std::make_shared<speech::SpeechService>(
                [factory = deps_.live_remote_backend, pinned](const speech::BackendConfig&) {
                    return factory(pinned);
                },
                ui_);
            speech::BackendConfig config;
            config.backend = "remote";
            service->select(speech::Slot::Meeting, std::move(config));
            const auto cancel = shutdown_.token();
            for (;;) {
                if (cancel.cancelled())
                    throw EngineError("Meeting transcription stopped.");
                const auto ready = service->readiness(speech::Slot::Meeting);
                if (ready.state == speech::ReadyState::Ready)
                    break;
                if (ready.state != speech::ReadyState::Loading) {
                    // asr/remote.py ensure_ready: the backend's last error, else the host named.
                    const auto named = pinned->value("host_name", std::string());
                    throw EngineError(
                        ready.message.empty()
                            ? "Cannot connect to " +
                                  (named.empty() ? std::string("the paired computer") : named) + "."
                            : ready.message);
                }
                cancel.wait_for(std::chrono::milliseconds(25));
            }
            options.asr_remote = *pinned;
            options.asr_model = pinned->at("model").get<std::string>();
            engine_deps.speech = service.get();
            engine_deps.live_remote_speech = std::move(service);
        }
        engine_deps.audio = deps_.audio;
        engine_deps.reactor = deps_.reactor;
        engine_deps.clock = &clock_;
        engine_deps.intelligence = intelligence_;
        engine_deps.local_fast_judge = deps_.local_fast_judge;
        engine_deps.local_review_judge = deps_.local_review_judge;
        engine_deps.model_lease = options.asr_remote ? nullptr : model_lease_;
        engine_deps.strand = deps_.strand;
        engine_deps.settings = [this] {
            return settings_snapshot();
        };
        engine_deps.install_routes = [this, site, pages, passes,
                                      dashboard](net::HttpServer& http,
                                                 meeting_web::MeetingWebServer& server) {
            auto api = std::make_unique<meeting_web::DashboardApi>(
                server.engine(), deps_.repository, pages, server, passes, dashboard,
                *deps_.settings, clock_);
            meeting_web::install_dashboard_routes(http, *api);
            auto titles = dashboard_titles(server, *api);
            site->install(std::move(api), std::move(titles));
        };
        return std::make_shared<EngineAdapter>(
            site, std::make_unique<MeetingEngine>(std::move(options), std::move(engine_deps)));
    };
}

ArchiveFactory MeetingRuntime::default_archive_factory() {
    return [this](const Json& meeting, const std::string& bind,
                  std::uint16_t port) -> std::shared_ptr<RuntimeArchive> {
        if (deps_.reactor == nullptr || deps_.settings == nullptr) {
            throw std::runtime_error("The meeting dashboard is not available.");
        }
        auto site = std::make_shared<DashboardSite>();
        auto pages = deps_.past_meetings ? deps_.past_meetings : std::make_shared<NoPastMeetings>();
        meeting_web::DashboardOptions dashboard;
        dashboard.meetings_root = deps_.meetings_root;
        dashboard.on_deleted = [this](std::string id, bool has_origin) {
            forget_deleted(id, has_origin);
        };
        const auto passes = post_meeting_passes(model_lease_);
        auto archive = std::make_unique<meeting_web::ArchivedMeetingDashboard>(
            deps_.repository, meeting, deps_.meetings_root, settings_snapshot(), clock_);
        auto server = std::make_unique<meeting_web::MeetingWebServer>(
            *deps_.reactor, *archive, deps_.repository, bind, port,
            [this, site, pages, passes, dashboard](net::HttpServer& http,
                                                   meeting_web::MeetingWebServer& owner) {
                auto api = std::make_unique<meeting_web::DashboardApi>(
                    owner.engine(), deps_.repository, pages, owner, passes, dashboard,
                    *deps_.settings, clock_);
                meeting_web::install_dashboard_routes(http, *api);
                auto titles = dashboard_titles(owner, *api);
                site->install(std::move(api), std::move(titles));
            });
        server->set_settings_source([this] { return settings_snapshot(); });
        const auto started = server->start();
        if (!started) {
            throw std::runtime_error(started.error());
        }
        archive->attach_server(server.get());
        return std::make_shared<ArchiveAdapter>(std::move(site), std::move(archive),
                                                std::move(server));
    };
}

AsrFactory MeetingRuntime::default_asr_factory() {
    return [this](const std::string& meeting_id, const std::string& /*model*/,
                  const std::string& language) -> std::unique_ptr<MeetingAsrEngine> {
        const auto row = deps_.repository->get_meeting(meeting_id);
        if (row && row->contains("asr_remote_json") && !row->at("asr_remote_json").is_null()) {
            throw EngineError(
                "Saved remote meeting audio requires a separate playback or transcription choice.");
        }
        // The model is the speech service's Meeting slot (the dictation engine unless the
        // meeting has one of its own).
        if (deps_.speech == nullptr) {
            return nullptr;
        }
        return std::make_unique<MeetingAsrEngine>(*deps_.speech, deps_.repository, meeting_id,
                                                  language, clock_);
    };
}

} // namespace fv::meeting
