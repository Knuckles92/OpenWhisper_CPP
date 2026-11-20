#pragma once

#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>

#include "appcore/activity.h"
#include "base/executor.h"
#include "base/signal.h"
#include "speech/service.h"

namespace fv::appcore {

// config.WHISPER_RELOAD_DEBOUNCE_MS: rapid engine changes coalesce into one reload.
inline constexpr std::chrono::milliseconds kReloadDebounce{400};

// When the dictation engine loads, reloads, warms up and is lent to Meeting Mode: OpenWhisper's
// reload state machine (application_controller.py 454-694, 1438-1491) for the Parakeet engine.
// It decides; the driver does. Commands go out as Signals and the driver answers with the
// matching input: start_load with on_load_finished, start_warmup with on_warmup_finished.
//
//   Settled -> Pending (request; 400 ms debounce, re-armed while recording, transcribing, a host
//   update or another reload runs; a meeting cancels it) -> InFlight (busy) -> [Warming] ->
//   finish (not busy; missing-runtime offer; preview set-up) -> Settled.
//
// The driver connects ActivityState::changed to on_activity and tells the machine which engine
// is selected (select) before it asks for a reload.
// Affinity: UI actor on the executor it is given; request(), settled() and in_flight() may be
// called from any thread.
class EngineReloadMachine {
public:
    explicit EngineReloadMachine(Executor& ui);
    ~EngineReloadMachine();
    EngineReloadMachine(const EngineReloadMachine&) = delete;
    EngineReloadMachine& operator=(const EngineReloadMachine&) = delete;

    // Schedules a debounced reload of the selected engine (Settings, the engine fields, a paired
    // computer, a readiness check). Refused, with a status and busy_changed(false), during a
    // meeting or while recording or transcribing. A non-empty note replaces "Reloading speech
    // engine..." as the next reload's status ("Switching to ..."). Any thread: settled() is
    // false when it returns; the debounce is armed at once on the UI executor, posted otherwise.
    void request(std::string_view note = {});
    // No reload running or queued.
    bool settled() const;
    bool in_flight() const;

    // The first load, after the main window is shown ("Loading speech engine..."). The preview
    // is set up once it has settled.
    void initial_load();

    // The selected engine (backend key, "parakeet") and its model (the missing-runtime offer
    // names it). A load still running for another backend is discarded when it ends.
    void select(std::string backend, std::string model);
    // The latest activity; read when a request or the debounce timer decides.
    void on_activity(const ActivitySnapshot& activity);
    // Whether the live preview is set up on the dictation engine; a lease stops it.
    void set_preview_active(bool active);
    // The preview waits for the engine to load (OpenWhisper's StreamingRuntime._wait_for_engine):
    // the next finished load asks for its set-up again with streaming_setup_due.
    void defer_streaming_setup();

    // The answer to start_load. Ready: device_info is the readout ("Parakeet TDT 0.6B v3 |
    // cuda") and a non-empty message is a note shown after it (a CPU fallback). ModelMissing,
    // RuntimeMissing and the other not-ready states: device_info is the engine's sentence.
    // Failed: the load threw; message is the error's sentence.
    void on_load_finished(const speech::Readiness& readiness);
    // The answer to start_warmup, whether the warm-up decode ran, failed or was cut short.
    void on_warmup_finished();
    // The loaded engine was closed by something other than a lease (a Cancel).
    void on_engine_closed();

    // Meeting Mode borrows the engine's memory (release_local_engine). True when a loaded
    // engine was released; the driver releases it in the speech service. The restore is owed
    // until restore_after_lease, even across further releases. During a load it returns true
    // and the release takes effect when the load ends; a load that leaves no engine loaded
    // (failed, model or runtime missing, switched away) released nothing, as OpenWhisper's
    // lease, which waits for the load, finds nothing to free: no restore is owed for it.
    bool release_for_lease();
    // Reloads the engine a lease released (restore_local_engine); a no-op unless one was. During
    // a reload the finish of that reload restarts it when the lease closed what it loaded.
    void restore_after_lease();
    bool released_for_lease() const;

    // Quit: the timer stops and nothing more is started or finished.
    void shutdown();

    // Commands.
    Signal<> start_load;             // load the selected engine (release the others first)
    Signal<> start_warmup;           // one throwaway decode on the engine just loaded
    Signal<> discard_load;           // release the engine just loaded: another one is selected now
    Signal<> stop_preview;           // a lease is taking the engine: clean up the live preview
    Signal<> ensure_model_available; // the model is missing: start the download consent flow
    Signal<bool> busy_changed;
    Signal<std::string> status;
    Signal<std::string, std::optional<bool>> device_info; // (readout, engine is usable)
    Signal<std::string> missing_runtime;                  // offer the model's runtime
    Signal<> streaming_setup_due;                         // set up the live preview now

private:
    void arm();
    void on_timer();
    void begin_worker();
    void finish();
    void submit_restore_reload();
    void flush_pending_streaming_setup();
    ActivitySnapshot activity() const;

    Executor& ui_;
    std::shared_ptr<int> alive_ = std::make_shared<int>(0);

    std::atomic<bool> pending_{false};
    std::atomic<bool> in_flight_{false};
    std::atomic<bool> shutting_down_{false};

    mutable std::mutex mutex_; // guards note_ and activity_, which request() reads off-thread
    std::string note_;
    ActivitySnapshot activity_;

    TimerHandle timer_;
    std::string backend_;
    std::string model_;
    std::string loading_backend_;
    std::string loading_model_;
    bool loading_ = false;
    bool warming_ = false;
    bool available_ = false;
    bool model_missing_ = false;
    bool preview_active_ = false;
    bool pending_streaming_setup_ = false;
    bool released_for_lease_ = false;
    bool restore_after_reload_ = false;
    bool release_owed_ = false;         // a lease arrived during a load; it takes effect at its end
    bool released_before_owed_ = false; // released_for_lease_ before that lease
};

} // namespace fv::appcore
