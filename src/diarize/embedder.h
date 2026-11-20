#pragma once

#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

// Speaker embeddings for diarization: a WeSpeaker-family speaker-verification model run on CPU
// through ONNX Runtime, behind an interface so that the meeting tests can run on fixtures. Every
// failure of the runtime or the model is an EmbedderUnavailable, the one error the diarizer
// degrades on.
namespace fv::diarize {

class OrtRuntime;

// Audio shorter than this gives no embedding.
inline constexpr double kMinAudioSeconds = 0.8;

class EmbedderUnavailable : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

// A speaker-embedding model over filterbank features.
// Affinity: embed on one thread at a time; available from any thread.
class EmbeddingModel {
public:
    virtual ~EmbeddingModel() = default;
    // The model's raw (not normalised) embedding of `frames` rows of dsp::kFbankMelBins
    // mean-normalised features, or nullopt when it gives none. Throws EmbedderUnavailable when
    // inference fails.
    virtual std::optional<std::vector<float>> embed(std::span<const float> fbank, int frames) = 0;
    // Whether the model is loaded and usable.
    virtual bool available() const { return true; }
};

// The model in an ONNX file, on ONNX Runtime's CPU execution provider with one intra-op and one
// inter-op thread. Its first input takes the [1, frames, 80] features; a later input whose name
// contains "len" (any case) takes the frame count as int64 [1]; the output named "embs" is read
// when there is one, else the first output.
class OrtEmbeddingModel final : public EmbeddingModel {
public:
    // Throws EmbedderUnavailable "Failed to load speaker embedding model '<path>': <reason>".
    OrtEmbeddingModel(OrtRuntime& runtime, const std::filesystem::path& model);
    ~OrtEmbeddingModel() override;
    OrtEmbeddingModel(const OrtEmbeddingModel&) = delete;
    OrtEmbeddingModel& operator=(const OrtEmbeddingModel&) = delete;

    // Loads the runtime at `onnxruntime` and the model; a runtime that cannot load is reported
    // the same way as a model that cannot.
    static std::unique_ptr<OrtEmbeddingModel> load(const std::filesystem::path& onnxruntime,
                                                   const std::filesystem::path& model);

    // Throws EmbedderUnavailable "Speaker embedding inference failed: <reason>".
    std::optional<std::vector<float>> embed(std::span<const float> fbank, int frames) override;
    bool available() const override;

    const std::string& input_name() const;
    // Empty when the model has no length input.
    const std::string& length_name() const;
    const std::string& output_name() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// L2-normalised speaker embeddings from 16 kHz mono audio: filterbank features, cepstral mean
// normalisation, the model, normalisation.
// Affinity: embed on one thread at a time; available from any thread.
class SpeakerEmbedder {
public:
    explicit SpeakerEmbedder(std::unique_ptr<EmbeddingModel> model);

    // True while the model is loaded and usable.
    bool available() const;
    // One embedding for a segment's audio (float, 16 kHz, in [-1, 1]), or nullopt when the audio
    // is shorter than kMinAudioSeconds, gives no features, or the embedding's norm is not finite
    // or below 1e-8. Throws EmbedderUnavailable when the model fails.
    std::optional<std::vector<float>> embed(std::span<const float> audio_16k);

    EmbeddingModel& model() { return *model_; }

private:
    std::unique_ptr<EmbeddingModel> model_;
};

} // namespace fv::diarize
