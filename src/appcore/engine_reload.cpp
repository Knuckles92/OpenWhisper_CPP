#include "appcore/engine_reload.h"

#include <utility>

#include "base/log.h"

namespace fv::appcore {

namespace {

constexpr log::Logger kLog{"services.application_controller"};

constexpr std::string_view kEndMeetingFirst = "End the meeting before changing the engine";
constexpr std::string_view kReloading = "Reloading speech engine...";

} // namespace

EngineReloadMachine::EngineReloadMachine(Executor& ui) : ui_(ui) {}

EngineReloadMachine::~EngineReloadMachine() {
    timer_.cancel();
}

ActivitySnapshot EngineReloadMachine::activity() const {
    std::lock_guard lock(mutex_);
    return activity_;
}

void EngineReloadMachine::request(std::string_view note) {
    if (!note.empty()) {
        std::lock_guard lock(mutex_);
        note_ = note;
    }
    const ActivitySnapshot now = activity();
    if (now.meeting_active) {
        kLog.info("Ignoring whisper reload: a meeting is in progress");
        status.emit(std::string(kEndMeetingFirst));
        busy_changed.emit(false);
        return;
    }
    if (now.recording || now.transcribing) {
        kLog.info("Ignoring whisper reload: recording/transcribing in progress");
        status.emit("Finish recording before changing the engine");
        busy_changed.emit(false);
        return;
    }
    // Set before the timer exists, so another thread sees the reload coming.
    pending_ = true;
    if (ui_.is_current()) {
        arm();
        return;
    }
    // A timer belongs to the UI executor: requests from other threads arm it there.
    ui_.post([weak = std::weak_ptr<int>(alive_), this] {
        if (weak.lock()) {
            arm();
        }
    });
}

bool EngineReloadMachine::settled() const {
    return !in_flight_ && !pending_;
}

bool EngineReloadMachine::in_flight() const {
    return in_flight_;
}

void EngineReloadMachine::arm() {
    timer_ = ui_.post_after(kReloadDebounce, [weak = std::weak_ptr<int>(alive_), this] {
        if (weak.lock()) {
            on_timer();
        }
    });
}

void EngineReloadMachine::on_timer() {
    const ActivitySnapshot now = activity();
    if (now.recording || now.transcribing || now.host_update_active) {
        arm();
        return;
    }
    if (now.meeting_active) {
        kLog.info("Canceling queued whisper reload: meeting owns the engine");
        pending_ = false;
        {
            std::lock_guard lock(mutex_);
            note_.clear();
        }
        status.emit(std::string(kEndMeetingFirst));
        busy_changed.emit(false);
        return;
    }
    if (in_flight_) {
        // Retry shortly, so the newest settings win.
        arm();
        return;
    }
    in_flight_ = true;
    pending_ = false;
    std::string note;
    {
        std::lock_guard lock(mutex_);
        note.swap(note_);
    }
    busy_changed.emit(true);
    status.emit(note.empty() ? std::string(kReloading) : note);
    begin_worker();
}

void EngineReloadMachine::initial_load() {
    pending_streaming_setup_ = true;
    if (in_flight_) {
        return;
    }
    in_flight_ = true;
    busy_changed.emit(true);
    status.emit("Loading speech engine...");
    begin_worker();
}

void EngineReloadMachine::begin_worker() {
    if (shutting_down_) {
        return;
    }
    const ActivitySnapshot now = activity();
    if (now.meeting_active || now.recording || now.transcribing) {
        in_flight_ = false;
        busy_changed.emit(false);
        return;
    }
    loading_ = true;
    loading_backend_ = backend_;
    loading_model_ = model_;
    start_load.emit();
}

void EngineReloadMachine::select(std::string backend, std::string model) {
    backend_ = std::move(backend);
    model_ = std::move(model);
}

void EngineReloadMachine::on_activity(const ActivitySnapshot& activity) {
    std::lock_guard lock(mutex_);
    activity_ = activity;
}

void EngineReloadMachine::set_preview_active(bool active) {
    preview_active_ = active;
}

void EngineReloadMachine::defer_streaming_setup() {
    pending_streaming_setup_ = true;
}

void EngineReloadMachine::on_load_finished(const speech::Readiness& readiness) {
    if (!loading_) {
        kLog.debug("Ignoring a load result with no load running");
        return;
    }
    loading_ = false;
    const bool release_owed = std::exchange(release_owed_, false);
    if (release_owed &&
        (readiness.state != speech::ReadyState::Ready || loading_backend_ != backend_)) {
        // OpenWhisper's lease waits for the load under the engine lock, and this load finishes
        // (failure, missing model or runtime, a switch) before the lease runs; the lease then
        // finds no model to free, returns False and owes no restore. A restore asked for since
        // can only belong to this lease.
        released_for_lease_ = released_before_owed_;
        if (!released_for_lease_) {
            restore_after_reload_ = false;
        }
    }
    if (readiness.state == speech::ReadyState::Failed) {
        const std::string error =
            readiness.message.empty() ? readiness.device_info : readiness.message;
        kLog.error("Engine load failed: {}", error);
        available_ = false;
        model_missing_ = false;
        status.emit("Engine load failed: " + error);
        device_info.emit(error, false);
        finish();
        return;
    }
    if (loading_backend_ != backend_) {
        // Switched away during its load: cleaned up, never announced or warmed.
        available_ = false;
        model_missing_ = false;
        discard_load.emit();
        finish();
        return;
    }
    available_ = readiness.state == speech::ReadyState::Ready;
    model_missing_ = readiness.state == speech::ReadyState::ModelMissing;
    device_info.emit(readiness.device_info, available_);
    status.emit(readiness.device_info);
    if (!readiness.message.empty()) {
        status.emit(readiness.message);
    }
    if (release_owed && available_) {
        // The lease waited for this load; a warm-up would only meet the closed engine.
        available_ = false;
        device_info.emit("Released while Meeting Mode runs", false);
    }
    if (model_missing_) {
        ensure_model_available.emit();
    } else if (available_ && !shutting_down_) {
        // Ready already: a recording may start while the first decode's one-time cost is paid
        // here. Busy and the shared preview wait for it.
        warming_ = true;
        start_warmup.emit();
        return;
    }
    finish();
}

void EngineReloadMachine::on_warmup_finished() {
    if (!warming_) {
        return;
    }
    warming_ = false;
    finish();
}

void EngineReloadMachine::on_engine_closed() {
    available_ = false;
}

void EngineReloadMachine::finish() {
    if (shutting_down_) {
        return;
    }
    in_flight_ = false;
    const bool restore = std::exchange(restore_after_reload_, false);
    const bool unloaded = loading_backend_ == backend_ && !available_;
    if (restore && unloaded && !activity().meeting_active) {
        // A lease closed what this reload loaded and was returned before it finished, so
        // nothing else would load it again.
        submit_restore_reload();
        return;
    }
    busy_changed.emit(false);
    // An engine a lease released is not missing its runtime.
    const bool leased = restore || released_for_lease_;
    if (unloaded && !model_missing_ && !leased) {
        missing_runtime.emit(loading_model_);
    }
    // The preview shares the engine, so it is set up only once the load has settled.
    flush_pending_streaming_setup();
}

void EngineReloadMachine::submit_restore_reload() {
    in_flight_ = true;
    busy_changed.emit(true);
    status.emit(std::string(kReloading));
    begin_worker();
}

void EngineReloadMachine::flush_pending_streaming_setup() {
    if (shutting_down_) {
        return;
    }
    if (pending_streaming_setup_) {
        pending_streaming_setup_ = false;
        streaming_setup_due.emit();
    }
}

bool EngineReloadMachine::release_for_lease() {
    bool released = false;
    if (loading_) {
        // The load cannot be interrupted: the engine it brings up is released when it ends.
        if (!release_owed_) {
            released_before_owed_ = released_for_lease_;
        }
        release_owed_ = true;
        released = true;
    } else if (available_) {
        available_ = false;
        released = true;
    }
    if (preview_active_) {
        preview_active_ = false;
        pending_streaming_setup_ = true;
        stop_preview.emit();
    }
    released_for_lease_ = released_for_lease_ || released;
    if (released && !loading_) {
        device_info.emit("Released while Meeting Mode runs", false);
    }
    return released;
}

void EngineReloadMachine::restore_after_lease() {
    if (!released_for_lease_) {
        return;
    }
    released_for_lease_ = false;
    if (in_flight_) {
        // The reload in flight loads the model; if the lease closed what it loaded, its finish
        // restarts it.
        restore_after_reload_ = true;
        return;
    }
    submit_restore_reload();
}

bool EngineReloadMachine::released_for_lease() const {
    return released_for_lease_;
}

void EngineReloadMachine::shutdown() {
    shutting_down_ = true;
    timer_.cancel();
}

} // namespace fv::appcore
