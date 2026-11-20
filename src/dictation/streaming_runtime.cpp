#include "dictation/streaming_runtime.h"

#include <chrono>
#include <exception>
#include <mutex>
#include <thread>
#include <utility>

#include "appcore/engine_reload.h"
#include "base/log.h"
#include "dictation/recorder_port.h"
#include "pyport/number.h"
#include "pyport/text.h"
#include "settings/keys.h"
#include "settings/store.h"
#include "speech/dictation_language.h"
#include "speech/preview_status.h"
#include "speech/service.h"

namespace fv::dictation {

namespace {

constexpr log::Logger kLog{"services.runtime.streaming"};

// The engine's name in the log lines (LocalSpeechBackend.name).
std::string engine_name(const std::string& engine) {
    if (engine == "parakeet") {
        return "Parakeet";
    }
    if (engine == "remote") {
        return "Remote";
    }
    return engine;
}

} // namespace

StreamingRuntime::StreamingRuntime(speech::SpeechService& speech, RecorderPort& recorder,
                                   SettingsStore& settings, Executor& ui, Clock& clock,
                                   PreviewFactory make_preview)
    : speech_(speech), recorder_(recorder), settings_(settings), ui_(ui), clock_(clock),
      make_preview_(std::move(make_preview)) {}

StreamingRuntime::~StreamingRuntime() {
    links_.clear();
    alive_.reset();
    // The previews go here, on the UI thread, while the service, executor and clock still live:
    // a window decoding on the speech thread delivers its result through them, and each
    // preview's destructor waits for a delivery already under way.
    std::vector<std::shared_ptr<speech::WindowPreview>> previews;
    {
        std::lock_guard lock(mutex_);
        previews = std::exchange(retired_, {});
        previews.push_back(std::exchange(preview_, nullptr));
    }
    previews.clear();
}

void StreamingRuntime::attach(appcore::EngineReloadMachine& reload) {
    reload_ = &reload;
    links_.push_back(reload.streaming_setup_due.connect(ui_, [this] { setup(); }));
    // A lease takes the engine's memory: the preview goes, and the machine sets it up again
    // after the restore.
    links_.push_back(reload.stop_preview.connect(ui_, [this] { cleanup(); }));
    reload.set_preview_active(configured());
}

bool StreamingRuntime::enabled() const {
    return enabled_.load();
}

bool StreamingRuntime::configured() const {
    return current() != nullptr;
}

std::optional<double> StreamingRuntime::chunk_duration() const {
    std::lock_guard lock(mutex_);
    return chunk_duration_;
}

std::shared_ptr<speech::WindowPreview> StreamingRuntime::current() const {
    std::lock_guard lock(mutex_);
    return preview_;
}

void StreamingRuntime::release(std::shared_ptr<speech::WindowPreview> preview) {
    if (!preview || ui_.is_current()) {
        return;
    }
    // Not handed to the executor's queue: a queued task may outlive the runtime and the clock.
    {
        std::lock_guard lock(mutex_);
        retired_.push_back(std::move(preview));
    }
    ui_.post([this, alive = std::weak_ptr<int>(alive_)] {
        if (!alive.lock()) {
            return; // the destructor dropped them
        }
        std::vector<std::shared_ptr<speech::WindowPreview>> retired;
        {
            std::lock_guard lock(mutex_);
            retired = std::exchange(retired_, {});
        }
    });
}

StreamingRuntime::Identity StreamingRuntime::engine_identity() const {
    return {speech::engine(*settings_.snapshot()), speech_.generation(speech::Slot::Dictation)};
}

bool StreamingRuntime::engine_ready() const {
    // A reload asked for or running replaces the engine now loaded, which may be another
    // engine than the one selected.
    if (reload_ != nullptr && !reload_->settled()) {
        return false;
    }
    const speech::ReadyState state = speech_.readiness(speech::Slot::Dictation).state;
    return state == speech::ReadyState::Ready || state == speech::ReadyState::Warming;
}

// --- set-up -----------------------------------------------------------------------------

void StreamingRuntime::setup() {
    if (configured()) {
        // A remote engine asks again after every reload, since the host may run another engine
        // than when this preview was set up. A preview that still fits, or that a recording is
        // using, stays.
        if (recorder_.is_recording() || configured_for_ == engine_identity()) {
            return;
        }
        cleanup_streaming_resources();
    }
    configure(true);
}

void StreamingRuntime::reconfigure() {
    kLog.info("Reconfiguring streaming transcription...");
    if (recorder_.is_recording()) {
        kLog.warning("Cannot reconfigure streaming while recording");
        status.emit("Stop recording before changing streaming mode");
        return;
    }
    cleanup_streaming_resources();
    configure(false);
}

void StreamingRuntime::configure(bool initial_setup) {
    try {
        const auto settings = settings_.snapshot();
        enabled_ = keys::streaming_enabled.read(*settings);
        if (!enabled_) {
            kLog.info("Streaming transcription disabled");
            return;
        }
        if (!preview_engine(speech::engine(*settings), initial_setup)) {
            return;
        }
        use_window_stream(keys::streaming_chunk_duration.read(*settings));
    } catch (const std::exception& error) {
        kLog.error_with(error, "Failed to setup streaming");
        enabled_ = false;
        if (!initial_setup) {
            status.emit("Failed to reconfigure streaming");
        }
    }
}

bool StreamingRuntime::preview_engine(const std::string& engine, bool initial_setup) {
    if (engine == "remote" && !engine_ready()) {
        // Until it connects, a remote engine has no family to check: the host says which engine
        // it runs. The reload runs set-up again once the connection settles.
        kLog.info("Streaming preview waits for the remote engine to connect");
        wait_for_engine();
        return false;
    }
    std::string reason;
    if (engine == "remote") {
        // The paired computer says which engine it runs once it has connected; one that cannot
        // preview is named, as the engine card names it.
        const auto host = speech::remote_voice_capabilities(*settings_.snapshot());
        reason = speech::preview_unavailable_reason(engine, host ? host->host : std::string(),
                                                    host ? std::optional<std::string>(host->family)
                                                         : std::nullopt);
    } else {
        reason = speech::preview_unavailable_reason(engine);
    }
    if (reason.empty()) {
        if (!engine_ready()) {
            // The engine is still loading or waiting on a download; the reload runs set-up
            // again once it has finished.
            kLog.info("Streaming preview waits for {} to load", engine_name(engine));
            wait_for_engine();
            return false;
        }
        kLog.info("Streaming preview shares the loaded {} engine", engine_name(engine));
        return true;
    }
    kLog.info("Streaming requested but not available for this backend");
    if (!initial_setup) {
        status.emit(reason);
    }
    enabled_ = false;
    return false;
}

void StreamingRuntime::wait_for_engine() {
    enabled_ = false;
    if (reload_ != nullptr) {
        reload_->defer_streaming_setup();
    }
}

void StreamingRuntime::use_window_stream(double chunk_duration_s) {
    speech::WindowPreviewOptions options;
    options.chunk_duration_s = chunk_duration_s;
    auto preview = make_preview_
                       ? make_preview_(options)
                       : std::make_shared<speech::WindowPreview>(speech_, ui_, clock_, options);
    {
        std::lock_guard lock(mutex_);
        preview_ = std::move(preview);
        chunk_duration_ = chunk_duration_s;
    }
    configured_for_ = engine_identity();
    if (reload_ != nullptr) {
        reload_->set_preview_active(true);
    }
    kLog.info("Streaming transcription enabled (chunk_duration={}s)",
              py::float_repr(chunk_duration_s));
}

void StreamingRuntime::cleanup() {
    cleanup_streaming_resources();
}

void StreamingRuntime::cleanup_streaming_resources() {
    configured_for_.reset();
    std::shared_ptr<speech::WindowPreview> preview;
    {
        std::lock_guard lock(mutex_);
        preview = std::exchange(preview_, nullptr);
        chunk_duration_.reset();
    }
    if (reload_ != nullptr) {
        reload_->set_preview_active(false);
    }
    if (!preview) {
        return;
    }
    try {
        preview->cleanup();
        kLog.info("Cleaned up existing streaming transcriber");
    } catch (const std::exception& error) {
        kLog.warning("Error cleaning up streaming transcriber: {}", error.what());
    }
}

// --- sessions ---------------------------------------------------------------------------

void StreamingRuntime::on_partial_transcription(std::string text, bool final) {
    if (stopping_) {
        return;
    }
    partial_transcription.emit(text, final);
    if (enabled_ && !text.empty()) {
        streaming_text_update.emit(text, final);
    }
}

void StreamingRuntime::start_session(std::string job_id) {
    session_previewed_ = false;
    const auto preview = current();
    if (!preview) {
        return;
    }
    stopping_ = false;
    std::weak_ptr<int> alive = alive_;
    preview->start(recorder_.tape(), std::move(job_id),
                   [this, alive](std::string text, bool final) {
                       if (alive.lock()) {
                           on_partial_transcription(std::move(text), final);
                       }
                   });
    if (!preview->running()) {
        // The last recording's window is still decoding (stop no longer waits for it), so this
        // one records without a preview; the preview overlay would stay empty throughout.
        kLog.warning("Live preview skipped: the previous preview is still finishing");
        return;
    }
    session_previewed_ = true;
    kLog.info("Streaming transcription started");
    if (enabled_) {
        streaming_overlay_show.emit();
    }
}

void StreamingRuntime::begin_stop() {
    stopping_ = true;
}

std::string StreamingRuntime::stop_session() {
    const bool previewed = session_previewed_.load();
    {
        std::lock_guard lock(stops_mutex_);
        stopped_previewed_[std::this_thread::get_id()] = previewed;
    }
    auto preview = current();
    if (!preview) {
        return "";
    }
    if (!previewed) {
        // Deliberately unlike OpenWhisper: the preview still holds the previous recording's text
        // and kept audio, which are not this recording's (and the previous one may still finish
        // them). One still following an old tape is stopped all the same, or every later start
        // would be refused too.
        if (preview->running()) {
            stopping_ = true;
            preview->stop();
        }
        return "";
    }
    stopping_ = true;
    const auto started = clock_.steady();
    std::string text = preview->stop();
    const double elapsed_ms =
        std::chrono::duration<double, std::milli>(clock_.steady() - started).count();
    kLog.info("Streaming transcription stopped in {} ms, got {} chars",
              py::format_fixed(elapsed_ms, 0), py::len(text));
    release(std::move(preview));
    return text;
}

std::optional<std::string> StreamingRuntime::finalize() {
    {
        std::lock_guard lock(stops_mutex_);
        const auto found = stopped_previewed_.find(std::this_thread::get_id());
        if (found != stopped_previewed_.end() && !found->second) {
            kLog.info("No live preview to finish: this recording's preview never started");
            return std::nullopt;
        }
    }
    auto preview = current();
    if (!preview) {
        return std::nullopt;
    }
    std::optional<std::string> text;
    try {
        text = preview->finalize();
    } catch (const std::exception& error) {
        kLog.warning("Could not finish the live preview text: {}", error.what());
        text.reset();
    }
    release(std::move(preview));
    return text;
}

void StreamingRuntime::cancel_session() {
    stopping_ = true;
    if (const auto preview = current()) {
        preview->cancel();
        kLog.info("Streaming transcription canceled");
    }
    if (enabled_) {
        streaming_overlay_hide.emit();
    }
}

} // namespace fv::dictation
