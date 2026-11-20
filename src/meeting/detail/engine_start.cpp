// MeetingEngine: start, the processing start, an aborted start's cleanup and the heartbeat
// (meeting/engine.py start, _start_processing, _processing_worker, _abort_start,
// _heartbeat_loop).

#include <exception>
#include <format>
#include <utility>

#include "base/utf8.h"
#include "engine/error.h"
#include "meeting/demo_fixture.h"
#include "meeting/detail/engine_internal.h"
#include "meeting/detail/py_value.h"
#include "meeting/engine.h"
#include "meeting_state/data_lifecycle.h"
#include "meeting_state/patches.h"
#include "meeting_state/schema.h"
#include "meeting_state/segment_ops.h"
#include "pyport/json.h"
#include "pyport/text.h"

namespace fv::meeting {

namespace {

const log::Logger& logger = detail::engine_logger;

} // namespace

Json MeetingEngine::start() {
    {
        const std::lock_guard lock(lifecycle_mutex_);
        if (active_.load() || starting_ || end_claimed_) {
            throw EngineError("Meeting engine is already active or finished");
        }
        starting_ = true;
        start_thread_ = std::this_thread::get_id();
        start_complete_ = false;
        active_ = true;
    }
    struct Finish {
        MeetingEngine& engine;
        ~Finish() {
            {
                const std::lock_guard lock(engine.lifecycle_mutex_);
                engine.starting_ = false;
                engine.start_thread_ = {};
                engine.start_complete_ = true;
            }
            engine.lifecycle_changed_.notify_all();
        }
    } finish{*this};

    try {
        const std::string id = meeting_state::new_id("m", ids_);
        const auto [host_token, guest_token] = deps_.tokens();
        const std::filesystem::path spool_dir = spool_root() / path_from_utf8(id.substr(0, 12));
        std::filesystem::create_directories(spool_dir);

        create_meeting_row(id, host_token, guest_token, spool_dir);
        {
            const std::lock_guard lock(state_mutex_);
            meeting_id_ = id;
            spool_dir_ = spool_dir;
        }
        open_store(id);

        meeting_clock_.start();
        start_heartbeat();

        std::string capture_note;
        if (options_.demo_mode) {
            capture_note = seed_demo_meeting();
        } else {
            // For a remote meeting the runtime confirmed the destination before opening capture.
            capture_note = capture_->start(id, spool_dir).value_or("");
        }
        const std::string url = start_server();
        if (!hooks_.skip_processing) {
            start_processing();
        }
        emit_status(capture_note);

        const auto transport = server();
        Json result = Json::object();
        result["meeting_id"] = id;
        result["url"] = url;
        result["host_url"] = transport ? Json(transport->host_url()) : Json();
        result["guest_url"] = transport ? Json(transport->guest_url()) : Json();
        return result;
    } catch (const std::exception& error) {
        logger.error_with(error, "Meeting start failed");
        abort_start();
        throw;
    } catch (...) {
        logger.error("Meeting start failed");
        abort_start();
        throw;
    }
}

void MeetingEngine::create_meeting_row(const std::string& id, const std::string& host_token,
                                       const std::string& guest_token,
                                       const std::filesystem::path& spool_dir) {
    const std::string started_at = utc_now();
    Json fields = Json::object();
    fields["id"] = id;
    fields["title"] = options_.title;
    fields["status"] = "active";
    fields["started_at"] = started_at;
    fields["ended_at"] = nullptr;
    fields["paused_total_s"] = 0.0;
    fields["host_token"] = host_token;
    fields["guest_token"] = guest_token;
    fields["cloud_enabled"] = options_.cloud_enabled && intelligence_->available();
    fields["asr_model"] = options_.asr_model;
    fields["asr_remote_json"] =
        options_.asr_remote ? Json(py::dumps(*options_.asr_remote)) : Json();
    fields["agent_provider"] = options_.llm_provider;
    fields["agent_model"] = options_.llm_model;
    fields["agent_endpoint_json"] = options_.llm_endpoint && detail::truthy(*options_.llm_endpoint)
                                        ? Json(py::dumps(*options_.llm_endpoint))
                                        : Json();
    fields["spool_dir"] = path_to_utf8(spool_dir);
    fields["state_json"] = nullptr;
    fields["state_seq"] = 0;
    fields["app_pid"] = *deps_.pid;
    fields["app_heartbeat_at"] = started_at;
    deps_.repository->create_meeting(fields);
}

void MeetingEngine::open_store(const std::string& id) {
    const bool cloud_enabled = options_.cloud_enabled && intelligence_->available();
    // Clipped rather than refused: a start must not fail over an over-long brief, and the
    // validation layer owns the limit.
    const std::string intent_text =
        py::slice(py::strip(options_.intent), std::nullopt,
                  static_cast<std::int64_t>(meeting_state::MAX_INTENT_LEN));
    meeting_state::MeetingState state;
    state.meeting_id = id;
    state.cloud_enabled = cloud_enabled;
    if (options_.asr_remote) {
        const std::string host = options_.asr_remote->value("host_name", std::string());
        // engine.py _record_remote_route: _on_asr_connection_status("Transcribing on <host>").
        state.speech = {{"source", "remote"},
                        {"host", host},
                        {"model", options_.asr_model},
                        {"connected", true},
                        {"message", "Transcribing on " + host}};
    }
    state.title = options_.title;
    state.intent.text = intent_text;
    state.intent.updated_at = intent_text.empty() ? std::string() : utc_now();
    state.finalization = meeting_state::FinalizationState::default_for_cloud(cloud_enabled);
    state.report_views = options_.report_views;
    state.insight_review = review_config(options_.insight_review, cloud_enabled);

    const auto repository = deps_.repository;
    meeting_state::SegmentPredicates predicates;
    predicates.segment_exists = [repository, id](std::string_view segment_id) {
        return repository->segment_exists(id, segment_id);
    };
    // Lets the validator refuse a stale diarizer re-cluster that would revert a human speaker
    // correction.
    predicates.segment_pinned = [repository, id](std::string_view segment_id) {
        const std::optional<Json> row = repository->get_segment(id, segment_id);
        return row && detail::truthy(detail::get(*row, "speaker_pinned"));
    };
    predicates.segment_lookup = meeting_state::repository_segment_lookup(repository, id);
    auto created = std::make_shared<meeting_state::MeetingStateStore>(
        std::move(state), repository, meeting_state::make_segment_handler(repository, id), clock_,
        ids_, std::move(predicates));
    {
        const std::lock_guard lock(state_mutex_);
        store_ = created;
    }
    const std::vector<meeting_state::OpResult> results =
        created->apply("system", std::nullopt,
                       Json::array({Json{{"op", "upsert_participant"},
                                         {"display_name", "Me"},
                                         {"kind", "me"},
                                         {"is_provisional", false}}}));
    if (!results.empty() && results.front().ok && detail::truthy(results.front().effect)) {
        const std::string me =
            detail::py_str(detail::get(detail::get(results.front().effect, "participant"), "id"));
        const std::lock_guard lock(state_mutex_);
        me_participant_id_ = me;
    }
}

std::string MeetingEngine::seed_demo_meeting() {
    const auto current = store();
    const std::string id = meeting_id();
    if (!current || id.empty()) {
        return std::string(DEMO_NOTE);
    }
    Json capture = Json::object();
    capture["mic_available"] = false;
    capture["loopback_available"] = false;
    capture["message"] = std::string(DEMO_NOTE);
    current->update_runtime_fields(Json{{"capture", capture},
                                        {"diarization_available", false},
                                        {"diarization_reason", std::string(DIARIZATION_DEMO)}});
    std::optional<std::string> me;
    {
        const std::lock_guard lock(state_mutex_);
        me = me_participant_id_;
    }
    meeting::seed_demo_meeting(
        id, me, *current, *deps_.repository, &meeting_clock_,
        options_.title.empty() ? std::nullopt : std::optional<std::string>(options_.title), ids_);
    logger.info("Demo meeting seeded for {} (no capture or ASR)", id);
    return std::string(DEMO_NOTE);
}

std::string MeetingEngine::start_server() {
    std::shared_ptr<DashboardTransport> transport;
    std::string url;
    try {
        transport = deps_.dashboard(*this, options_.server_bind, options_.server_port);
        if (!transport) {
            throw std::runtime_error("no dashboard server");
        }
        std::expected<std::string, std::string> started = transport->start();
        if (!started) {
            throw std::runtime_error(started.error());
        }
        url = std::move(*started);
    } catch (const std::exception& error) {
        logger.error_with(error, "Meeting web server failed to start");
        throw EngineError(std::format("Meeting dashboard could not start: {}", error.what()));
    }
    {
        const std::lock_guard lock(state_mutex_);
        server_ = transport;
    }
    emit("server_started", Json{{"url", url},
                                {"host_url", transport->host_url()},
                                {"guest_url", transport->guest_url()}});
    return url;
}

// Optional initialization, kept apart from the required startup.
void MeetingEngine::start_processing() {
    {
        const std::lock_guard lock(lifecycle_mutex_);
        if (processing_stop_.load() || !active_.load()) {
            return;
        }
        processing_.done = false;
        if (!deps_.processing_inline) {
            processing_.started = true;
            processing_.thread = std::thread([this] {
                detail::name_engine_thread("fv-meeting-start");
                processing_worker();
            });
            processing_.id = processing_.thread.get_id();
            return;
        }
    }
    processing_worker();
}

void MeetingEngine::processing_worker() {
    try {
        if (!processing_stop_.load()) {
            if (!options_.demo_mode) {
                // Transcription is the required feature and its model loads locally; speaker
                // separation may sit minutes on a slow speaker-model download, so it starts on
                // its own thread and never delays ASR.
                start_asr(true);
                if (!processing_stop_.load()) {
                    start_diarizer_background();
                }
            }
            if (!processing_stop_.load()) {
                start_fast_features();
                start_live_intelligence();
                emit_status();
            }
        }
    } catch (const std::exception& error) {
        if (!processing_stop_.load()) {
            logger.error_with(error, "Meeting processing initialization failed");
            emit("error", Json{{"code", "processing_unavailable"}, {"message", error.what()}});
        } else {
            // End or Quit tore the pipeline down under this worker.
            logger.debug("Meeting processing initialization stopped by End; ignoring its error: "
                         "{}",
                         error.what());
        }
    }
    set_worker_done(processing_);
}

bool MeetingEngine::wait_for_processing(std::chrono::milliseconds timeout) {
    const bool ready = wait_worker(processing_, timeout);
    wait_worker(diarizer_init_, timeout);
    return ready;
}

void MeetingEngine::set_worker_done(Worker& worker) {
    {
        const std::lock_guard lock(lifecycle_mutex_);
        worker.done = true;
    }
    lifecycle_changed_.notify_all();
}

bool MeetingEngine::wait_worker(const Worker& worker, std::chrono::milliseconds timeout) {
    std::unique_lock lock(lifecycle_mutex_);
    if (worker.started && worker.id == std::this_thread::get_id()) {
        return worker.done;
    }
    return lifecycle_changed_.wait_for(lock, timeout, [&worker] { return worker.done; });
}

// ---- an aborted start ----

void MeetingEngine::abort_start() {
    active_ = false;
    processing_stop_ = true;
    processing_cancel_.cancel();
    revoke_agent_writes();
    capture_->stop();
    stop_fast_features();
    stop_live_intelligence();
    capture_->flush(); // releases the spool writer threads
    stop_asr("start abort");
    std::shared_ptr<DashboardTransport> transport;
    {
        const std::lock_guard lock(state_mutex_);
        transport = std::move(server_);
    }
    if (transport) {
        try {
            transport->stop();
        } catch (const std::exception& error) {
            logger.error_with(error, "Server stop failed during start abort");
        }
    }
    stop_heartbeat();
    meeting_clock_.pause();
    const std::string id = meeting_id();
    if (id.empty()) {
        return;
    }
    // A start that never captured anything is not a real meeting: keeping it would leave an
    // "Untitled · Ended" history row for every missing dependency or unavailable device. The
    // row stays whenever it cannot be proven empty.
    if (!aborted_meeting_has_artifacts(id) && discard_empty_meeting(id)) {
        return;
    }
    mark_aborted_failed(id);
}

bool MeetingEngine::aborted_meeting_has_artifacts(const std::string& id) {
    try {
        return !deps_.repository->get_audio_chunks(id).empty() ||
               !deps_.repository->get_segments(id, -1.0, std::nullopt).empty();
    } catch (const std::exception& error) {
        logger.error_with(error, "Could not inspect aborted meeting artifacts");
        return true;
    }
}

bool MeetingEngine::discard_empty_meeting(const std::string& id) {
    try {
        meeting_state::delete_meeting_data(*deps_.repository, id, spool_root());
    } catch (const std::exception& error) {
        logger.error_with(error, "Failed to discard empty aborted meeting");
        return false;
    }
    logger.info("Discarded empty failed meeting start: {}", id);
    const std::lock_guard lock(state_mutex_);
    store_.reset();
    return true;
}

void MeetingEngine::mark_aborted_failed(const std::string& id) {
    try {
        deps_.repository->update_meeting(id, Json{{"status", "failed"}, {"ended_at", utc_now()}});
    } catch (const std::exception& error) {
        logger.error_with(error, "Failed to mark aborted meeting as failed");
    }
    const auto current = store();
    if (!current) {
        return;
    }
    try {
        current->update_runtime_fields(Json{{"status", "failed"}});
    } catch (const std::exception& error) {
        logger.error_with(error, "Failed to mark aborted store status");
    }
    try {
        set_finalization({.status = "unavailable",
                          .message = "Meeting failed to start before final insights could run.",
                          .emit = false});
    } catch (const std::exception& error) {
        logger.error_with(error, "Failed to persist finalization after aborted start");
    }
}

// ---- the heartbeat ----

void MeetingEngine::start_heartbeat() {
    const std::lock_guard lock(heartbeat_mutex_);
    heartbeat_stopped_ = false;
    schedule_heartbeat();
}

// Called with heartbeat_mutex_ held.
void MeetingEngine::schedule_heartbeat() {
    heartbeat_timer_ = deps_.strand->post_after(
        std::chrono::milliseconds(static_cast<std::int64_t>(HEARTBEAT_INTERVAL_S * 1000)),
        [this, guard = heartbeat_guard_] { guard->run([this] { heartbeat_tick(); }); });
}

void MeetingEngine::heartbeat_tick() {
    {
        const std::lock_guard lock(heartbeat_mutex_);
        if (heartbeat_stopped_) {
            return;
        }
    }
    // It must outlive any failure, and a lasting one warns once rather than every pass.
    try {
        deps_.repository->heartbeat(meeting_id(), *deps_.pid, utc_now());
        failures_.recovered("heartbeat");
    } catch (const std::exception&) {
        failures_.failed("heartbeat", "Meeting heartbeat failed");
    }
    // Fast features retry without new speech: a Settings change or a judge configured later
    // starts the missing workers, and the highlight status follows at once.
    try {
        start_fast_features();
        if (refresh_live_highlights_status()) {
            emit_status();
        }
        failures_.recovered("fast_features");
    } catch (const std::exception&) {
        failures_.failed("fast_features", "Fast meeting features could not start");
    }
    const std::lock_guard lock(heartbeat_mutex_);
    if (!heartbeat_stopped_) {
        schedule_heartbeat();
    }
}

void MeetingEngine::stop_heartbeat() {
    const std::lock_guard lock(heartbeat_mutex_);
    heartbeat_stopped_ = true;
    heartbeat_timer_.cancel();
}

} // namespace fv::meeting
