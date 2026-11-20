#pragma once

#include <chrono>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>

#include "base/env.h"
#include "base/executor.h"
#include "speech/service.h"

// The speech engine a benchmark decodes with: the production SpeechService over a
// ParakeetBackend (or a test's backend), loaded once. The runtime and model come from the
// command line or the decode-test variables; nothing is ever downloaded.
namespace fv::bench {

inline constexpr std::string_view kRuntimeVariable = "OPENWHISPER_CPP_TEST_RUNTIME_DIR";
inline constexpr std::string_view kModelVariable = "OPENWHISPER_CPP_TEST_MODEL";

struct EngineFiles {
    std::filesystem::path runtime_dir;
    std::filesystem::path model_path;
    std::string device = "cpu";
};

// The runtime and model: each given path, else its variable. On a gap, the sentence to print:
// "Set OPENWHISPER_CPP_TEST_RUNTIME_DIR (or pass --runtime) to a NeMo-Speech.cpp runtime folder."
// or "Set OPENWHISPER_CPP_TEST_MODEL (or pass --model) to a Parakeet .gguf model."
struct EngineLookup {
    std::optional<EngineFiles> files;
    std::string missing;
};
EngineLookup find_engine_files(const std::optional<std::string>& runtime,
                               const std::optional<std::string>& model, const EnvReader& env,
                               std::string device = "cpu");

// The BackendConfig of a ParakeetBackend over `files`.
speech::BackendConfig parakeet_config(const EngineFiles& files);

// A SpeechService with one slot selected and loaded. Its notifications run on a thread of its
// own. Throws std::runtime_error with the readiness message when the engine does not become
// Ready within `timeout`.
class BenchEngine {
public:
    BenchEngine(speech::BackendFactory factory, speech::BackendConfig config,
                speech::Slot slot = speech::Slot::Dictation,
                std::chrono::milliseconds timeout = std::chrono::minutes(5));
    ~BenchEngine();
    BenchEngine(const BenchEngine&) = delete;
    BenchEngine& operator=(const BenchEngine&) = delete;

    speech::SpeechService& service() { return *service_; }
    // Seconds from select() to Ready: the first load.
    double load_s() const { return load_s_; }
    const speech::Readiness& readiness() const { return readiness_; }

private:
    ThreadPool notify_;
    std::unique_ptr<speech::SpeechService> service_;
    double load_s_ = 0.0;
    speech::Readiness readiness_;
};

// A BenchEngine over ParakeetBackend.
std::unique_ptr<BenchEngine> load_parakeet(const EngineFiles& files,
                                           speech::Slot slot = speech::Slot::Dictation);

} // namespace fv::bench
