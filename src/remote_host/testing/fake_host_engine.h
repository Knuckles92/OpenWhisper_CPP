#pragma once

#include <cstddef>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "remote_host/host_engine.h"

// The engine OpenWhisper's remote tests put behind a real host (tests/fakes/remote.py
// FakeEngine): it answers "heard <samples>" for any audio and records what it was asked.
namespace fv::remote_host::testing {

// One call the engine took.
struct FakeEngineCall {
    std::string op; // "transcribe" or "stream"
    std::vector<float> audio;
    std::optional<std::string> language;
    std::vector<std::string> phrases; // transcribe
    std::string session;              // stream
    bool finish = false;              // stream
};

// Affinity: thread-safe.
class FakeHostEngine final : public HostEngine {
public:
    explicit FakeHostEngine(std::string family = "parakeet", std::string model = "parakeet-v3",
                            bool available = true);

    // ("fake", family, model).
    std::vector<std::string> identity() const override;
    bool accepts_phrases() const override;
    // {family, model, label "Fake Parakeet", device "cuda", streaming false, available, status
    // "ready" or "not loaded"}.
    Json describe() const override;
    // {"text": "heard <n>", "segments": [{"text": "heard <n>", "start": 0.0, "end": n / 16000}]},
    // or std::runtime_error with the failure set.
    Json transcribe(std::span<const float> audio, std::optional<std::string> language,
                    std::span<const std::string> phrases) override;
    // {"events": [{"text": "partial <n>", "final": finish}]}.
    Json stream(std::string_view session, std::span<const float> audio,
                std::optional<std::string> language, bool finish) override;
    void cancel_stream(std::string_view session) override;

    // A switch of model or family changes the identity, as a host's engine change does.
    void set_family(std::string family);
    void set_model(std::string model);
    void set_available(bool available);
    void set_accepts_phrases(bool accepts);
    // Every later transcribe throws std::runtime_error(message); nullopt stops it.
    void set_failure(std::optional<std::string> message);

    std::vector<FakeEngineCall> calls() const;
    std::vector<std::string> canceled() const;

private:
    mutable std::mutex mutex_;
    std::string family_;
    std::string model_;
    bool available_;
    bool accepts_phrases_ = false;
    std::optional<std::string> failure_;
    std::vector<FakeEngineCall> calls_;
    std::vector<std::string> canceled_;
};

} // namespace fv::remote_host::testing
