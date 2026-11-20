#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "base/clock.h"
#include "base/executor.h"
#include "base/signal.h"
#include "base/subscription.h"
#include "speech/window_preview.h"

namespace fv {
class SettingsStore;
namespace appcore {
class EngineReloadMachine;
}
namespace speech {
class SpeechService;
}
} // namespace fv

namespace fv::dictation {

class RecorderPort;

// The dictation's live preview: when it is set up, started, muted, stopped, finished and
// canceled (services/runtime/streaming.py StreamingRuntime), over one speech::WindowPreview that
// decodes windows of the recorder's tape on the dictation engine. FioriVoce previews only with
// Parakeet (and the paired host's engine, M9): the engine's own preview shares its decoder, so
// the preview loads nothing of its own. Native streams and the tiny.en preview are not ported.
//
// The preview exists while `streaming_enabled` is on and the dictation engine can preview; while
// the engine loads, setup waits and runs again on EngineReloadMachine::streaming_setup_due
// (attach()). enabled() is OpenWhisper's controller._streaming_enabled: on only while a preview
// is set up (or a set-up waits for the engine, which leaves it off).
//
// Affinity: UI actor, except stop_session(), which any thread may call (the stop's worker calls
// it after the post-roll, so that the post-roll audio stays in the preview for finalize()), and
// finalize(), which blocks and runs on worker threads only. Signals are emitted on the UI
// thread. The service, recorder, settings, executor and clock must outlive the runtime.
class StreamingRuntime {
public:
    // Builds the preview a set-up uses. Empty: a speech::WindowPreview over the runtime's
    // service, UI executor and clock (a test passes one that fails, as a set-up can).
    using PreviewFactory =
        std::function<std::shared_ptr<speech::WindowPreview>(const speech::WindowPreviewOptions&)>;

    StreamingRuntime(speech::SpeechService& speech, RecorderPort& recorder, SettingsStore& settings,
                     Executor& ui, Clock& clock, PreviewFactory make_preview = {});
    ~StreamingRuntime();
    StreamingRuntime(const StreamingRuntime&) = delete;
    StreamingRuntime& operator=(const StreamingRuntime&) = delete;

    // Follows the engine's reloads: a set-up that waits for the engine runs again on
    // streaming_setup_due, a lease's stop_preview cleans the preview up, and the machine is told
    // whether a preview is set up. `reload` must outlive the runtime. UI thread.
    void attach(appcore::EngineReloadMachine& reload);

    // setup_streaming: sets the preview up unless one exists that still fits the engine, or a
    // recording is using it. Startup and every finished engine load call it.
    void setup();
    // reconfigure_streaming: the settings or the engine changed; refused while recording ("Stop
    // recording before changing streaming mode"). A failure says "Failed to reconfigure
    // streaming"; an engine that cannot preview says why.
    void reconfigure();
    // The preview's text: shown until the stop begins, and published while enabled.
    void on_partial_transcription(std::string text, bool final);

    // start_streaming_session, for the recording job `job_id` that just started on the recorder.
    // Skipped (no overlay) while the last recording's preview still finishes a window; that
    // recording then has no preview text, and its finalize() returns nullopt (unlike
    // OpenWhisper, whose fallback showed the previous recording's text). When
    // enabled, streaming_overlay_show is emitted before this returns, so a subscriber on the UI
    // executor marks the streaming flow active before the recording's Listening overlay.
    void start_session(std::string job_id);
    // begin_stop_streaming_session: no more text reaches the screen; the post-roll still feeds
    // the preview.
    void begin_stop();
    // stop_streaming_session: stops without decoding and returns the text so far ("" without a
    // preview). The window it left is decoded only by finalize(). Any thread.
    std::string stop_session();
    // finalize_streaming_text: the stopped preview's complete text, decoding the audio stop
    // left; nullopt without a preview, when the preview has nothing to finish, when it fails
    // (logged), or when the recording the calling thread last stopped had no preview of its own
    // (its start was skipped). Blocks up to the preview's deadline plus the decode. Worker
    // threads only: the one that called stop_session() for the recording.
    std::optional<std::string> finalize();
    // cancel_streaming_session: discards the preview's text, its kept audio and any late output
    // without waiting for a decode, and hides the preview overlay when enabled.
    void cancel_session();
    // Removes the preview (quit, a lease taking the engine). A finalize() still running on a
    // worker returns at once without its text: Quit's streaming step calls this before it waits
    // for the jobs strand, whose worker may be inside finalize().
    void cleanup();

    // controller._streaming_enabled.
    bool enabled() const;
    // A preview is set up (controller.streaming_transcriber is not None).
    bool configured() const;
    // Seconds of new audio per preview window (the streaming_chunk_duration it was set up
    // with); nullopt without a preview.
    std::optional<double> chunk_duration() const;
    // The clock previews are timed by (the transcription runtime's preview throttle runs on it).
    Clock& clock() const { return clock_; }

    // The preview text as it grows: (text, final), final meaning it replaces what was shown.
    Signal<std::string, bool> partial_transcription;
    // The same text while the preview is enabled and the text is not empty: the overlay's
    // header or footer and the transcription.preview event.
    Signal<std::string, bool> streaming_text_update;
    Signal<> streaming_overlay_show;
    Signal<> streaming_overlay_hide;
    // A status line for the user (ui_controller.set_status).
    Signal<std::string> status;

private:
    // What the preview was set up for: the selected engine and the dictation slot's generation,
    // which every reload, model or device change advances (_engine_identity).
    using Identity = std::pair<std::string, std::uint64_t>;

    Identity engine_identity() const;
    bool engine_ready() const;
    void configure(bool initial_setup);
    bool preview_engine(const std::string& engine, bool initial_setup);
    void wait_for_engine();
    void use_window_stream(double chunk_duration_s);
    void cleanup_streaming_resources();
    std::shared_ptr<speech::WindowPreview> current() const;
    // Lets go of `preview` on the UI thread, where a WindowPreview is destroyed: off it, the
    // reference is kept in `retired_` until a UI task drops it, so no preview (which uses the
    // service, executor and clock) outlives the runtime.
    void release(std::shared_ptr<speech::WindowPreview> preview);

    speech::SpeechService& speech_;
    RecorderPort& recorder_;
    SettingsStore& settings_;
    Executor& ui_;
    Clock& clock_;
    PreviewFactory make_preview_;
    appcore::EngineReloadMachine* reload_ = nullptr;

    mutable std::mutex mutex_; // guards preview_, retired_ and chunk_duration_
    std::shared_ptr<speech::WindowPreview> preview_;
    std::vector<std::shared_ptr<speech::WindowPreview>> retired_;
    std::optional<double> chunk_duration_;
    std::atomic<bool> enabled_{false};
    std::atomic<bool> stopping_{false};
    // Whether the recording started last has a preview of its own (its start was not skipped).
    std::atomic<bool> session_previewed_{false};
    // For each thread that stopped a recording, whether that recording had a preview: finalize()
    // reads the caller's. The transcription runtime stops and finalizes a recording on its job's
    // worker, while a stop refused as busy (a later recording, during that job) stops on the UI
    // thread, so the busy stop never takes the earlier recording's fallback away.
    std::mutex stops_mutex_;
    std::map<std::thread::id, bool> stopped_previewed_;
    std::optional<Identity> configured_for_; // UI thread

    std::vector<Subscription> links_;
    std::shared_ptr<int> alive_ = std::make_shared<int>(0);
};

} // namespace fv::dictation
