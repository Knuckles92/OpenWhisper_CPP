#include "diarize/embedder.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <utility>

#include "base/log.h"
#include "base/utf8.h"
#include "diarize/ort_api.h"
#include "dsp/fbank.h"

#include "onnxruntime_c_api.h"

namespace fv::diarize {

namespace {

constexpr log::Logger logger("meeting.diarize.embedder");

// Python's int(MIN_AUDIO_SECONDS * SAMPLE_RATE).
constexpr auto kMinSamples = static_cast<std::size_t>(kMinAudioSeconds * dsp::kFbankSampleRate);

// Throws `prefix` + the status's message when `status` reports an error.
void check(const OrtApi& api, OrtStatus* status, const std::string& prefix) {
    if (status == nullptr) {
        return;
    }
    std::string message = api.GetErrorMessage(status);
    api.ReleaseStatus(status);
    throw EmbedderUnavailable(prefix + message);
}

bool contains_len(std::string name) {
    std::transform(name.begin(), name.end(), name.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return name.find("len") != std::string::npos;
}

} // namespace

struct OrtEmbeddingModel::Impl {
    const OrtApi* api = nullptr;
    OrtSession* session = nullptr;
    OrtMemoryInfo* memory = nullptr;
    std::string input_name;
    std::string length_name;
    std::string output_name;

    ~Impl() {
        if (memory != nullptr) {
            api->ReleaseMemoryInfo(memory);
        }
        if (session != nullptr) {
            api->ReleaseSession(session);
        }
    }

    std::string name_at(OrtAllocator* allocator, std::size_t index, bool input,
                        const std::string& prefix) const {
        char* raw = nullptr;
        check(*api,
              input ? api->SessionGetInputName(session, index, allocator, &raw)
                    : api->SessionGetOutputName(session, index, allocator, &raw),
              prefix);
        std::string name = raw != nullptr ? raw : "";
        if (raw != nullptr) {
            check(*api, api->AllocatorFree(allocator, raw), prefix);
        }
        return name;
    }
};

OrtEmbeddingModel::OrtEmbeddingModel(OrtRuntime& runtime, const std::filesystem::path& model)
    : impl_(std::make_unique<Impl>()) {
    const std::string prefix =
        "Failed to load speaker embedding model '" + path_to_utf8(model) + "': ";
    const OrtApi& api = runtime.api();
    impl_->api = &api;

    OrtSessionOptions* options = nullptr;
    check(api, api.CreateSessionOptions(&options), prefix);
    struct OptionsGuard {
        const OrtApi& api;
        OrtSessionOptions* options;
        ~OptionsGuard() { api.ReleaseSessionOptions(options); }
    } guard{api, options};
    check(api, api.SetInterOpNumThreads(options, 1), prefix);
    check(api, api.SetIntraOpNumThreads(options, 1), prefix);
    // The CPU execution provider is the session's default.
    check(api, api.CreateSession(runtime.env(), model.c_str(), options, &impl_->session), prefix);
    check(api, api.CreateCpuMemoryInfo(OrtArenaAllocator, OrtMemTypeDefault, &impl_->memory),
          prefix);

    OrtAllocator* allocator = nullptr;
    check(api, api.GetAllocatorWithDefaultOptions(&allocator), prefix);
    std::size_t inputs = 0;
    std::size_t outputs = 0;
    check(api, api.SessionGetInputCount(impl_->session, &inputs), prefix);
    check(api, api.SessionGetOutputCount(impl_->session, &outputs), prefix);
    if (inputs == 0 || outputs == 0) {
        throw EmbedderUnavailable(prefix + "the model has no inputs or no outputs");
    }
    impl_->input_name = impl_->name_at(allocator, 0, true, prefix);
    for (std::size_t i = 1; i < inputs; ++i) {
        std::string name = impl_->name_at(allocator, i, true, prefix);
        if (contains_len(name)) {
            impl_->length_name = std::move(name);
            break;
        }
    }
    // Without "embs", Python runs every output and reads the first.
    impl_->output_name = impl_->name_at(allocator, 0, false, prefix);
    for (std::size_t i = 0; i < outputs; ++i) {
        if (impl_->name_at(allocator, i, false, prefix) == "embs") {
            impl_->output_name = "embs";
            break;
        }
    }
    logger.info("Speaker embedder loaded: {} (input '{}')", path_to_utf8(model), impl_->input_name);
}

OrtEmbeddingModel::~OrtEmbeddingModel() = default;

std::unique_ptr<OrtEmbeddingModel> OrtEmbeddingModel::load(const std::filesystem::path& onnxruntime,
                                                           const std::filesystem::path& model) {
    const auto runtime = OrtRuntime::load(onnxruntime);
    if (!runtime) {
        throw EmbedderUnavailable("Failed to load speaker embedding model '" + path_to_utf8(model) +
                                  "': " + runtime.error());
    }
    return std::make_unique<OrtEmbeddingModel>(**runtime, model);
}

bool OrtEmbeddingModel::available() const {
    return impl_->session != nullptr;
}

const std::string& OrtEmbeddingModel::input_name() const {
    return impl_->input_name;
}

const std::string& OrtEmbeddingModel::length_name() const {
    return impl_->length_name;
}

const std::string& OrtEmbeddingModel::output_name() const {
    return impl_->output_name;
}

std::optional<std::vector<float>> OrtEmbeddingModel::embed(std::span<const float> fbank,
                                                           int frames) {
    static const std::string prefix = "Speaker embedding inference failed: ";
    const OrtApi& api = *impl_->api;
    if (impl_->session == nullptr) {
        throw EmbedderUnavailable("Speaker embedder has no loaded session");
    }
    const std::size_t expected = static_cast<std::size_t>(frames) * dsp::kFbankMelBins;
    if (frames <= 0 || fbank.size() != expected) {
        throw EmbedderUnavailable(prefix + "the features do not have " +
                                  std::to_string(dsp::kFbankMelBins) + " bins per frame");
    }

    struct Values {
        const OrtApi& api;
        std::vector<OrtValue*> values;
        ~Values() {
            for (OrtValue* value : values) {
                if (value != nullptr) {
                    api.ReleaseValue(value);
                }
            }
        }
    } inputs{api, {}};

    const std::array<std::int64_t, 3> shape{1, frames, dsp::kFbankMelBins};
    OrtValue* features = nullptr;
    // ONNX Runtime does not write to an input; the API takes the buffer as non-const.
    check(api,
          api.CreateTensorWithDataAsOrtValue(impl_->memory, const_cast<float*>(fbank.data()),
                                             fbank.size_bytes(), shape.data(), shape.size(),
                                             ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT, &features),
          prefix);
    inputs.values.push_back(features);
    std::vector<const char*> names{impl_->input_name.c_str()};
    std::array<std::int64_t, 1> length{frames};
    const std::array<std::int64_t, 1> length_shape{1};
    if (!impl_->length_name.empty()) {
        OrtValue* length_value = nullptr;
        check(api,
              api.CreateTensorWithDataAsOrtValue(
                  impl_->memory, length.data(), sizeof(length), length_shape.data(),
                  length_shape.size(), ONNX_TENSOR_ELEMENT_DATA_TYPE_INT64, &length_value),
              prefix);
        inputs.values.push_back(length_value);
        names.push_back(impl_->length_name.c_str());
    }

    Values outputs{api, {nullptr}};
    const char* output_name = impl_->output_name.c_str();
    check(api,
          api.Run(impl_->session, nullptr, names.data(), inputs.values.data(), names.size(),
                  &output_name, 1, outputs.values.data()),
          prefix);

    OrtTensorTypeAndShapeInfo* info = nullptr;
    check(api, api.GetTensorTypeAndShape(outputs.values[0], &info), prefix);
    ONNXTensorElementDataType type = ONNX_TENSOR_ELEMENT_DATA_TYPE_UNDEFINED;
    std::size_t count = 0;
    OrtStatus* type_status = api.GetTensorElementType(info, &type);
    OrtStatus* count_status =
        type_status == nullptr ? api.GetTensorShapeElementCount(info, &count) : nullptr;
    api.ReleaseTensorTypeAndShapeInfo(info);
    check(api, type_status, prefix);
    check(api, count_status, prefix);

    void* data = nullptr;
    check(api, api.GetTensorMutableData(outputs.values[0], &data), prefix);
    // np.asarray(output, dtype=np.float32), flattened.
    std::vector<float> embedding(count);
    if (type == ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT) {
        const auto* values = static_cast<const float*>(data);
        std::copy(values, values + count, embedding.begin());
    } else if (type == ONNX_TENSOR_ELEMENT_DATA_TYPE_DOUBLE) {
        const auto* values = static_cast<const double*>(data);
        std::transform(values, values + count, embedding.begin(),
                       [](double value) { return static_cast<float>(value); });
    } else {
        throw EmbedderUnavailable(prefix + "the model's output is not floating point");
    }
    return embedding;
}

SpeakerEmbedder::SpeakerEmbedder(std::unique_ptr<EmbeddingModel> model)
    : model_(std::move(model)) {}

bool SpeakerEmbedder::available() const {
    return model_ != nullptr && model_->available();
}

std::optional<std::vector<float>> SpeakerEmbedder::embed(std::span<const float> audio_16k) {
    if (model_ == nullptr) {
        throw EmbedderUnavailable("Speaker embedder has no loaded session");
    }
    if (audio_16k.size() < kMinSamples) {
        return std::nullopt;
    }
    dsp::Fbank features = dsp::compute_fbank(audio_16k);
    if (features.frames == 0) {
        return std::nullopt;
    }
    dsp::apply_cmn(features);
    std::optional<std::vector<float>> embedding = model_->embed(features.values, features.frames);
    if (!embedding) {
        return std::nullopt;
    }
    // np.linalg.norm of float32 values, then a float32 division by it.
    double sum = 0.0;
    for (const float value : *embedding) {
        sum += static_cast<double>(value) * static_cast<double>(value);
    }
    const auto norm = static_cast<float>(std::sqrt(sum));
    if (!std::isfinite(norm) || static_cast<double>(norm) < 1e-8) {
        return std::nullopt;
    }
    for (float& value : *embedding) {
        value /= norm;
    }
    return embedding;
}

} // namespace fv::diarize
