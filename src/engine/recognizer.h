#pragma once

#include <filesystem>
#include <memory>
#include <span>
#include <string>

#include "engine/transcript.h"

namespace fv {

inline constexpr int kCpuDevice = -1;

struct RecognizerConfig {
    // An unpacked NeMo-Speech.cpp release (see find_runtime_library).
    std::filesystem::path runtime_dir;
    // The model's .gguf file.
    std::filesystem::path model_path;
    // The runtime's GPU index, or kCpuDevice. The CPU-only runtime has no GPU
    // to index.
    int gpu = kCpuDevice;
};

struct RecognitionOptions {
    // A language code such as "en"; empty or "auto" lets the model detect it.
    std::string language;
};

// A loaded Parakeet model. Loading takes seconds and holds the model in
// memory (or VRAM) until the object dies.
//
// Not thread-safe: use a recognizer from one thread at a time. The runtime
// decodes one request at a time anyway, and reports errors through a
// thread-local slot that only the calling thread can read.
//
// Parakeet TDT is an offline model: the runtime refuses to open a streaming
// recognition on it, so a live preview has to decode windows of the recording
// with transcribe().
class Recognizer {
public:
    // Loads the runtime and the model. Throws EngineError when either is
    // missing or the runtime rejects the model or device.
    explicit Recognizer(const RecognizerConfig& config);
    ~Recognizer();

    Recognizer(const Recognizer&) = delete;
    Recognizer& operator=(const Recognizer&) = delete;

    // Decodes a whole utterance of mono float samples in [-1, 1]. The runtime
    // resamples 8-96 kHz input to the model's rate.
    Transcript transcribe(std::span<const float> samples, int sample_rate,
                          const RecognitionOptions& options = {});

    // The runtime's own version string, such as "0.1.0".
    std::string runtime_version() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace fv
