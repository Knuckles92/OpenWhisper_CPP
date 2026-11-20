// MeetingRuntime: the bridge from engine events to the desktop, and shutdown
// (services/runtime/meeting.py _queue_engine_event, _route_engine_event, _forward_public_event,
// _on_engine_event and its handlers, _apply_finalization, _shutdown_engine, cleanup).
//
// Events are queued on the thread that emits them, with the retry generation and claim current
// then, and routed on the UI actor, so even events queued before a background handoff cannot
// change a new meeting's card, URLs, active flag or model lease.

#include <condition_variable>
#include <exception>
#include <format>
#include <thread>
#include <utility>

#include "automation/meeting_events.h"
#include "meeting/detail/engine_internal.h"
#include "meeting/detail/py_value.h"
#include "meeting/detail/runtime_internal.h"
#include "meeting/runtime.h"
#include "meeting/runtime_messages.h"
#include "meeting_state/schema.h"

namespace fv::meeting {

namespace {

const log::Logger& logger = detail::runtime_logger;

// Runs every task on the posting thread: engine listeners queue their event where it happens.
class InlineExecutor final : public Executor {
public:
    void post(Task task) override { task(); }
    TimerHandle post_after(std::chrono::milliseconds, Task task) override {
        task();
        return {};
    }
    bool is_current() const override { return true; }
};

InlineExecutor& inline_executor() {
    static InlineExecutor executor;
    return executor;
}

bool generation_kind(std::string_view kind) {
    return kind == "status" || kind == "retry_finalization" || kind == "retry_status";
}

std::optional<std::string> optional_text(const Json& value) {
    if (value.is_string()) {
        return value.get<std::string>();
    }
    return std::nullopt;
}

} // namespace

void MeetingRuntime::listen(const std::shared_ptr<RuntimeEngine>& engine) {
    std::weak_ptr<RuntimeEngine> weak = engine;
    Subscription subscription =
        engine->add_listener(inline_executor(), [this, weak](const MeetingEvent& event) {
            if (const auto source = weak.lock()) {
                queue_engine_event(source, event.kind, event.payload);
            }
        });
    Subscription previous;
    {
        const std::lock_guard lock(mutex_);
        previous = std::exchange(listeners_[engine.get()], std::move(subscription));
    }
}

void MeetingRuntime::queue_engine_event(const std::shared_ptr<RuntimeEngine>& engine,
                                        std::string kind, Json payload,
                                        std::optional<std::uint64_t> retry_generation) {
    QueuedEvent event;
    {
        const std::lock_guard lock(mutex_);
        event.generation = retry_generation ? *retry_generation : retry_generation_;
        event.claim = retry_claim_;
    }
    if (!payload.is_object()) {
        payload = Json::object();
    }
    // A status event's finalization carries the card's content summary. It is read here, on the
    // emitting thread, because the repository is never read on the UI actor.
    if (kind == "status" && engine) {
        if (auto found = payload.find("finalization");
            found != payload.end() && found->is_object()) {
            const auto summary = found->find("content_summary");
            if (summary == found->end() || !summary->is_object()) {
                (*found)["content_summary"] = meeting_content_summary(engine->meeting_id());
            }
        }
    }
    event.engine = engine;
    event.kind = std::move(kind);
    event.payload = std::move(payload);
    post_ui([this, event = std::move(event)] { route_engine_event(event); });
}

void MeetingRuntime::route_engine_event(const QueuedEvent& event) {
    const std::string& kind = event.kind;
    const Json& payload = event.payload;
    std::shared_ptr<RuntimeEngine> current;
    {
        const std::lock_guard lock(mutex_);
        // A pause, resume or end call on this engine is in flight: its outcome goes first.
        if (event.engine) {
            if (const auto held = held_events_.find(event.engine.get());
                held != held_events_.end()) {
                held->second.events.push_back(event);
                return;
            }
        }
        // A retained engine can serve several retries: source identity alone cannot reject a
        // previous retry's queued terminal event.
        if (generation_kind(kind) && event.generation != retry_generation_) {
            return;
        }
        if (generation_kind(kind) && event.claim && !event.claim->is_current()) {
            return;
        }
        if ((kind == "retry_finalization" || kind == "retry_status") &&
            optional_text(detail::get(payload, "meeting_id")) != card_meeting_id_) {
            return;
        }
        current = engine_;
    }
    try {
        if (kind == "retry_finalization") {
            Json state = payload;
            const Json finalization = detail::get(state, "finalization");
            apply_finalization(finalization, state,
                               optional_text(detail::get(state, "meeting_id")));
            meeting_state_changed.emit(state);
        } else if (kind == "retry_status") {
            meeting_status_update.emit(detail::py_str(detail::get(payload, "message")));
        } else if (event.engine && event.engine == current) {
            on_engine_event(current, kind, payload);
            const std::string meeting_id = current->meeting_id();
            try {
                automation::meeting_events::forward(kind, payload, meeting_id, events_);
            } catch (const std::exception& error) {
                logger.debug("Could not publish a meeting event: {}", error.what());
            }
            meeting_engine_event.emit(meeting_id, kind, payload);
        }
    } catch (const std::exception& error) {
        logger.error_with(error, std::format("Error routing meeting event '{}'", kind));
    }
}

void MeetingRuntime::on_engine_event(const std::shared_ptr<RuntimeEngine>& engine,
                                     const std::string& kind, const Json& payload) {
    // Finalization outcomes are non-modal status lines, never meeting_error.
    try {
        // Only the browser dashboard shows these: command acknowledgements reach it over its own
        // socket, and segments stream to it.
        if (kind == "voice_feedback" || kind == "segments") {
            return;
        }
        if (kind == "status") {
            on_status_event(engine, payload);
        } else if (kind == "server_started") {
            on_server_started_event(payload);
        } else if (kind == "error") {
            on_error_event(payload);
        } else if (kind == "background_ready") {
            on_background_ready_event();
        } else if (kind == "asr_released") {
            // Not on "ended": that fires before the offline final pass, which still decodes on
            // the meeting's model.
            restore_local_engine();
        } else if (kind == "ended") {
            on_ended_event(payload);
        } else if (kind == "intelligence") {
            on_intelligence_event(payload);
        } else {
            logger.debug("Unhandled meeting engine event: {}", kind);
        }
    } catch (const std::exception& error) {
        logger.error("Error bridging meeting event '{}': {}", kind, error.what());
    }
}

void MeetingRuntime::on_status_event(const std::shared_ptr<RuntimeEngine>& engine,
                                     const Json& payload) {
    const Json& status_value = detail::get(payload, "status");
    const std::string status = detail::truthy(status_value) ? detail::py_str(status_value) : "";
    const Json& finalization = detail::get(payload, "finalization");
    Json state = Json::object();
    if (const Json& cloud = detail::get(payload, "cloud_enabled"); cloud.is_boolean()) {
        state["cloud_enabled"] = cloud;
    }
    if (const Json& speech = detail::get(payload, "speech"); speech.is_object()) {
        state["speech"] = speech;
    }
    if (!status.empty()) {
        state["status"] = status;
        // Pause and resume can come from the dashboard, which changes only the engine status;
        // the tab mirrors it.
        if (status == "active" || status == "paused") {
            state["paused"] = status == "paused";
            state.update(clock_fields(engine));
        }
        // A human note when there is one, else the short lifecycle status.
        const Json& note = detail::get(payload, "note");
        meeting_status_update.emit(detail::truthy(note) ? detail::py_str(note) : status);
    }
    if (finalization.is_object()) {
        apply_finalization(finalization, state, std::nullopt);
    } else if (finalization.is_null() && payload.is_object() && payload.contains("finalization")) {
        {
            const std::lock_guard lock(mutex_);
            finalizing_ = retry_meeting_id_.has_value();
            finalization_.reset();
        }
        sync_activity();
        state["finalization"] = nullptr;
    }
    if (!state.empty()) {
        // Capture ownership is engine-derived: a status alone never flips active. After End,
        // active stays false while finalization updates.
        bool controller_active = false;
        {
            const std::lock_guard lock(mutex_);
            controller_active = controller_meeting_active_;
        }
        if (!state.contains("active") && !is_active() && !controller_active) {
            state["active"] = false;
        }
        meeting_state_changed.emit(state);
    }
}

void MeetingRuntime::on_server_started_event(const Json& payload) {
    {
        const std::lock_guard lock(mutex_);
        if (const std::string host = detail::text_of(payload, "host_url"); !host.empty()) {
            host_url_ = host;
        }
        if (const std::string guest = detail::text_of(payload, "guest_url"); !guest.empty()) {
            guest_url_ = guest;
        }
    }
    meeting_server_started.emit(detail::public_payload(payload));
}

void MeetingRuntime::on_error_event(const Json& payload) {
    const Json& message_value = detail::get(payload, "message");
    const std::string message =
        detail::truthy(message_value) ? detail::py_str(message_value) : detail::py_str(payload);
    {
        // Non-fatal warnings wait until the required start checks pass; a failed start shows only
        // its own error.
        const std::lock_guard lock(mutex_);
        if (starting_) {
            deferred_start_errors_.push_back(message);
            return;
        }
    }
    meeting_error.emit(message);
}

void MeetingRuntime::on_background_ready_event() {
    {
        const std::lock_guard lock(mutex_);
        background_ready_ = true;
    }
    restore_local_engine();
    Json state = Json::object();
    state["background_available"] = true;
    meeting_state_changed.emit(state);
}

void MeetingRuntime::on_ended_event(const Json& payload) {
    std::optional<Json> finalization;
    bool finalizing = false;
    {
        const std::lock_guard lock(mutex_);
        controller_meeting_active_ = false;
        finalization = finalization_;
        finalizing = finalizing_;
    }
    sync_activity();
    std::string status = detail::text_of(payload, "status");
    if (status.empty()) {
        status = "ended";
    }
    meeting_status_update.emit(ended_message(payload, status, finalizing));
    Json state = Json::object();
    state["active"] = false;
    state["status"] = status;
    if (finalization && detail::truthy(*finalization)) {
        state["finalization"] = *finalization;
    }
    meeting_state_changed.emit(state);
}

void MeetingRuntime::on_intelligence_event(const Json& payload) {
    const bool online = payload.is_object() && payload.contains("online")
                            ? detail::truthy(payload.at("online"))
                            : detail::truthy(detail::get(payload, "intelligence_online"));
    meeting_status_update.emit(
        std::string(online ? messages::kIntelligenceOnline : messages::kIntelligenceOffline));
}

void MeetingRuntime::apply_finalization(const Json& finalization, Json& state_payload,
                                        std::optional<std::string> meeting_id) {
    if (!meeting_id) {
        const std::lock_guard lock(mutex_);
        if (engine_) {
            if (std::string id = engine_->meeting_id(); !id.empty()) {
                meeting_id = std::move(id);
            }
        }
    }
    Json normalized = meeting_state::FinalizationState::coerce(finalization).to_json();
    // The summary is read where the event was queued (the repository is never read here).
    const Json& summary = detail::get(finalization, "content_summary");
    normalized["content_summary"] = summary.is_object() ? summary : Json::object();
    const std::string status = detail::text_of(normalized, "status");
    const std::string message = detail::text_of(normalized, "message");
    const bool terminal = detail::terminal_finalization(status);
    {
        const std::lock_guard lock(mutex_);
        const bool retry_running = retry_meeting_id_.has_value();
        finalization_.emplace(normalized);
        finalizing_ = status == "running" || retry_running;
        if (meeting_id) {
            card_meeting_id_ = meeting_id;
        }
    }
    sync_activity();
    state_payload["finalization"] = normalized;
    if (status == "running") {
        meeting_status_update.emit(message.empty() ? std::string(messages::kPreparingFinalInsights)
                                                   : message);
    } else if (terminal && !message.empty()) {
        // Persistent non-modal feedback, never an error dialog.
        meeting_status_update.emit(message);
    }
}

void MeetingRuntime::shutdown_engine(std::optional<std::chrono::milliseconds> timeout) {
    std::shared_ptr<RuntimeEngine> engine;
    Subscription listener;
    {
        const std::lock_guard lock(mutex_);
        engine = std::move(engine_);
        engine_.reset();
        // Retained dashboard links go only when the engine itself is torn down (a new meeting or
        // app exit), not merely when capture ends.
        host_url_.reset();
        guest_url_.reset();
        finalizing_ = false;
        background_ready_ = false;
        if (engine) {
            if (const auto found = listeners_.find(engine.get()); found != listeners_.end()) {
                listener = std::move(found->second);
                listeners_.erase(found);
            }
        }
    }
    sync_activity();
    listener.disconnect();
    if (!engine) {
        return;
    }
    // A speaker model download would hold the engine's shutdown, and its destruction, until
    // the transfer ended.
    cancel_engine_work(engine.get());
    const auto shutdown = [](const std::shared_ptr<RuntimeEngine>& target) {
        try {
            target->shutdown();
        } catch (const std::exception& error) {
            logger.warning("Error shutting down meeting engine: {}", error.what());
        }
    };
    if (!timeout) {
        shutdown(engine);
        return;
    }
    struct Done {
        std::mutex mutex;
        std::condition_variable changed;
        bool done = false;
    };
    auto done = std::make_shared<Done>();
    // The thread owns everything it touches, so it can be left running past the budget.
    std::thread worker([engine, done, shutdown] {
        detail::name_engine_thread("meeting-shutdown");
        shutdown(engine);
        {
            const std::lock_guard lock(done->mutex);
            done->done = true;
        }
        done->changed.notify_all();
    });
    bool finished = false;
    {
        std::unique_lock lock(done->mutex);
        finished = done->changed.wait_for(lock, *timeout, [&done] { return done->done; });
    }
    if (finished) {
        worker.join();
        return;
    }
    worker.detach();
    logger.warning("Meeting engine shutdown still running after {:.1f}s; abandoning it so the "
                   "application can exit.",
                   std::chrono::duration<double>(*timeout).count());
}

void MeetingRuntime::cleanup() {
    // Runs after the window and tray are gone: a full engine shutdown (ASR drain plus a final
    // consolidation) would keep an invisible process for minutes, so the waits are bounded. The
    // meeting is already persisted; only the tail of a pass is at risk.
    shutdown_.cancel();
    shutdown_archive_dashboard();
    shutdown_engine(kShutdownJoinTimeout);
    const auto deadline = std::chrono::steady_clock::now() + kShutdownJoinTimeout;
    {
        std::unique_lock lock(mutex_);
        background_done_.wait_until(lock, deadline, [this] { return background_workers_.empty(); });
        controller_meeting_active_ = false;
    }
    sync_activity();
}

} // namespace fv::meeting
