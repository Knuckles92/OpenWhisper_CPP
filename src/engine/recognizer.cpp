#include "engine/recognizer.h"

#include <system_error>
#include <utility>

#include "engine/error.h"
#include "engine/nemo_api.h"
#include "engine/platform.h"
#include "engine/request_options.h"
#include "engine/runtime.h"

namespace fv {

namespace {

// Throws the runtime's message for a failed call. Must run on the thread that
// made the call, before any other runtime call: the message is thread-local.
void check(const NemoApi& api, nemo_speech_asr_status status) {
    if (status == NEMO_SPEECH_ASR_OK) {
        return;
    }
    const char* message = api.last_error();
    if (message != nullptr && *message != '\0') {
        throw EngineError(message);
    }
    throw EngineError("Speech runtime error " + std::to_string(static_cast<int>(status)) + ".");
}

std::string text_or_empty(const char* text) {
    return text != nullptr ? std::string(text) : std::string();
}

struct ResultDestroyer {
    const NemoApi* api;
    void operator()(nemo_speech_asr_result* result) const { api->result_destroy(result); }
};
using OwnedResult = std::unique_ptr<nemo_speech_asr_result, ResultDestroyer>;

// Copies a result out of the runtime. The caller fills in the segments.
Transcript read_result(const NemoApi& api, const nemo_speech_asr_result* result) {
    Transcript transcript;
    transcript.text = text_or_empty(api.result_transcript(result, 0));
    const std::size_t word_count = api.result_word_count(result, 0);
    transcript.words.reserve(word_count);
    for (std::size_t i = 0; i < word_count; ++i) {
        transcript.words.push_back(Word{
            text_or_empty(api.result_word_text(result, 0, i)),
            api.result_word_start_time(result, 0, i) / 1000.0,
            api.result_word_end_time(result, 0, i) / 1000.0,
        });
    }
    transcript.audio_processed = api.result_audio_processed(result);
    return transcript;
}

} // namespace

struct Recognizer::Impl {
    const NemoApi& api;
    nemo_speech_asr_recognizer* handle = nullptr;

    explicit Impl(const NemoApi& api_) : api(api_) {}

    ~Impl() {
        if (handle != nullptr) {
            api.destroy(handle);
        }
    }
};

Recognizer::Recognizer(const RecognizerConfig& config) {
    std::error_code error;
    if (!std::filesystem::is_regular_file(config.model_path, error)) {
        throw EngineError("The model file " + path_to_utf8(config.model_path) + " does not exist.");
    }
    impl_ = std::make_unique<Impl>(NemoApi::get(find_runtime_library(config.runtime_dir)));

    nemo_speech_asr_backend_config backend{};
    backend.size = sizeof(backend);
    backend.gpu = config.gpu;

    const std::string model_path = path_to_utf8(config.model_path);
    nemo_speech_asr_model_config model{};
    model.size = sizeof(model);
    model.path = model_path.c_str();

    nemo_speech_asr_recognizer_config recognizer{};
    recognizer.size = sizeof(recognizer);
    recognizer.backend = &backend;
    recognizer.model = &model;

    check(impl_->api, impl_->api.create(&recognizer, &impl_->handle));
}

Recognizer::~Recognizer() = default;

Transcript Recognizer::transcribe(std::span<const float> samples, int sample_rate,
                                  const RecognitionOptions& options) {
    const NemoApi& api = impl_->api;
    const nemo_speech_asr_recognition_options native =
        request_options(api.recognition_options_default(), options.language);
    nemo_speech_asr_result* raw = nullptr;
    const nemo_speech_asr_status status = api.recognize_f32(impl_->handle, &native, samples.data(),
                                                            samples.size(), sample_rate, &raw);
    // Owned before the status is checked: a result handed back with an error is freed too.
    const OwnedResult result(raw, ResultDestroyer{&api});
    check(api, status);
    if (result == nullptr) {
        throw EngineError("The speech runtime returned no result.");
    }
    Transcript transcript = read_result(api, result.get());
    // A rate of 0 means "the model's own rate", which only the runtime knows.
    const double duration = sample_rate > 0 ? static_cast<double>(samples.size()) / sample_rate
                                            : transcript.audio_processed;
    transcript.segments = segments_from_words(transcript.text, transcript.words, duration);
    return transcript;
}

std::string Recognizer::runtime_version() const {
    return text_or_empty(impl_->api.version());
}

} // namespace fv
