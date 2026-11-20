#pragma once

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "engine/recognizer.h"
#include "speech/file_windows.h"
#include "speech/service.h"

namespace fv::speech {

// What a ParakeetBackend decodes with: a loaded fv::Recognizer, or a test's stand-in.
// Affinity: one thread at a time (the backend's lane thread).
class Decoder {
public:
    virtual ~Decoder() = default;
    // Mono float at 16 kHz; throws EngineError with the runtime's sentence.
    virtual Transcript transcribe(std::span<const float> samples, const RecognitionOptions&) = 0;
};

// Loads a decoder for the config (runtime, model, GPU index or kCpuDevice); throws EngineError
// when the runtime or model is missing or rejected.
using DecoderFactory = std::function<std::unique_ptr<Decoder>(const RecognizerConfig&)>;

// fv::Recognizer behind Decoder.
DecoderFactory recognizer_factory();

// The local Parakeet engine (transcriber/optional_backend.py LocalSpeechBackend for "parakeet"),
// in process: the model loads into a Decoder on the lane thread instead of a worker process, so
// a cancel cannot abort a running decode; it is checked between windows and makes the running
// operation's result stale through the generation instead.
//
// Affinity: load, decode, warm_up, transcribe*, cleanup and release on the lane thread
// ("fv-speech"); cancel_transcription, supersede and the readers (generation, is_available,
// device_info, last_error, cpu_fallback_note, is_transcribing, should_cancel) from any thread.
class ParakeetBackend final : public Backend {
public:
    explicit ParakeetBackend(BackendConfig config, DecoderFactory factory = recognizer_factory());
    ~ParakeetBackend() override;

    std::string_view id() const override; // "parakeet"
    Capabilities capabilities() const override;
    // reload_model, as a readiness: Ready, ModelMissing, RuntimeMissing, Failed (the error's
    // sentence), or Loading when cancelled or superseded before it could finish.
    Readiness load(const CancelToken& cancel) override;
    // The clip through transcribe_audio. On the Final, Incremental and Preview lanes the job's
    // language goes through recognition_language (transcribe's and decode_window's
    // recognition_inputs: "" is the engine's setting, a code Parakeet does not list is "auto");
    // on the RemoteClient, Meeting and Background lanes it is sent as given (recognize() and
    // SpeechDecoder), "" still meaning the engine's setting. Throws
    // EngineError("Transcription canceled") when the job's token, a cancel or a supersede stops
    // it, and device_info()'s sentence when nothing is loaded, even for a silent clip.
    DecodeResult decode(const DecodeJob& job) override;
    void release() override; // cleanup
    bool warm_up(const CancelToken& superseded) override;
    std::string recognition_support() const override; // "after": phrases are applied after

    // LocalSpeechBackend.
    std::string name() const; // "Parakeet"
    bool is_model_missing() const;
    std::optional<std::string> last_loaded_model() const; // the model key while available
    std::string device_info() const; // "<label> | <device>", last_error or "Parakeet is not loaded"
    bool is_available() const;
    // Advances on every cleanup and supersede, so a changed value means a different engine.
    std::uint64_t generation() const;
    std::string last_error() const;
    // Why an Auto device runs on the CPU although a GPU was resolved ("" otherwise).
    std::string cpu_fallback_note() const;
    std::string device() const; // "cpu" or "cuda" once loaded
    bool is_transcribing() const;
    bool should_cancel() const;
    void reset_cancel_flag();

    // Loads the configured model, closing the current one first. Returns at once when `cancel`
    // is already set. A model or runtime that is missing leaves last_error saying so. Throws
    // std::invalid_argument("Model does not belong to this backend") for another family's
    // config, and the decoder's EngineError when the load fails (after an Auto device's GPU
    // failure has been retried on the CPU).
    void reload_model(const CancelToken& cancel = {});
    // Closes the engine and advances the generation.
    void cleanup();
    // Sets should_cancel, then supersede(): from any thread, so the decoder goes on the lane
    // thread once the operation that holds it ends (Python's cleanup() kills the worker).
    void cancel_transcription();
    // From any thread: advances the generation and marks the engine closed; the decoder itself
    // is destroyed on the lane thread when the operation that holds it ends, or by the next
    // lane call.
    void supersede();

    // The language a request made without one uses: local_asr_language from the config's
    // settings, through selected_language.
    std::string request_language() const;
    // The language a decode for a recognition context's language code sends: the engine's
    // setting when the context has none, else the code when Parakeet takes it, else "auto".
    // Parakeet's phrases never reach the model (recognition_support "after").
    std::string recognition_language(std::string_view code) const;

    // One throwaway decode of 1 s of seeded noise at about -40 dBFS: skipped when nothing is
    // loaded, ended quietly by a reload or cancel ("{name} warmup stopped by a reload or
    // cancel"), non-fatal on failure ("{name} warmup failed (non-fatal)"), logged ("{name}
    // warmed up in N ms"). True when the decode ran.
    bool warmup(const CancelToken& superseded = {});

    // _transcribe_audio on 16 kHz audio (see window_decode.h).
    Transcript transcribe_audio(std::span<const float> audio, const std::string& language,
                                const CancelToken& cancel = {});
    // One window's text, as transcribe_windows would decode it; leaves is_transcribing alone.
    std::string decode_window(std::span<const float> audio, const std::string& language);
    // Each window's text, in order, as one durable transcription: is_transcribing while it
    // runs; "Transcription canceled" or device_info() when cancelled or not loaded.
    std::vector<std::string> transcribe_windows(std::span<const AudioClip> windows,
                                                const std::string& language = {});
    // A file's text: its windows (file_windows) through transcribe_windows, joined.
    std::string transcribe(const std::filesystem::path& audio_path,
                           const std::string& language = {});

private:
    std::shared_ptr<Decoder> current_decoder() const;
    std::vector<std::string>
    transcribe_pulled(const std::function<std::optional<AudioClip>()>& next,
                      const std::string& language);
    void check_cancel(const CancelToken& cancel) const;
    std::string job_language(const DecodeJob& job) const;
    Transcript recognize(std::span<const float> window, const std::string& language,
                         const CancelToken& cancel);
    void drop_closed_decoder();
    void load_failed(const std::exception& error, const std::string& device,
                     std::uint64_t generation, const CancelToken& cancel);

    BackendConfig config_;
    DecoderFactory factory_;

    mutable std::mutex mutex_; // guards the fields below that other threads read
    std::shared_ptr<Decoder> decoder_;
    bool closed_ = false; // supersede() asked for decoder_ to go
    std::uint64_t generation_ = 0;
    std::string device_ = "cpu";
    std::string last_error_;
    std::string cpu_fallback_note_;

    std::atomic<bool> should_cancel_{false};
    std::atomic<bool> is_transcribing_{false};
};

} // namespace fv::speech
