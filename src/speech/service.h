#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include "base/cancel.h"
#include "base/executor.h"
#include "base/json.h"
#include "base/signal.h"
#include "engine/error.h"
#include "engine/recognizer.h"
#include "engine/transcript.h"

namespace fv::speech {

struct AudioClip { // mono float at 16 kHz unless rate says otherwise
    std::shared_ptr<const std::vector<float>> samples;
    std::size_t begin = 0, end = 0;
    int rate = 16000;
    double offset_s = 0.0; // where begin sits in the recording (source time)
};

// The queue order is the policy: Final > RemoteClient > Meeting > Incremental > Preview >
// Background > Warmup.
enum class Lane { Final, RemoteClient, Meeting, Incremental, Preview, Background, Warmup };

// Which engine a job is for: dictation's, or the meeting's own when meeting_speech_separate
// picks a different model.
enum class Slot { Dictation, Meeting };

struct DecodeJob {
    std::variant<AudioClip, std::filesystem::path> input; // path only for cloud uploads
    RecognitionOptions options; // language, speech contexts (outlive the request)
    Lane lane = Lane::Final;
    std::string coalesce_key; // a newer job with this key replaces a queued one
    CancelToken cancel;
    // The meeting slot falls back to the dictation slot's engine while nothing was selected for
    // it (the same model for both, the default).
    Slot slot = Slot::Dictation;
    // Vocabulary hints (RecognitionContext.phrases): starred terms first, already cut to the
    // budget. Only a backend whose supports_recognition() is true reads them.
    std::vector<std::string> phrases;
    // Status of the steps inside one long decode (splitting a large file, uploading its
    // chunks): (message, transcribing), transcribing true from the step that starts the
    // transcription proper. Called on the thread that reached the step; never with a lock held.
    std::function<void(const std::string& message, bool transcribing)> on_progress;
};

// Segment and word times are seconds from the clip's `begin`; add AudioClip::offset_s for
// source time.
struct DecodeResult {
    Transcript transcript;
    double decode_s = 0;
    std::uint64_t generation = 0;
};

struct Capabilities {
    bool windows = true;
    bool live_preview = true;
    bool accepts_files = false;
    double max_window_s = 30.0;
    std::vector<std::string> languages;
    // MiB above which the backend splits a file before sending it (the cloud upload limit);
    // unset for a backend that takes any size in one pass.
    std::optional<double> split_over_mb;
};

enum class ReadyState {
    Ready,
    Loading,
    Warming,
    ModelMissing,
    RuntimeMissing,
    NeedsKey,
    Offline,
    Failed
};

// Ready: device_info is the readout ("Parakeet TDT 0.6B v3 | cuda"), message a note shown after
// it (a CPU fallback) or empty. Any other state: device_info is the engine's sentence ("Download
// Parakeet TDT 0.6B v3 in Downloads."), and message the same sentence or the error's.
struct Readiness {
    ReadyState state;
    std::string device_info;
    std::string message;
};

// load/decode/release run on the backend's lane thread.
class Backend {
public:
    virtual ~Backend() = default;
    virtual std::string_view id() const = 0; // "parakeet", "api", "remote"
    virtual Capabilities capabilities() const = 0;
    virtual Readiness load(const CancelToken&) = 0;
    virtual DecodeResult decode(const DecodeJob&) = 0; // throws EngineError (full sentence)
    virtual void release() = 0;
    // For backends whose authenticated connection can fail between decode jobs.
    virtual Subscription watch_readiness(Executor&, std::function<void(Readiness)>) { return {}; }

    // One throwaway decode so the first real one after a load is warm (warm-up is best-effort:
    // never throws). True when the decode ran; false when skipped, failed or cut short by
    // `superseded` (a reload or cancel while it ran). The base has nothing to warm.
    virtual bool warm_up(const CancelToken& superseded);
    // "model" when dictionary phrases reach the speech model, else "after" (transcriber/base.py
    // recognition_support).
    virtual std::string recognition_support() const;
    // The file's size in MiB when the backend splits it for upload; nullopt (the default) when
    // it decodes any size in one pass.
    virtual std::optional<double> large_file_size_mb(const std::filesystem::path&) const;

    // Whether decode reads DecodeJob::phrases and the language as recognition hints
    // (transcriber/base.py supports_recognition). False by default.
    virtual bool supports_recognition() const;
    // Whether prepare_for_transcription has work to do; the dictation runtime then calls it on
    // a thread of its own each time a recording starts. False by default.
    virtual bool prepares_for_transcription() const;
    // Gets ready, while a recording is made, to decode it when it stops. May block; must not
    // touch the network (the recording may yet be cancelled); never throws. Nothing by default.
    virtual void prepare_for_transcription();

    // on_progress: every status a decode reports, after the job's own on_progress. Emitted on
    // the reporting thread to subscribers whose executor is current, posted to the rest.
    Signal<std::string, bool> progress;

protected:
    // transcriber/base.py _report_progress: the job's callback, then the progress signal.
    void report_progress(const DecodeJob&, const std::string& message, bool transcribing) const;
};

// What a slot should run: the backend key (a03's selected_model: "parakeet", "api", "remote")
// and the settings snapshot the factory reads that backend's keys from, plus what
// models::resolve_parakeet found for a local engine.
struct BackendConfig {
    std::string backend;
    std::shared_ptr<const Json> settings;

    std::filesystem::path runtime_dir;     // the NeMo-Speech.cpp release folder
    std::filesystem::path model_path;      // the model's .gguf
    std::string model_key;                 // "parakeet-v3"
    std::string model_label;               // "Parakeet TDT 0.6B v3"
    std::string requested_device = "auto"; // the setting: "auto", "cpu" or "cuda"
    std::string device = "cpu";            // what it resolved to: "cpu" or "cuda"
    int gpu_index = 0;                     // the runtime's GPU index when device is "cuda"
    // Set when the runtime or the model is not installed: why (the backend's own sentence is
    // used when empty) and which (ModelMissing or RuntimeMissing).
    std::optional<std::string> missing_message;
    ReadyState missing_state = ReadyState::Ready;
};

// The a03 backend table, keyed by BackendConfig::backend. Returns null for a key it has no
// backend for.
using BackendFactory = std::function<std::unique_ptr<Backend>(const BackendConfig&)>;

namespace detail {
struct ServiceState;
struct JobState;
} // namespace detail

// A submitted job. Cancelling drops it if still queued; a running window finishes and its
// result is discarded. Destroying the handle does not cancel.
// Affinity: thread-safe.
class JobHandle {
public:
    JobHandle() = default; // no job
    std::uint64_t id() const;
    void cancel();

private:
    friend class SpeechService;
    JobHandle(std::uint64_t id, std::weak_ptr<detail::ServiceState> service);
    std::uint64_t id_ = 0;
    std::weak_ptr<detail::ServiceState> service_;
};

// Held while a meeting (or anything else) must not see the engine reloaded under it. RAII:
// destroying it releases the hold. Move-only.
// Affinity: thread-safe.
class EngineLease {
public:
    EngineLease(EngineLease&&) noexcept;
    EngineLease& operator=(EngineLease&&) noexcept;
    EngineLease(const EngineLease&) = delete;
    EngineLease& operator=(const EngineLease&) = delete;
    ~EngineLease();
    void release();

private:
    friend class SpeechService;
    EngineLease(std::weak_ptr<detail::ServiceState> service, std::uint64_t id);
    std::weak_ptr<detail::ServiceState> service_;
    std::uint64_t id_ = 0;
};

// Every NeMo call (runtime and model load, warm-up, decode) runs on the service's one thread,
// one at a time: jobs wait in lanes (Lane's order, then submission order), and an input of a
// backend's max_window_s or more is decoded one window at a time (cut where dsp::split_point
// cuts), so a Final job waits for at most one window of anything else. A select, release or restore
// advances the slot's generation at once; jobs submitted before it then end with "Transcription
// canceled". Signals are emitted on the `notify` executor, in the order the service raised them.
// Thread-safe facade; owns the "fv-speech" thread.
class SpeechService {
public:
    SpeechService(BackendFactory, Executor& notify);
    // Cancels every job (a running window finishes first), releases the backends on the speech
    // thread and joins it.
    ~SpeechService();
    SpeechService(const SpeechService&) = delete;
    SpeechService& operator=(const SpeechService&) = delete;

    // The callback runs on reply_on with the result, or with EngineError("Transcription
    // canceled") for a cancelled, replaced or superseded job.
    JobHandle submit(DecodeJob, Executor& reply_on,
                     std::function<void(std::expected<DecodeResult, EngineError>)>);
    DecodeResult run(DecodeJob); // blocking, for worker threads; asserts not GUI
    // A feature may supply a backend after startup. The constructor's factory is tried first.
    // An empty factory removes this key for subsequent loads. No backend runs on the caller.
    void set_backend_factory(std::string backend, BackendFactory factory);
    void select(Slot, BackendConfig); // debounced reload lives in appcore::EngineReloadMachine
    // Runs a cache mutation on the caller's worker only when this local model is not
    // resident or queued for either slot. Backend load/release cannot overlap the callback.
    bool with_model_unused(std::string_view model_key, const std::function<void()>& work);
    EngineLease lease(std::string_view holder); // blocks reloads while held
    // Releases the slot's engine (meeting model lease, 5.4); restore_local loads the same
    // config again. Readiness meanwhile: Offline, "Released while Meeting Mode runs".
    void release_local(Slot);
    void restore_local(Slot); // meeting model lease, 5.4
    Readiness readiness(Slot) const;
    Signal<Slot, Readiness> readiness_changed;
    Signal<bool> busy_changed;

    // One throwaway decode (Backend::warm_up) on the Warmup lane; skipped when anything else is
    // queued or running, the slot's generation moved, or nothing is loaded. warmed_up reports
    // whether the decode ran.
    void warm_up(Slot);
    Signal<Slot, bool> warmed_up;
    std::uint64_t generation(Slot) const;
    // Shutdown: ends every queued job with "Transcription canceled" and waits up to `budget`
    // for the running window to finish.
    void cancel_all(std::chrono::milliseconds budget);
    // Holders of the leases now held, in the order they were taken.
    std::vector<std::string> lease_holders() const;

    // The slot's loaded backend, reached from the caller's thread (OpenWhisper calls these on its
    // current backend directly): they run concurrently with the speech thread, so a backend that
    // answers them must do so thread-safely. With nothing loaded they answer as the Backend base
    // does (false, nullopt, nothing to do).
    bool supports_recognition(Slot) const;
    // Whether the backend loaded in the slot takes a whole file (capabilities().accepts_files):
    // a file decode then goes to it as a path, never cut into windows. False with nothing
    // loaded.
    bool accepts_files(Slot) const;
    // Whether the backend loaded in the slot decodes windows a preview can use
    // (capabilities().live_preview: Parakeet, local or on a paired host). False with nothing
    // loaded.
    bool live_preview(Slot) const;
    bool prepares_for_transcription(Slot) const;
    // Blocking (the backend may wait on disk or a lock): a thread of its own at recording start,
    // never the GUI thread.
    void prepare_for_transcription(Slot);
    std::optional<double> large_file_size_mb(Slot, const std::filesystem::path&) const;

private:
    std::shared_ptr<detail::ServiceState> state_;
};

} // namespace fv::speech
