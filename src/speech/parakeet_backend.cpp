#include "speech/parakeet_backend.h"

#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <numbers>
#include <random>
#include <stdexcept>
#include <system_error>
#include <utility>

#include "base/log.h"
#include "engine/error.h"
#include "engine/platform.h"
#include "engine/runtime.h"
#include "settings/keys.h"
#include "speech/languages.h"
#include "speech/messages.h"
#include "speech/window_decode.h"

namespace fv::speech {

namespace {

constexpr log::Logger kLog{"transcriber.optional_backend"};

constexpr std::string_view kBackendId = "parakeet";
constexpr std::string_view kName = "Parakeet"; // catalog BACKENDS["parakeet"]
constexpr std::string_view kCanceled = kTranscriptionCanceled;

// warmup() input: one second of seeded noise at about -40 dBFS. Quiet enough that Parakeet
// returns no text for it, but well above kSilencePeak, so unlike zeros it reaches the engine.
constexpr std::size_t kWarmupSamples = 16000;
constexpr double kWarmupLevel = 0.01;

class RecognizerDecoder final : public Decoder {
public:
    explicit RecognizerDecoder(const RecognizerConfig& config) : recognizer_(config) {}
    Transcript transcribe(std::span<const float> samples,
                          const RecognitionOptions& options) override {
        return recognizer_.transcribe(samples, 16000, options);
    }

private:
    Recognizer recognizer_;
};

// Standard normal noise from a fixed seed (Box-Muller over mt19937, which every standard
// library implements the same way), so every warm-up decodes the same samples. OpenWhisper uses
// numpy's default_rng(0); the warm-up only needs quiet, non-silent audio, not those samples.
const std::vector<float>& warmup_noise() {
    static const std::vector<float> noise = [] {
        std::mt19937 generator(0);
        std::vector<float> made;
        made.reserve(kWarmupSamples);
        while (made.size() < kWarmupSamples) {
            const double u1 = (static_cast<double>(generator()) + 1.0) / 4294967297.0;
            const double u2 = static_cast<double>(generator()) / 4294967296.0;
            const double radius = std::sqrt(-2.0 * std::log(u1));
            const double angle = 2.0 * std::numbers::pi * u2;
            made.push_back(static_cast<float>(radius * std::cos(angle) * kWarmupLevel));
            made.push_back(static_cast<float>(radius * std::sin(angle) * kWarmupLevel));
        }
        made.resize(kWarmupSamples);
        return made;
    }();
    return noise;
}

// "Loaded the NeMo-Speech.cpp {CPU|GPU} runtime from {folder}", once: the first load fixes the
// runtime for the process (architecture 5.7).
void log_runtime_once(const std::filesystem::path& runtime_dir) {
    static std::atomic<bool> logged{false};
    if (logged.exchange(true)) {
        return;
    }
    kLog.info("Loaded the NeMo-Speech.cpp {} runtime from {}",
              runtime_has_gpu_backend(runtime_dir) ? "GPU" : "CPU", path_to_utf8(runtime_dir));
}

struct TranscribingFlag {
    std::atomic<bool>& flag;
    explicit TranscribingFlag(std::atomic<bool>& f) : flag(f) { flag = true; }
    ~TranscribingFlag() { flag = false; }
    TranscribingFlag(const TranscribingFlag&) = delete;
    TranscribingFlag& operator=(const TranscribingFlag&) = delete;
};

} // namespace

DecoderFactory recognizer_factory() {
    return [](const RecognizerConfig& config) -> std::unique_ptr<Decoder> {
        return std::make_unique<RecognizerDecoder>(config);
    };
}

ParakeetBackend::ParakeetBackend(BackendConfig config, DecoderFactory factory)
    : config_(std::move(config)), factory_(std::move(factory)) {}

ParakeetBackend::~ParakeetBackend() = default;

std::string_view ParakeetBackend::id() const {
    return kBackendId;
}

Capabilities ParakeetBackend::capabilities() const {
    Capabilities capabilities;
    capabilities.windows = true;
    capabilities.live_preview = true;
    capabilities.accepts_files = false;
    capabilities.max_window_s = 30.0;
    for (const std::string_view code : language_choices(kBackendId)) {
        capabilities.languages.emplace_back(code);
    }
    return capabilities;
}

std::string ParakeetBackend::name() const {
    return std::string(kName);
}

bool ParakeetBackend::is_model_missing() const {
    return config_.missing_state == ReadyState::ModelMissing;
}

std::optional<std::string> ParakeetBackend::last_loaded_model() const {
    if (is_available()) {
        return config_.model_key;
    }
    return std::nullopt;
}

std::string ParakeetBackend::device_info() const {
    std::lock_guard lock(mutex_);
    if (decoder_ && !closed_) {
        return config_.model_label + " | " + device_;
    }
    return last_error_.empty() ? name() + " is not loaded" : last_error_;
}

bool ParakeetBackend::is_available() const {
    std::lock_guard lock(mutex_);
    return decoder_ != nullptr && !closed_;
}

std::uint64_t ParakeetBackend::generation() const {
    std::lock_guard lock(mutex_);
    return generation_;
}

std::string ParakeetBackend::last_error() const {
    std::lock_guard lock(mutex_);
    return last_error_;
}

std::string ParakeetBackend::cpu_fallback_note() const {
    std::lock_guard lock(mutex_);
    return cpu_fallback_note_;
}

std::string ParakeetBackend::device() const {
    std::lock_guard lock(mutex_);
    return device_;
}

bool ParakeetBackend::is_transcribing() const {
    return is_transcribing_;
}

bool ParakeetBackend::should_cancel() const {
    return should_cancel_;
}

void ParakeetBackend::reset_cancel_flag() {
    should_cancel_ = false;
}

std::shared_ptr<Decoder> ParakeetBackend::current_decoder() const {
    std::lock_guard lock(mutex_);
    return closed_ ? nullptr : decoder_;
}

void ParakeetBackend::drop_closed_decoder() {
    std::shared_ptr<Decoder> closed;
    {
        std::lock_guard lock(mutex_);
        if (closed_) {
            closed = std::exchange(decoder_, nullptr);
            closed_ = false;
        }
    }
    // Destroyed here, on the lane thread, outside the lock.
}

void ParakeetBackend::cleanup() {
    std::shared_ptr<Decoder> closed;
    {
        std::lock_guard lock(mutex_);
        ++generation_;
        closed = std::exchange(decoder_, nullptr);
        closed_ = false;
    }
}

void ParakeetBackend::supersede() {
    std::lock_guard lock(mutex_);
    ++generation_;
    closed_ = decoder_ != nullptr;
}

void ParakeetBackend::cancel_transcription() {
    should_cancel_ = true;
    supersede();
}

void ParakeetBackend::release() {
    cleanup();
}

void ParakeetBackend::reload_model(const CancelToken& cancel) {
    std::uint64_t generation = 0;
    if (cancel.cancelled()) {
        return;
    }
    cleanup();
    reset_cancel_flag();
    {
        std::lock_guard lock(mutex_);
        generation = generation_;
        cpu_fallback_note_.clear();
    }
    if (config_.backend != kBackendId) {
        throw std::invalid_argument("Model does not belong to this backend");
    }
    const std::string requested = config_.requested_device;
    std::string device = config_.device;
    if (config_.missing_state == ReadyState::RuntimeMissing) {
        const bool gpu = device == "cuda" || device == "mps" || device == "metal";
        std::lock_guard lock(mutex_);
        last_error_ = config_.missing_message.value_or(
            "Install " + name() + "'s " + (gpu ? "GPU" : "CPU") + " runtime in Downloads.");
        return;
    }
    if (is_model_missing()) {
        std::lock_guard lock(mutex_);
        last_error_ =
            config_.missing_message.value_or("Download " + config_.model_label + " in Downloads.");
        return;
    }

    std::unique_ptr<Decoder> made;
    for (;;) {
        {
            std::lock_guard lock(mutex_);
            if (generation != generation_ || cancel.cancelled()) {
                return;
            }
        }
        RecognizerConfig recognizer;
        recognizer.runtime_dir = config_.runtime_dir;
        recognizer.model_path = config_.model_path;
        recognizer.gpu = device == "cuda" ? config_.gpu_index : kCpuDevice;
        try {
            made = factory_(recognizer);
            break;
        } catch (const EngineError& error) {
            if (requested == "auto" && device != "cpu") {
                // Auto picked a GPU the runtime cannot use; the CPU still transcribes.
                kLog.warning("{} cannot use this GPU; loading on the CPU: {}", name(),
                             error.what());
                {
                    std::lock_guard lock(mutex_);
                    cpu_fallback_note_ = name() + " is using the CPU. " + error.what();
                }
                device = "cpu";
                continue;
            }
            load_failed(error, device, generation, cancel);
            throw;
        } catch (const std::exception& error) {
            load_failed(error, device, generation, cancel);
            throw;
        }
    }
    bool published = false;
    {
        std::lock_guard lock(mutex_);
        if (generation == generation_ && !cancel.cancelled()) {
            decoder_ = std::move(made);
            closed_ = false;
            device_ = device;
            last_error_.clear();
            published = true;
        }
    }
    // Superseded while it loaded: never published, destroyed here on this thread.
    made.reset();
    if (published) {
        log_runtime_once(config_.runtime_dir);
    }
}

void ParakeetBackend::load_failed(const std::exception& error, const std::string& device,
                                  std::uint64_t generation, const CancelToken& cancel) {
    bool current = false;
    {
        std::lock_guard lock(mutex_);
        current = generation == generation_;
        if (current) {
            last_error_ = error.what();
        }
    }
    if (current && !cancel.cancelled()) {
        kLog.error("{} failed to load on {}: {}", name(), device, error.what());
    }
}

Readiness ParakeetBackend::load(const CancelToken& cancel) {
    try {
        reload_model(cancel);
    } catch (const std::exception& error) {
        return Readiness{ReadyState::Failed, error.what(), error.what()};
    }
    if (is_available()) {
        return Readiness{ReadyState::Ready, device_info(), cpu_fallback_note()};
    }
    const std::string error = last_error();
    if (!error.empty() && config_.missing_state == ReadyState::RuntimeMissing) {
        return Readiness{ReadyState::RuntimeMissing, error, error};
    }
    if (!error.empty() && is_model_missing()) {
        return Readiness{ReadyState::ModelMissing, error, error};
    }
    // Cancelled or superseded before it could finish.
    return Readiness{ReadyState::Loading, device_info(), ""};
}

std::string ParakeetBackend::recognition_support() const {
    return "after";
}

std::string ParakeetBackend::request_language() const {
    const Json empty = Json::object();
    const Json& settings = config_.settings ? *config_.settings : empty;
    return selected_language(kBackendId, keys::local_asr_language.read(settings));
}

std::string ParakeetBackend::recognition_language(std::string_view code) const {
    if (code.empty()) {
        return request_language();
    }
    return selected_language(kBackendId, code);
}

std::string ParakeetBackend::job_language(const DecodeJob& job) const {
    switch (job.lane) {
    case Lane::Final:
    case Lane::Incremental:
    case Lane::Preview:
    case Lane::Warmup:
        // transcribe and decode_window take recognition_inputs' language (the preview sends
        // "auto", which maps to itself).
        return recognition_language(job.options.language);
    case Lane::RemoteClient:
    case Lane::Meeting:
    case Lane::Background:
        // recognize() and SpeechDecoder pass the caller's language as it is: the remote client
        // chose it for this engine already, and the meeting asks for its own ("auto" or a code).
        break;
    }
    return job.options.language;
}

void ParakeetBackend::check_cancel(const CancelToken& cancel) const {
    if (should_cancel_ || cancel.cancelled()) {
        throw EngineError(std::string(kCanceled));
    }
}

Transcript ParakeetBackend::recognize(std::span<const float> window, const std::string& language,
                                      const CancelToken& cancel) {
    check_cancel(cancel);
    std::uint64_t generation = 0;
    std::shared_ptr<Decoder> decoder;
    std::string error;
    {
        std::lock_guard lock(mutex_);
        generation = generation_;
        if (!closed_) {
            decoder = decoder_;
        }
        error = last_error_;
    }
    if (decoder == nullptr) {
        throw EngineError(error.empty() ? "Speech engine is not loaded" : error);
    }
    RecognitionOptions options;
    options.language = language.empty() ? request_language() : language;
    Transcript transcript = decoder->transcribe(window, options);
    check_cancel(cancel);
    if (this->generation() != generation) {
        // A reload or cancel closed the engine while it decoded; the result belongs to it.
        throw EngineError(std::string(kCanceled));
    }
    return transcript;
}

Transcript ParakeetBackend::transcribe_audio(std::span<const float> audio,
                                             const std::string& language,
                                             const CancelToken& cancel) {
    drop_closed_decoder();
    struct Drop {
        ParakeetBackend* backend;
        ~Drop() { backend->drop_closed_decoder(); }
    } drop{this};
    return speech::transcribe_audio(
        audio, [&](std::span<const float> window) { return recognize(window, language, cancel); },
        [&] { check_cancel(cancel); });
}

DecodeResult ParakeetBackend::decode(const DecodeJob& job) {
    const auto* clip = std::get_if<AudioClip>(&job.input);
    if (clip == nullptr) {
        throw std::invalid_argument("Parakeet decodes audio clips, not files");
    }
    if (clip->rate != 16000 || clip->samples == nullptr || clip->end < clip->begin ||
        clip->end > clip->samples->size()) {
        throw std::invalid_argument("Parakeet decodes 16 kHz clips within their samples");
    }
    // transcribe_windows' refusals: a cancelled job, then an engine that is not loaded, before
    // anything else, so a silent clip cannot hide either.
    check_cancel(job.cancel);
    drop_closed_decoder();
    if (!is_available()) {
        throw EngineError(device_info());
    }
    const auto started = std::chrono::steady_clock::now();
    DecodeResult result;
    result.generation = generation();
    const std::span<const float> audio(clip->samples->data() + clip->begin,
                                       clip->end - clip->begin);
    result.transcript = transcribe_audio(audio, job_language(job), job.cancel);
    result.decode_s =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
    return result;
}

std::string ParakeetBackend::decode_window(std::span<const float> audio,
                                           const std::string& language) {
    return transcribe_audio(audio, language).text;
}

std::vector<std::string> ParakeetBackend::transcribe_windows(std::span<const AudioClip> windows,
                                                             const std::string& language) {
    std::size_t next = 0;
    return transcribe_pulled(
        [&]() -> std::optional<AudioClip> {
            if (next == windows.size()) {
                return std::nullopt;
            }
            return windows[next++];
        },
        language);
}

std::vector<std::string>
ParakeetBackend::transcribe_pulled(const std::function<std::optional<AudioClip>()>& next,
                                   const std::string& language) {
    const TranscribingFlag transcribing(is_transcribing_);
    if (should_cancel_) {
        throw EngineError(std::string(kCanceled));
    }
    if (!is_available()) {
        throw EngineError(device_info());
    }
    std::vector<std::string> texts;
    while (std::optional<AudioClip> clip = next()) {
        const std::span<const float> audio(clip->samples->data() + clip->begin,
                                           clip->end - clip->begin);
        texts.push_back(transcribe_audio(audio, language).text);
    }
    return texts;
}

std::string ParakeetBackend::transcribe(const std::filesystem::path& audio_path,
                                        const std::string& language) {
    FileWindows windows(audio_path);
    const std::vector<std::string> texts =
        transcribe_pulled([&] { return windows.next(); }, language);
    return join_texts(texts);
}

bool ParakeetBackend::warm_up(const CancelToken& superseded) {
    return warmup(superseded);
}

bool ParakeetBackend::warmup(const CancelToken& superseded) {
    drop_closed_decoder();
    std::uint64_t generation = 0;
    {
        std::lock_guard lock(mutex_);
        generation = generation_;
        if (decoder_ == nullptr || closed_) {
            return false;
        }
    }
    const auto is_superseded = [&] {
        return this->generation() != generation || should_cancel_ || superseded.cancelled();
    };
    const auto started = std::chrono::steady_clock::now();
    try {
        if (is_superseded()) {
            return false; // a reload landed before the decode could start
        }
        recognize(warmup_noise(), "", superseded);
    } catch (const std::exception& error) {
        if (is_superseded()) {
            kLog.info("{} warmup stopped by a reload or cancel", name());
        } else {
            kLog.warning("{} warmup failed (non-fatal): {}", name(), error.what());
        }
        drop_closed_decoder();
        return false;
    }
    const double ms =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started)
            .count();
    kLog.info("{} warmed up in {:.0f} ms", name(), ms);
    return true;
}

} // namespace fv::speech
