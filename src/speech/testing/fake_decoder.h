#pragma once

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "speech/parakeet_backend.h"

namespace fv::speech::testing {

// The decoder side of ParakeetBackend without a runtime: records every load and decode, returns
// "w<n>" for decode n (or scripted texts) with one segment over the window, and can fail a GPU
// load, fail decodes, or hold a load or a decode at a gate until the test opens it.
// Affinity: thread-safe.
class FakeDecoders : public std::enable_shared_from_this<FakeDecoders> {
public:
    struct Script {
        std::optional<std::string> create_error;     // every load throws EngineError
        std::optional<std::string> gpu_create_error; // a load with a GPU index throws
        std::optional<std::string> transcribe_error; // every decode throws EngineError
        std::vector<std::string> texts;              // cycled; empty gives "w<n>"
        bool hold_loads = false;
        bool hold_decodes = false;
        // Runs inside each decode, before it returns (on the decoding thread).
        std::function<void()> on_transcribe;
    };

    struct Decode {
        std::vector<float> samples;
        std::string language;
    };

    // Two constructors, not a "= {}" default: Script's member initializers are not usable in a
    // default argument until this class is complete (GCC rejects it).
    FakeDecoders();
    explicit FakeDecoders(Script script);
    DecoderFactory factory();

    void set_script(Script script);
    std::vector<int> created_gpus() const; // the gpu of each load attempt, in order
    std::vector<Decode> decodes() const;
    std::size_t alive() const; // decoders not yet destroyed

    void resume();
    bool wait_entered(std::chrono::milliseconds timeout);

private:
    friend class FakeDecoder;
    void gate(bool Script::*which);

    mutable std::mutex mutex_;
    std::condition_variable changed_;
    Script script_;
    std::vector<int> gpus_;
    std::vector<Decode> decodes_;
    std::size_t alive_ = 0;
    std::size_t waiting_ = 0;
    std::size_t released_ = 0; // resume() calls
};

} // namespace fv::speech::testing
