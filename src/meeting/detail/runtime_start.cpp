// MeetingRuntime: starting a meeting, the start worker, and pause, resume and end
// (services/runtime/meeting.py start_meeting, _begin_start, on_consent_result, _launch,
// _start_worker, _start_failed, _adopt_started_engine, _announce_started, _build_options,
// pause_meeting, resume_meeting, end_meeting).

#include <exception>
#include <format>
#include <stdexcept>
#include <utility>

#include "appcore/activity.h"
#include "meeting/detail/py_value.h"
#include "meeting/detail/runtime_internal.h"
#include "meeting/runtime.h"
#include "meeting/runtime_messages.h"
#include "models/meeting_speech.h"
#include "remote/meeting_route.h"
#include "settings/keys.h"
#include "settings/store.h"

namespace fv::meeting {

namespace {

const log::Logger& logger = detail::runtime_logger;

ActivitySnapshot activity_of(ActivityState* activity) {
    return activity != nullptr ? activity->snapshot() : ActivitySnapshot{};
}

} // namespace

void require_dashboard_address(RuntimeEngine& engine, const Json& result) {
    if (!detail::truthy(detail::get(result, "host_url")) &&
        !detail::truthy(detail::get(result, "url"))) {
        engine.abort_start();
        throw std::runtime_error(std::string(messages::kDashboardNoAddress));
    }
}

void MeetingRuntime::start_meeting(std::optional<bool> cloud, std::string system_audio_policy,
                                   std::string intent) {
    begin_start(cloud, false, std::move(system_audio_policy), std::move(intent));
}

void MeetingRuntime::start_demo_meeting(std::optional<bool> cloud,
                                        std::string system_audio_policy) {
    begin_start(cloud, true, std::move(system_audio_policy), {});
}

void MeetingRuntime::begin_start(std::optional<bool> cloud, bool demo,
                                 std::string system_audio_policy, std::string intent) {
    // is_active is engine-derived and stays false for the seconds the start worker takes, so the
    // authoritative guard is the exclusive flag plus a claim held across the consent round-trip.
    // Without it a second launch would build a second engine and leak the first.
    const ActivitySnapshot activity = activity_of(deps_.activity);
    bool finalizing = false;
    bool busy = false;
    {
        const std::lock_guard lock(mutex_);
        finalizing = finalizing_;
        busy = is_claimed_locked();
    }
    if (activity.host_update_active) {
        meeting_status_update.emit(messages::update_preparing());
        return;
    }
    if (activity.backup_in_progress) {
        meeting_status_update.emit(std::string(messages::kBackupBusy));
        return;
    }
    if (finalizing) {
        meeting_status_update.emit(std::string(messages::kFinalInsightsPreparing));
        return;
    }
    if (busy) {
        meeting_status_update.emit(std::string(messages::kAlreadyInProgress));
        return;
    }
    if (deps_.engine_loading_message &&
        models::resolve_meeting_asr_source(settings_snapshot()) != "remote") {
        if (const auto message = deps_.engine_loading_message(); message && !message->empty()) {
            meeting_status_update.emit(*message);
            return;
        }
    }
    // File the leftover card before claiming the start: Keep for later refuses while a start
    // is claimed. The gates are checked again once it is filed.
    file_leftover_card([this, cloud, demo, system_audio_policy = std::move(system_audio_policy),
                        intent = std::move(intent)]() mutable {
        if (activity_of(deps_.activity).backup_in_progress) {
            meeting_status_update.emit(std::string(messages::kBackupBusy));
            return;
        }
        bool claimed = false;
        {
            const std::lock_guard lock(mutex_);
            if (!(starting_ || controller_meeting_active_ || is_active_locked() || finalizing_)) {
                starting_ = true;
                claimed = true;
            }
        }
        if (!claimed) {
            meeting_status_update.emit(std::string(messages::kAlreadyInProgress));
            return;
        }
        sync_activity();
        const ActivitySnapshot now = activity_of(deps_.activity);
        if (now.recording || now.transcribing) {
            {
                const std::lock_guard lock(mutex_);
                starting_ = false;
            }
            sync_activity();
            status_update.emit(std::string(messages::kFinishRecording));
            return;
        }

        const bool chosen =
            cloud ? *cloud : keys::meeting_cloud_last_enabled.read(settings_snapshot());
        std::function<bool(bool, std::function<void(bool)>)> gate;
        {
            const std::lock_guard lock(mutex_);
            pending_system_audio_policy_ =
                system_audio_policy.empty() ? std::string("auto") : system_audio_policy;
            // Every start rewrites the brief, so an abandoned one cannot leak into the next.
            pending_intent_ = intent;
            pending_agent_approval_.reset();
            gate = consent_gate_;
        }
        if (chosen && gate &&
            gate(chosen, [this, demo](bool granted) { on_consent_result(demo, granted); })) {
            return;
        }
        std::string policy;
        {
            const std::lock_guard lock(mutex_);
            policy = pending_system_audio_policy_;
        }
        launch(chosen, demo, policy);
    });
}

void MeetingRuntime::on_consent_result(bool demo, bool granted) {
    // A declined consent still starts the meeting, transcript-only; the launch releases the
    // start claim through its worker.
    std::string policy;
    {
        const std::lock_guard lock(mutex_);
        policy = pending_system_audio_policy_;
    }
    launch(granted, demo, policy);
}

void MeetingRuntime::launch(bool cloud, bool demo, std::string system_audio_policy) {
    // The persisted selection is only a preference. Capture needs a new positive answer,
    // bound to the named peer, for this specific live session.
    std::optional<Json> approved_route;
    const auto settings = settings_snapshot();
    if (!demo && keys::meeting_speech_separate.read(settings) &&
        keys::meeting_asr_source.read(settings) == "remote") {
        try {
            auto proposed = remote::meeting_route(settings);
            if (!deps_.confirm_live_audio || !deps_.confirm_live_audio(proposed)) {
                throw EngineError("Meeting canceled. No audio was sent.");
            }
            approved_route = std::move(proposed);
        } catch (const std::exception& error) {
            {
                const std::lock_guard lock(mutex_);
                starting_ = false;
            }
            sync_activity();
            meeting_status_update.emit(error.what());
            return;
        }
    }
    try {
        // Before capture opens: the meeting would record History playback.
        playback_stop_requested.emit("meeting");
    } catch (const std::exception& error) {
        logger.warning("Could not stop playback before a meeting: {}", error.what());
    }
    if (deps_.settings != nullptr) {
        try {
            Json patch = Json::object();
            patch[std::string(keys::meeting_cloud_last_enabled.key())] = cloud;
            deps_.settings->update(patch);
        } catch (const std::exception& error) {
            logger.warning("Could not persist meeting cloud toggle: {}", error.what());
        }
    }
    {
        const std::lock_guard lock(mutex_);
        controller_meeting_active_ = false;
        // A new meeting replaces any retained post-meeting finalization.
        finalizing_ = false;
        finalization_.reset();
        card_meeting_id_.reset();
        deferred_start_errors_.clear();
    }
    sync_activity();
    meeting_status_update.emit(std::string(demo ? messages::kStartingDemo : messages::kStarting));
    Json state = Json::object();
    state["active"] = false;
    state["paused"] = false;
    state["status"] = "starting";
    state["cloud_enabled"] = cloud;
    state["finalization"] = nullptr;
    meeting_state_changed.emit(state);
    if (system_audio_policy.empty()) {
        system_audio_policy = "auto";
    }
    spawn("meeting-start", [this, cloud, demo, policy = std::move(system_audio_policy),
                            approved_route = std::move(approved_route)]() mutable {
        start_worker(cloud, demo, policy, std::move(approved_route));
    });
}

MeetingEngineOptions MeetingRuntime::make_options(bool cloud, bool demo,
                                                  const std::string& system_audio_policy,
                                                  const CancelToken& cancel) const {
    std::string intent;
    std::optional<LiveIntelligenceApproval> approval;
    {
        const std::lock_guard lock(mutex_);
        intent = pending_intent_;
        approval = pending_agent_approval_;
    }
    auto options = build_options(settings_snapshot(), cloud, demo, system_audio_policy, intent,
                                 deps_.sources, cancel);
    if (cloud && approval) {
        const auto& route = approval->primary;
        options.agent_core_kind = route.kind;
        options.llm_provider = route.provider;
        options.llm_model = route.model;
        options.llm_endpoint = route.endpoint.is_null()
                                   ? std::nullopt
                                   : std::optional<Json>(std::in_place, route.endpoint);
        options.agent_approval = std::move(approval);
    }
    return options;
}

void MeetingRuntime::cancel_engine_work(const RuntimeEngine* engine) {
    std::optional<CancelSource> source;
    {
        const std::lock_guard lock(mutex_);
        if (const auto found = engine_cancels_.find(engine); found != engine_cancels_.end()) {
            source = std::move(found->second);
            engine_cancels_.erase(found);
        }
    }
    if (source) {
        source->cancel();
    }
}

void MeetingRuntime::hold_engine_events(const std::shared_ptr<RuntimeEngine>& engine) {
    const std::lock_guard lock(mutex_);
    ++held_events_[engine.get()].calls;
}

void MeetingRuntime::release_engine_events(const RuntimeEngine* engine) {
    std::vector<QueuedEvent> events;
    {
        const std::lock_guard lock(mutex_);
        const auto found = held_events_.find(engine);
        if (found == held_events_.end() || --found->second.calls > 0) {
            return;
        }
        events = std::move(found->second.events);
        held_events_.erase(found);
    }
    for (const QueuedEvent& event : events) {
        route_engine_event(event);
    }
}

void MeetingRuntime::start_worker(bool cloud, bool demo, std::string system_audio_policy,
                                  std::optional<Json> approved_route) {
    std::shared_ptr<RuntimeEngine> engine;
    Json result;
    try {
        shutdown_archive_dashboard();
        shutdown_engine(std::nullopt); // a previous (ended) session's server
        // Cancelled when this engine is shut down, at cleanup or with the runtime.
        CancelSource engine_cancel(shutdown_.token());
        MeetingEngineOptions options =
            make_options(cloud, demo, system_audio_policy, engine_cancel.token());
        options.asr_remote = std::move(approved_route);
        // Permission for this live session does not extend to a second pass over its recording.
        if (options.asr_remote)
            options.end_redecode = false;
        {
            // Retained background dashboards may still own the configured port.
            const std::lock_guard lock(mutex_);
            if (!background_engines_.empty()) {
                options.server_port = 0;
            }
        }
        const bool remote = options.asr_remote.has_value();
        engine = deps_.engine_factory(std::move(options));
        if (!engine) {
            throw std::runtime_error("No meeting engine is available.");
        }
        {
            const std::lock_guard lock(mutex_);
            engine_cancels_.insert_or_assign(engine.get(), engine_cancel);
        }
        listen(engine);
        {
            // Published before start(): start takes seconds to bring capture, ASR and the server
            // up, and a cleanup() landing meanwhile must find the engine to shut it down.
            const std::lock_guard lock(mutex_);
            engine_ = engine;
        }
        // Free the dictation model before start() loads the meeting's own, so identical weights
        // never sit in memory twice.
        if (!remote) {
            release_local_engine();
        }
        result = engine->start();
        require_dashboard_address(*engine, result);
    } catch (const std::exception& error) {
        start_failed(engine, error.what());
        return;
    }
    const auto deferred = adopt_started_engine(engine, result);
    if (!deferred) {
        return;
    }
    announce_started(engine, result, cloud, demo);
    for (const std::string& message : *deferred) {
        meeting_error.emit(message);
    }
}

void MeetingRuntime::start_failed(const std::shared_ptr<RuntimeEngine>& engine,
                                  const std::string& error) {
    logger.error("Failed to start meeting: {}", error);
    if (engine) {
        cancel_engine_work(engine.get());
    }
    Subscription listener;
    {
        const std::lock_guard lock(mutex_);
        starting_ = false;
        if (engine && engine_ == engine) {
            engine_.reset();
        }
        if (engine) {
            if (const auto found = listeners_.find(engine.get()); found != listeners_.end()) {
                listener = std::move(found->second);
                listeners_.erase(found);
            }
        }
        host_url_.reset();
        guest_url_.reset();
        finalizing_ = false;
        finalization_.reset();
        card_meeting_id_.reset();
        deferred_start_errors_.clear();
        controller_meeting_active_ = false;
    }
    listener.disconnect();
    sync_activity();
    // A start that failed before the meeting's ASR existed never reports asr_released.
    restore_local_engine();
    meeting_error.emit(messages::could_not_start(error));
    Json state = Json::object();
    state["active"] = false;
    state["paused"] = false;
    state["status"] = "failed";
    state["dashboard_available"] = false;
    state["finalization"] = nullptr;
    state["meeting_id"] = nullptr;
    meeting_state_changed.emit(state);
    refresh_past_meetings();
}

std::optional<std::vector<std::string>>
MeetingRuntime::adopt_started_engine(const std::shared_ptr<RuntimeEngine>& engine,
                                     const Json& result) {
    std::optional<std::vector<std::string>> deferred;
    {
        const std::lock_guard lock(mutex_);
        starting_ = false;
        if (engine->processing_stopped()) {
            // A host may End or Quit as soon as the dashboard opens; its terminal events own the
            // UI, so this late completion must not announce an ended meeting as active.
            deferred_start_errors_.clear();
        } else {
            engine_ = engine;
            // The engine's server_started event usually came first; keep its URLs when the
            // result omits them.
            const std::string host = detail::text_of(result, "host_url");
            const std::string url = detail::text_of(result, "url");
            if (!host.empty()) {
                host_url_ = host;
            } else if (!url.empty()) {
                host_url_ = url;
            }
            if (const std::string guest = detail::text_of(result, "guest_url"); !guest.empty()) {
                guest_url_ = guest;
            }
            deferred = std::move(deferred_start_errors_);
            deferred_start_errors_.clear();
        }
    }
    sync_activity();
    return deferred;
}

void MeetingRuntime::announce_started(const std::shared_ptr<RuntimeEngine>& engine,
                                      const Json& result, bool cloud, bool demo) {
    // No meeting_server_started here: the engine's own server_started event carried the URLs,
    // and a second one would open a second dashboard tab.
    meeting_status_update.emit(std::string(demo ? messages::kDemoLoaded : messages::kInProgress));
    {
        const std::lock_guard lock(mutex_);
        controller_meeting_active_ = true;
    }
    sync_activity();
    Json state = Json::object();
    state["active"] = true;
    state["paused"] = false;
    state["status"] = "active";
    state["cloud_enabled"] = cloud;
    const std::string meeting_id = detail::py_str(detail::get(result, "meeting_id"));
    state["meeting_id"] = meeting_id;
    state.update(clock_fields(engine));
    meeting_state_changed.emit(state);
    if (demo) {
        logger.info("Demo meeting started: {}", meeting_id);
    } else {
        logger.info("Meeting started: {}", meeting_id);
    }
}

// ---- pause, resume, end ----
// The engine persists its status (and End waits out a start), so the calls run on a worker.
// Python makes them on the Qt thread, so its own state goes out before any event the engine
// queued meanwhile: the outcome is published on the UI actor, and the engine's events wait for
// it (an `ended` routed before "ending" would leave the tab locked on an ended meeting).

namespace {

// Runs `call` on the engine; the error text, or nullopt when it worked.
std::optional<std::string> engine_call(const char* what, const std::function<void()>& call) {
    try {
        call();
        return std::nullopt;
    } catch (const std::exception& error) {
        logger.error("Failed to {} meeting: {}", what, error.what());
        return std::string(error.what());
    }
}

} // namespace

std::optional<std::pair<std::string, meeting_agent::MeetingAgentRoute>>
MeetingRuntime::live_intelligence_destination() const {
    std::shared_ptr<RuntimeEngine> engine;
    {
        const std::lock_guard lock(mutex_);
        engine = engine_;
    }
    if (engine && engine_active(engine)) {
        if (auto route = engine->live_intelligence_destination()) {
            return std::pair(engine->meeting_id(), std::move(*route));
        }
    }
    return std::nullopt;
}

void MeetingRuntime::set_start_intelligence_approval(LiveIntelligenceApproval approval) {
    const std::lock_guard lock(mutex_);
    if (starting_) {
        pending_agent_approval_ = std::move(approval);
    }
}

void MeetingRuntime::request_browser_insights(
    std::string meeting_id, CancelToken permit,
    std::function<void(std::optional<std::string>)> finish) {
    post_ui([this, meeting_id = std::move(meeting_id), permit,
             finish = std::move(finish)]() mutable {
        try {
            std::shared_ptr<RuntimeEngine> engine;
            {
                const std::lock_guard lock(mutex_);
                engine = engine_;
            }
            if (permit.cancelled() || !engine || !engine_active(engine) ||
                engine->meeting_id() != meeting_id) {
                finish("This meeting is no longer active.");
                return;
            }
            const auto current = engine->store();
            if (!current) {
                finish("This meeting is no longer active.");
                return;
            }
            const Json snapshot = current->snapshot();
            if (snapshot.value("cloud_enabled", false)) {
                finish(std::nullopt);
                return;
            }
            const auto route = engine->live_intelligence_destination();
            if (!route || !deps_.confirm_browser_insights(snapshot.value("title", ""), *route)) {
                finish("AI insights were not approved on the desktop.");
                return;
            }
            bool still_current = false;
            {
                const std::lock_guard lock(mutex_);
                still_current = engine_ == engine;
            }
            if (!still_current || permit.cancelled()) {
                finish("This AI insights request is no longer active.");
                return;
            }
            spawn("meeting-browser-insights", [this, engine, route = *route, permit, finish] {
                const auto failed = engine_call("change AI insights", [&] {
                    engine->set_live_intelligence(LiveIntelligenceApproval{route, std::nullopt},
                                                  permit);
                });
                if (failed) {
                    finish(*failed);
                    return;
                }
                post_ui([this, engine] {
                    {
                        const std::lock_guard lock(mutex_);
                        if (engine_ != engine) {
                            return;
                        }
                    }
                    if (const auto current = engine->store()) {
                        const bool enabled = current->snapshot().value("cloud_enabled", false);
                        meeting_state_changed.emit(Json{{"cloud_enabled", enabled}});
                    }
                });
                finish(std::nullopt);
            });
        } catch (const std::exception&) {
            finish("Could not request AI insights from the desktop.");
        }
    });
}

void MeetingRuntime::set_live_intelligence(const std::string& meeting_id,
                                           std::optional<LiveIntelligenceApproval> approval) {
    std::shared_ptr<RuntimeEngine> engine;
    {
        const std::lock_guard lock(mutex_);
        engine = engine_;
    }
    if (!engine || !engine_active(engine) || engine->meeting_id() != meeting_id) {
        return;
    }
    spawn("meeting-insights", [this, engine, approval = std::move(approval)] {
        const auto failed =
            engine_call("change AI insights", [&] { engine->set_live_intelligence(approval); });
        const auto current = engine->store();
        const auto enabled = current && current->snapshot().value("cloud_enabled", false);
        post_ui([this, engine, failed, enabled] {
            {
                const std::lock_guard lock(mutex_);
                if (engine_ != engine) {
                    return;
                }
            }
            if (failed) {
                meeting_error.emit(messages::could_not_change_insights(*failed));
            }
            meeting_state_changed.emit(Json{{"cloud_enabled", enabled}});
        });
    });
}

void MeetingRuntime::pause() {
    std::shared_ptr<RuntimeEngine> engine;
    {
        const std::lock_guard lock(mutex_);
        engine = engine_;
    }
    if (!engine || !engine_active(engine)) {
        return;
    }
    hold_engine_events(engine);
    spawn("meeting-pause", [this, engine] {
        const auto failed = engine_call("pause", [&engine] { engine->pause(); });
        post_ui([this, engine, failed] {
            if (failed) {
                meeting_error.emit(messages::could_not_pause(*failed));
            } else {
                meeting_status_update.emit(std::string(messages::kPaused));
                Json state = Json::object();
                state["active"] = true;
                state["paused"] = true;
                state["status"] = "paused";
                state.update(clock_fields(engine));
                meeting_state_changed.emit(state);
            }
            release_engine_events(engine.get());
        });
    });
}

void MeetingRuntime::resume() {
    std::shared_ptr<RuntimeEngine> engine;
    {
        const std::lock_guard lock(mutex_);
        engine = engine_;
    }
    if (!engine || !engine_active(engine)) {
        return;
    }
    hold_engine_events(engine);
    spawn("meeting-resume", [this, engine] {
        const auto failed = engine_call("resume", [&engine] { engine->resume(); });
        post_ui([this, engine, failed] {
            if (failed) {
                meeting_error.emit(messages::could_not_resume(*failed));
            } else {
                meeting_status_update.emit(std::string(messages::kInProgress));
                Json state = Json::object();
                state["active"] = true;
                state["paused"] = false;
                state["status"] = "active";
                state.update(clock_fields(engine));
                meeting_state_changed.emit(state);
            }
            release_engine_events(engine.get());
        });
    });
}

void MeetingRuntime::end() {
    // End is asynchronous in the engine (capture stops, ASR drains, the end passes run); its
    // `ended` event marks completion and releases the exclusive mode.
    std::shared_ptr<RuntimeEngine> engine;
    {
        const std::lock_guard lock(mutex_);
        engine = engine_;
    }
    if (!engine || !engine_active(engine)) {
        return;
    }
    hold_engine_events(engine);
    spawn("meeting-end", [this, engine] {
        const auto failed = engine_call("end", [&engine] { engine->end(); });
        post_ui([this, engine, failed] {
            if (failed) {
                meeting_error.emit(messages::could_not_end(*failed));
            } else {
                meeting_status_update.emit(std::string(messages::kEnding));
                Json state = Json::object();
                state["active"] = true;
                state["paused"] = false;
                state["status"] = "ending";
                meeting_state_changed.emit(state);
            }
            release_engine_events(engine.get());
        });
    });
}

} // namespace fv::meeting
