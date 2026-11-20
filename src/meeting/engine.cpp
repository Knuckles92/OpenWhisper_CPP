#include "meeting/engine.h"

#include <algorithm>
#include <array>
#include <exception>
#include <format>
#include <thread>
#include <tuple>
#include <utility>

#include "base/log.h"
#include "engine/error.h"
#include "meeting/detail/engine_internal.h"
#include "meeting/detail/py_value.h"
#include "meeting_state/content.h"
#include "meeting_state/schema.h"
#include "meeting_state/time_utils.h"
#include "meeting_web/auth.h"
#include "pyport/errors.h"
#include "pyport/text.h"

namespace fv::meeting {

namespace {

using meeting_state::MeetingState;
using meeting_state::OpResult;

// MeetingAsrEngine behind the engine's ASR interface, holding the speech service's engine lease
// (when given) from its creation until it stops, so no reload swaps the model under the meeting.
class AsrEngineAdapter final : public MeetingAsr {
public:
    AsrEngineAdapter(std::unique_ptr<MeetingAsrEngine> engine,
                     std::optional<speech::EngineLease> lease)
        : engine_(std::move(engine)), lease_(std::move(lease)) {}

    bool load_backend(const CancelToken& cancel) override { return engine_->load_backend(cancel); }
    bool is_available() const override { return engine_->is_available(); }
    std::string last_error() const override { return engine_->last_error(); }
    void start(ChunkResult on_chunk_result) override { engine_->start(std::move(on_chunk_result)); }
    bool enqueue(const meeting_state::SpooledChunk& chunk) override {
        return engine_->enqueue(chunk);
    }
    std::size_t requeue_pending() override { return engine_->requeue_pending(); }
    bool drain(std::chrono::milliseconds timeout) override { return engine_->drain(timeout); }
    void stop() override {
        engine_->stop();
        if (lease_) {
            lease_->release();
        }
    }
    void start_preview(PreviewCallback callback) override {
        engine_->start_preview(std::move(callback));
    }
    void feed_preview(const CaptureBlock& block, double start_s) noexcept override {
        engine_->feed_preview(block, start_s);
    }
    void stop_preview() override { engine_->stop_preview(); }
    std::vector<meeting_state::TranscriptSegment>
    transcribe_offline_session(const std::filesystem::path& spool_dir,
                               const std::vector<Json>& chunks, OfflineProgress progress,
                               const CancelToken& cancel) override {
        return engine_->transcribe_offline_session(spool_dir, chunks, std::move(progress), cancel);
    }

private:
    std::unique_ptr<MeetingAsrEngine> engine_;
    std::optional<speech::EngineLease> lease_;
};

// meeting_web::MeetingWebServer behind the engine's dashboard interface.
class WebServerTransport final : public DashboardTransport {
public:
    WebServerTransport(net::Reactor& reactor, MeetingEngine& engine,
                       std::shared_ptr<meeting_state::MeetingRepository> repository,
                       const std::string& bind, std::uint16_t port,
                       meeting_web::MeetingWebServer::RouteInstaller install_routes,
                       std::function<Json()> settings)
        : server_(reactor, engine, std::move(repository), bind, port, std::move(install_routes)) {
        if (settings) {
            server_.set_settings_source(std::move(settings));
        }
    }

    std::expected<std::string, std::string> start() override { return server_.start(); }
    void stop() override { server_.stop(); }
    void broadcast(const Json& message, bool host_only) override {
        server_.broadcast(message, host_only);
    }
    std::string base_url() const override { return server_.base_url(); }
    std::string host_url() const override { return server_.host_url(); }
    std::string guest_url() const override { return server_.guest_url(); }
    void invalidate_connections() override { server_.invalidate_connections(false); }

private:
    meeting_web::MeetingWebServer server_;
};

} // namespace

std::unique_ptr<MeetingAsr> adapt_asr_engine(std::unique_ptr<MeetingAsrEngine> engine,
                                             speech::SpeechService* lease_from) {
    std::optional<speech::EngineLease> lease;
    if (lease_from != nullptr) {
        lease.emplace(lease_from->lease("meeting"));
    }
    return std::make_unique<AsrEngineAdapter>(std::move(engine), std::move(lease));
}

MeetingEngine::MeetingEngine(MeetingEngineOptions engine_options, EngineDeps engine_deps)
    : options_(std::move(engine_options)), deps_(std::move(engine_deps)),
      clock_(deps_.clock != nullptr ? *deps_.clock : system_clock()),
      ids_(deps_.ids != nullptr ? *deps_.ids : meeting_state::random_ids()),
      intelligence_(deps_.intelligence ? deps_.intelligence : null_intelligence()),
      model_lease_(deps_.model_lease ? deps_.model_lease : std::make_shared<NullModelLease>()),
      failures_(detail::engine_logger), meeting_clock_(clock_),
      live_approved_route_(options_.agent_approval ? std::optional(options_.agent_approval->primary)
                                                   : std::nullopt),
      approved_backup_(options_.agent_approval ? options_.agent_approval->backup : std::nullopt),
      diarization_model_path_(options_.diarization_model_path),
      diarizer_host_([this] { return store(); },
                     [this](const diarize::RelabelOps& ops) { on_diarizer_relabel(ops); }),
      capture_sink_([this] { return store(); }, [this](std::string note) { emit_status(note); },
                    [this](std::string code, std::string message) {
                        emit("error",
                             Json{{"code", std::move(code)}, {"message", std::move(message)}});
                    }) {
    if (!deps_.pid) {
        deps_.pid = current_process_id();
    }
    if (!deps_.tokens) {
        deps_.tokens = [] {
            return meeting_web::generate_token_pair();
        };
    }
    if (!deps_.asr) {
        deps_.asr = [this](const std::string& id, const std::string& /*model*/,
                           const std::string& language,
                           AsrCallbacks callbacks) -> std::unique_ptr<MeetingAsr> {
            if (deps_.speech == nullptr) {
                throw EngineError("No speech engine is available for meetings.");
            }
            return adapt_asr_engine(
                std::make_unique<MeetingAsrEngine>(*deps_.speech, deps_.repository, id, language,
                                                   clock_, std::move(callbacks)),
                deps_.speech);
        };
    }
    if (!deps_.dashboard) {
        deps_.dashboard = [this](MeetingEngine& engine, const std::string& bind,
                                 std::uint16_t port) -> std::unique_ptr<DashboardTransport> {
            if (deps_.reactor == nullptr) {
                throw EngineError("No network reactor is available for the dashboard.");
            }
            return std::make_unique<WebServerTransport>(*deps_.reactor, engine, deps_.repository,
                                                        bind, port, deps_.install_routes,
                                                        deps_.settings);
        };
    }
    if (!deps_.diarizer) {
        deps_.diarizer = [](const std::optional<std::filesystem::path>& model,
                            const std::optional<std::filesystem::path>& onnxruntime,
                            diarize::DiarizerHost& host,
                            const std::string& id) -> std::unique_ptr<diarize::Diarizer> {
            return diarize::create_diarizer(model.value_or(std::filesystem::path()),
                                            onnxruntime.value_or(std::filesystem::path()), host,
                                            id);
        };
    }
    if (deps_.strand == nullptr) {
        own_pool_ = std::make_unique<ThreadPool>("fv-meeting-heartbeat", 1);
        deps_.strand = own_pool_.get();
    }
    listeners_ = std::make_shared<ListenerList>();

    CaptureOptions capture_options;
    capture_options.system_audio_policy = options_.system_audio_policy;
    capture_options.mic_priority = options_.mic_priority;
    capture_options.mic_device_id = options_.mic_device_id;
    capture_ = std::make_unique<MeetingCapture>(
        std::move(capture_options), meeting_clock_, deps_.repository, deps_.audio, capture_sink_,
        [this] { return active_.load(); },
        [this](const meeting_state::SpooledChunk& chunk) { on_chunk(chunk); },
        [this](const CaptureBlock& block) { feed_preview(block); });
    if (deps_.capture_sources) {
        capture_->set_source_factory(*deps_.capture_sources);
    }
    if (deps_.capture_timing) {
        capture_->set_timing(*deps_.capture_timing);
    }
    capture_->set_stop_preview([this] {
        if (const auto current = asr()) {
            current->stop_preview();
        }
    });
}

MeetingEngine::~MeetingEngine() {
    try {
        shutdown();
    } catch (const std::exception& error) {
        detail::engine_logger.error_with(error, "Meeting engine shutdown failed");
    }
    // The threads run members of this object, so they are waited for however long they take.
    for (Worker* worker : {&end_, &processing_, &diarizer_init_, &backup_switch_}) {
        std::thread thread;
        {
            const std::lock_guard lock(lifecycle_mutex_);
            thread = std::move(worker->thread);
        }
        if (thread.joinable()) {
            thread.join();
        }
    }
    stop_heartbeat();
    heartbeat_guard_->close();
    // The capture's watchdog and device threads call back into the engine.
    capture_.reset();
    std::vector<std::shared_ptr<MeetingAsr>> retired;
    {
        const std::lock_guard lock(retired_mutex_);
        retired.swap(retired_asr_);
    }
    retired.clear();
}

bool MeetingEngine::on_engine_thread() const {
    const std::thread::id self = std::this_thread::get_id();
    return std::ranges::any_of(std::array{&end_, &processing_, &diarizer_init_, &backup_switch_},
                               [&](const Worker* worker) { return worker->id.load() == self; });
}

void release_off_engine_threads(const MeetingEngine& engine, std::function<void()> release) {
    if (!engine.on_engine_thread()) {
        release();
        return;
    }
    detail::engine_logger.debug("Meeting engine released on one of its own threads; releasing it "
                                "on another");
    std::thread([release = std::move(release)] {
        detail::name_engine_thread("fv-meeting-release");
        try {
            release();
        } catch (const std::exception& error) {
            detail::engine_logger.error_with(error, "Meeting engine release failed");
        }
    }).detach();
}

// ---- listeners and events ----

Subscription MeetingEngine::add_listener(Executor& executor,
                                         std::function<void(const MeetingEvent&)> fn) {
    auto listener = std::make_shared<Listener>(
        Listener{&executor, std::move(fn), std::make_shared<CallbackGuard>()});
    {
        const std::lock_guard lock(listeners_->mutex);
        listeners_->entries.push_back(listener);
    }
    std::weak_ptr<ListenerList> weak = listeners_;
    const Listener* key = listener.get();
    return Subscription(listener->guard, [weak, key] {
        if (const auto list = weak.lock()) {
            const std::lock_guard lock(list->mutex);
            std::erase_if(list->entries, [key](const auto& entry) { return entry.get() == key; });
        }
    });
}

void MeetingEngine::emit(std::string kind, Json payload) {
    std::vector<std::shared_ptr<Listener>> listeners;
    {
        const std::lock_guard lock(listeners_->mutex);
        listeners = listeners_->entries;
    }
    const auto event =
        std::make_shared<const MeetingEvent>(MeetingEvent{std::move(kind), std::move(payload)});
    for (const auto& listener : listeners) {
        if (listener->guard->closed()) {
            continue;
        }
        auto run = [listener, event] {
            listener->guard->run([&] {
                try {
                    listener->fn(*event);
                } catch (const std::exception& error) {
                    detail::engine_logger.error_with(
                        error, std::format("Meeting listener raised for {} event", event->kind));
                } catch (...) {
                    detail::engine_logger.error("Meeting listener raised for {} event",
                                                event->kind);
                }
            });
        };
        if (listener->executor->is_current()) {
            run();
        } else {
            listener->executor->post(std::move(run));
        }
    }
}

std::shared_ptr<DashboardTransport> MeetingEngine::server() const {
    const std::lock_guard lock(state_mutex_);
    return server_;
}

std::shared_ptr<MeetingAsr> MeetingEngine::asr() const {
    const std::lock_guard lock(state_mutex_);
    return asr_;
}

std::shared_ptr<diarize::Diarizer> MeetingEngine::diarizer() const {
    const std::lock_guard lock(state_mutex_);
    return diarizer_;
}

std::string MeetingEngine::utc_now() const {
    return meeting_state::utc_now_iso(clock_);
}

void MeetingEngine::broadcast(const Json& message, bool host_only) {
    const auto transport = server();
    if (!transport) {
        return;
    }
    if (host_only && !transport->supports_host_only()) {
        detail::engine_logger.debug("Transport has no host-only broadcast; dropped {}",
                                    detail::py_str(detail::get(message, "type")));
        return;
    }
    try {
        transport->broadcast(message, host_only);
    } catch (const std::exception& error) {
        detail::engine_logger.error_with(error, "Meeting web broadcast failed");
    }
}

void MeetingEngine::emit_status(const std::string& note) {
    const auto current = store();
    if (!current) {
        return;
    }
    refresh_live_highlights_status();
    Json payload = current->with_state([](const MeetingState& state) {
        Json out = Json::object();
        out["status"] = state.status;
        out["cloud_enabled"] = state.cloud_enabled;
        out["intelligence_online"] = state.intelligence_online;
        out["diarization_available"] = state.diarization_available;
        out["diarization_reason"] = state.diarization_reason;
        out["capture"] = state.capture.is_object() ? state.capture : Json::object();
        out["finalization"] = state.finalization.to_json();
        out["live_highlights_status"] = state.live_highlights_status;
        out["speech"] = state.speech.is_object() ? state.speech : Json::object();
        return out;
    });
    Json listener_payload = payload;
    if (!note.empty()) {
        listener_payload["note"] = note;
    }
    emit("status", std::move(listener_payload));
    Json message = Json::object();
    message["type"] = "status";
    message.update(payload);
    broadcast(message);
}

void MeetingEngine::emit_ephemeral_finalization(
    const meeting_state::FinalizationState& finalization) {
    const auto current = store();
    if (!current) {
        return;
    }
    try {
        Json payload = current->with_state([](const MeetingState& state) {
            Json out = Json::object();
            out["status"] = state.status;
            out["intelligence_online"] = state.intelligence_online;
            out["diarization_available"] = state.diarization_available;
            out["diarization_reason"] = state.diarization_reason;
            out["capture"] = state.capture.is_object() ? state.capture : Json::object();
            return out;
        });
        payload["finalization"] = finalization.to_json();
        emit("status", payload);
        Json message = Json::object();
        message["type"] = "status";
        message.update(payload);
        broadcast(message);
    } catch (const std::exception& error) {
        detail::engine_logger.error_with(error, "Ephemeral finalization emit failed");
    }
}

bool MeetingEngine::set_finalization(const FinalizationUpdate& update) {
    const auto current = store();
    if (!current) {
        return false;
    }
    Json payload = Json::object();
    payload["status"] = update.status;
    payload["message"] = update.message;
    payload["stage"] = update.stage;
    payload["current_step"] = update.current_step;
    payload["total_steps"] = update.total_steps;
    payload["step_details"] = update.step_details;
    if (update.steps) {
        payload["steps"] = *update.steps;
    }
    if (update.summary_stats) {
        payload["summary_stats"] = *update.summary_stats;
    }
    const auto [card_deferred, cloud_enabled, meeting_status] =
        current->with_state([](const MeetingState& state) {
            return std::tuple(state.finalization.card_deferred, state.cloud_enabled, state.status);
        });
    if (card_deferred) {
        payload["card_deferred"] = true;
    }
    const meeting_state::FinalizationState finalization =
        meeting_state::FinalizationState::coerce(payload, cloud_enabled, meeting_status);
    const std::string& status = finalization.status;
    const bool terminal = status == "completed" || status == "disabled" ||
                          status == "unavailable" || status == "failed";
    if (std::find(meeting_state::FINALIZATION_STATUSES.begin(),
                  meeting_state::FINALIZATION_STATUSES.end(),
                  status) == meeting_state::FINALIZATION_STATUSES.end()) {
        return false;
    }
    if (!current->update_runtime_fields(Json{{"finalization", finalization.to_json()}})) {
        detail::engine_logger.warning("Could not persist finalization status={}", status);
        if (update.emit && terminal) {
            // An unlock and warning only: durable success is never claimed.
            emit_ephemeral_finalization(finalization);
        }
        return false;
    }
    if (terminal) {
        adopt_untitled_title_from_topic();
    }
    if (update.emit) {
        emit_status();
    }
    return true;
}

bool MeetingEngine::publish_finalization(const Json& finalization) {
    const Json payload = finalization.is_object() ? finalization : Json::object();
    FinalizationUpdate update;
    const std::string status = detail::str_or_empty(detail::get(payload, "status"));
    update.status = status.empty() ? "running" : status;
    update.message = detail::str_or_empty(detail::get(payload, "message"));
    update.stage = detail::str_or_empty(detail::get(payload, "stage"));
    update.current_step =
        static_cast<int>(detail::int_or_zero(detail::get(payload, "current_step")));
    update.total_steps = static_cast<int>(detail::int_or_zero(detail::get(payload, "total_steps")));
    update.step_details = detail::str_or_empty(detail::get(payload, "step_details"));
    update.steps =
        std::optional<Json>(std::in_place, detail::list_or_empty(detail::get(payload, "steps")));
    const Json& stats = detail::get(payload, "summary_stats");
    update.summary_stats =
        std::optional<Json>(std::in_place, stats.is_object() ? stats : Json::object());
    return set_finalization(update);
}

void MeetingEngine::adopt_untitled_title_from_topic() {
    const auto current = store();
    if (!current) {
        return;
    }
    try {
        const Json ops = meeting_state::build_untitled_title_ops(current->snapshot());
        if (!ops.empty()) {
            current->apply("system", std::string("title_from_topic"), ops);
        }
    } catch (const std::exception& error) {
        detail::engine_logger.error_with(error,
                                         "Could not adopt untitled meeting title from topic");
    }
}

ModelLease& MeetingEngine::model_lease() {
    return *model_lease_;
}

// ---- the dashboard's engine surface ----

std::string MeetingEngine::meeting_id() const {
    const std::lock_guard lock(state_mutex_);
    return meeting_id_;
}

std::shared_ptr<meeting_state::MeetingStateStore> MeetingEngine::store() const {
    const std::lock_guard lock(state_mutex_);
    return store_;
}

bool MeetingEngine::is_active() const {
    return active_.load();
}

std::vector<OpResult> MeetingEngine::apply_client_action(
    std::string_view actor, const std::optional<std::string>& participant_id, const Json& op) {
    const auto current = store();
    if (!current) {
        OpResult result;
        result.ok = false;
        result.op = op.is_object() ? op : Json::object();
        result.reason = "inactive";
        return {result};
    }
    std::vector<OpResult> results = current->apply(actor, participant_id, Json::array({op}));
    apply_diarizer_pins(results);
    notify_intelligence_guidance(results);
    return results;
}

std::vector<OpResult> MeetingEngine::undo(std::int64_t seq,
                                          const std::optional<std::string>& participant_id) {
    const auto current = store();
    if (!current) {
        return {};
    }
    std::vector<OpResult> results = current->undo(seq, participant_id);
    apply_diarizer_pins(results);
    notify_intelligence_guidance(results);
    return results;
}

void MeetingEngine::apply_diarizer_pins(const std::vector<OpResult>& results) {
    const auto current = diarizer();
    if (!current) {
        return;
    }
    for (const OpResult& result : results) {
        const Json& effect = result.effect;
        if (!result.ok || detail::get(effect, "entity") != Json("segment_speaker") ||
            !detail::truthy(detail::get(effect, "pinned")) ||
            detail::get(effect, "source") != Json("human") ||
            !detail::truthy(detail::get(effect, "participant_id"))) {
            continue;
        }
        const std::string segment_id = detail::py_str(detail::get(effect, "segment_id"));
        try {
            current->pin(segment_id, detail::py_str(detail::get(effect, "participant_id")));
        } catch (const std::exception& error) {
            detail::engine_logger.error_with(error,
                                             std::format("Diarizer pin failed for {}", segment_id));
        }
    }
}

Json MeetingEngine::add_guest(std::string_view display_name) {
    const auto current = store();
    if (!current) {
        throw EngineError("No active meeting");
    }
    std::string name = py::slice(py::strip(display_name), std::nullopt,
                                 static_cast<std::int64_t>(MAX_GUEST_NAME_LEN));
    if (name.empty()) {
        name = "Guest";
    }
    const std::vector<OpResult> results =
        current->apply("system", std::nullopt,
                       Json::array({Json{{"op", "upsert_participant"},
                                         {"display_name", name},
                                         {"kind", "guest"},
                                         {"is_provisional", false}}}));
    if (results.empty() || !results.front().ok || !detail::truthy(results.front().effect)) {
        const std::optional<std::string> reason =
            results.empty() ? std::nullopt : results.front().reason;
        throw py::ValueError(reason && !reason->empty() ? *reason : "guest_rejected");
    }
    const Json& participant = detail::get(results.front().effect, "participant");
    return participant.is_object() ? participant : Json::object();
}

Json MeetingEngine::regenerate_tokens() {
    const std::string id = meeting_id();
    if (id.empty()) {
        throw EngineError("No active meeting");
    }
    const auto [host_token, guest_token] = deps_.tokens();
    deps_.repository->replace_tokens(id, host_token, guest_token);
    const auto transport = server();
    const std::string host_url = transport ? transport->host_url() : "";
    const std::string guest_url = transport ? transport->guest_url() : "";
    emit("server_started", Json{{"url", transport ? transport->base_url() : ""},
                                {"host_url", host_url},
                                {"guest_url", guest_url}});
    if (transport) {
        transport->invalidate_connections();
    }
    return Json{{"host_url", host_url}, {"guest_url", guest_url}};
}

std::filesystem::path MeetingEngine::spool_root() const {
    return options_.spool_root.empty() ? std::filesystem::path("meeting_audio")
                                       : options_.spool_root;
}

// ---- transcript reads ----

std::vector<Json> MeetingEngine::get_transcript(double after_start_s,
                                                std::optional<std::int64_t> limit) const {
    const std::string id = meeting_id();
    if (id.empty()) {
        return {};
    }
    return deps_.repository->get_segments(id, after_start_s, limit);
}

std::vector<Json> MeetingEngine::get_transcript_delivery_page(std::int64_t after_seq,
                                                              std::int64_t limit) const {
    const std::string id = meeting_id();
    if (id.empty()) {
        return {};
    }
    return deps_.repository->get_segments_delivery_page(id, after_seq, limit);
}

std::int64_t MeetingEngine::get_transcript_delivery_cursor(std::string_view consumer) const {
    const std::string id = meeting_id();
    if (id.empty()) {
        return 0;
    }
    return deps_.repository->get_delivery_cursor(id, consumer);
}

void MeetingEngine::advance_transcript_delivery_cursor(std::string_view consumer,
                                                       std::int64_t seq) {
    const std::string id = meeting_id();
    if (!id.empty()) {
        deps_.repository->advance_delivery_cursor(id, consumer, seq);
    }
}

std::int64_t MeetingEngine::get_transcript_delivery_high_water() const {
    const std::string id = meeting_id();
    if (id.empty()) {
        return 0;
    }
    return deps_.repository->get_delivery_high_water(id);
}

} // namespace fv::meeting
