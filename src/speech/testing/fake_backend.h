#pragma once

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "speech/service.h"

namespace fv::speech::testing {

// What every FakeBackend of one FakeEngine does.
struct FakeScript {
    std::string id = "parakeet";
    Capabilities capabilities;
    // What load() returns. A non-empty device_info is used as given; empty becomes
    // "<model_label or Fake> | <device>".
    Readiness readiness{ReadyState::Ready, "", ""};
    std::chrono::milliseconds load_delay{0};
    std::optional<std::string> load_error; // load() throws EngineError with it
    // An Auto device whose GPU the runtime cannot use: load() falls back to the CPU and reports
    // Ready with the note "Parakeet is using the CPU. <reason>", as ParakeetBackend does.
    std::optional<std::string> unsupported_gpu;
    // The texts decode() returns, call after call (cycling); empty gives "w<n>" for call n.
    std::vector<std::string> texts;
    std::chrono::milliseconds decode_delay{0};
    std::optional<std::string> decode_error; // every decode() throws EngineError with it
    std::optional<std::size_t> fail_call;    // only this decode call (from 1) throws
    // What those failures say about retrying (a remote decode the host never started is
    // Connection).
    EngineError::Retry decode_retry = EngineError::Retry::None;
    bool warm_up_result = true;
    // Subscribed to each decode's cancel token while it runs, as a backend that aborts an
    // upload on cancel would; it runs on the cancelling thread.
    std::function<void()> on_cancel;
};

// One call a FakeBackend took.
struct FakeCall {
    std::size_t backend = 0; // which backend of the engine (from 1, in creation order)
    std::string op;          // "load", "decode", "warm_up", "release"
    Lane lane = Lane::Final;
    std::size_t samples = 0; // decode: the clip's length
    double offset_s = 0.0;   // decode: the clip's offset
    std::string language;    // decode: the job's language
    std::string model_key;   // load: the config's model
};

// The shared state behind a factory of FakeBackends: the script, the record of calls, and a
// gate that holds decodes (and warm-ups) until the test opens it.
// Affinity: thread-safe.
class FakeEngine : public std::enable_shared_from_this<FakeEngine> {
public:
    explicit FakeEngine(FakeScript script = {});

    BackendFactory factory();

    void set_script(FakeScript script);
    FakeScript script() const;
    std::vector<FakeCall> calls() const;
    std::vector<std::string> ops() const; // calls()' op names, in order
    std::size_t created() const;

    // While held, decode() and warm_up() wait at the gate until resume().
    void hold();
    void resume();
    // Waits until a call is waiting at the gate; false when the time ran out.
    bool wait_entered(std::chrono::milliseconds timeout);
    // Waits until `count` calls were recorded; false when the time ran out.
    bool wait_calls(std::size_t count, std::chrono::milliseconds timeout);

private:
    friend class FakeBackend;
    std::size_t record(FakeCall call);
    void gate();

    mutable std::mutex mutex_;
    std::condition_variable changed_;
    FakeScript script_;
    std::vector<FakeCall> calls_;
    std::size_t created_ = 0;
    std::size_t decodes_ = 0;
    bool held_ = false;
    std::size_t waiting_ = 0;
};

// speech::Backend on a script: scripted readiness, texts per window, delays, failures and the
// Auto-device CPU fallback.
// Affinity: the backend's lane thread, as any Backend.
class FakeBackend final : public Backend {
public:
    FakeBackend(std::shared_ptr<FakeEngine> engine, BackendConfig config, std::size_t number);

    std::string_view id() const override;
    Capabilities capabilities() const override;
    Readiness load(const CancelToken&) override;
    DecodeResult decode(const DecodeJob&) override;
    void release() override;
    bool warm_up(const CancelToken& superseded) override;

    const BackendConfig& config() const;

private:
    std::shared_ptr<FakeEngine> engine_;
    BackendConfig config_;
    std::size_t number_;
    std::string id_;
    bool loaded_ = false;
    std::string last_error_; // what decode() says while nothing is loaded
};

} // namespace fv::speech::testing

// The names the M1 spec gives the seam (speech::FakeBackend).
namespace fv::speech {
using testing::FakeBackend;
using testing::FakeCall;
using testing::FakeEngine;
using testing::FakeScript;
} // namespace fv::speech
