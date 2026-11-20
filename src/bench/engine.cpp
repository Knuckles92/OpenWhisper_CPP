#include "bench/engine.h"

#include <stdexcept>
#include <thread>

#include "base/utf8.h"
#include "speech/parakeet_backend.h"

namespace fv::bench {

EngineLookup find_engine_files(const std::optional<std::string>& runtime,
                               const std::optional<std::string>& model, const EnvReader& env,
                               std::string device) {
    const auto pick = [&](const std::optional<std::string>& given, std::string_view variable) {
        if (given && !given->empty()) {
            return given;
        }
        auto value = env.get(variable);
        return value && !value->empty() ? value : std::nullopt;
    };
    const auto runtime_dir = pick(runtime, kRuntimeVariable);
    if (!runtime_dir) {
        return {std::nullopt, "Set " + std::string(kRuntimeVariable) +
                                  " (or pass --runtime) to a NeMo-Speech.cpp runtime folder."};
    }
    const auto model_path = pick(model, kModelVariable);
    if (!model_path) {
        return {std::nullopt, "Set " + std::string(kModelVariable) +
                                  " (or pass --model) to a Parakeet .gguf model."};
    }
    return {
        EngineFiles{path_from_utf8(*runtime_dir), path_from_utf8(*model_path), std::move(device)},
        ""};
}

speech::BackendConfig parakeet_config(const EngineFiles& files) {
    speech::BackendConfig config;
    config.backend = "parakeet";
    config.runtime_dir = files.runtime_dir;
    config.model_path = files.model_path;
    config.model_key = "parakeet-v3";
    config.model_label = "Parakeet TDT 0.6B v3";
    config.requested_device = files.device;
    config.device = files.device;
    config.gpu_index = 0;
    return config;
}

BenchEngine::BenchEngine(speech::BackendFactory factory, speech::BackendConfig config,
                         speech::Slot slot, std::chrono::milliseconds timeout)
    : notify_("fv-bench-notify", 1),
      service_(std::make_unique<speech::SpeechService>(std::move(factory), notify_)) {
    const auto started = std::chrono::steady_clock::now();
    service_->select(slot, std::move(config));
    const auto deadline = started + timeout;
    for (;;) {
        readiness_ = service_->readiness(slot);
        if (readiness_.state != speech::ReadyState::Loading &&
            readiness_.state != speech::ReadyState::Warming) {
            break;
        }
        if (std::chrono::steady_clock::now() > deadline) {
            throw std::runtime_error("The speech engine did not load within " +
                                     std::to_string(timeout.count() / 1000) + " s.");
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    load_s_ = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
    if (readiness_.state != speech::ReadyState::Ready) {
        throw std::runtime_error(readiness_.message.empty() ? readiness_.device_info
                                                            : readiness_.message);
    }
}

BenchEngine::~BenchEngine() {
    // The service posts to the notify pool: it goes first.
    service_.reset();
}

std::unique_ptr<BenchEngine> load_parakeet(const EngineFiles& files, speech::Slot slot) {
    return std::make_unique<BenchEngine>(
        [](const speech::BackendConfig& config) {
            return std::make_unique<speech::ParakeetBackend>(config);
        },
        parakeet_config(files), slot);
}

} // namespace fv::bench
