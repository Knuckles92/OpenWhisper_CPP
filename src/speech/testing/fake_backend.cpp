#include "speech/testing/fake_backend.h"

#include <thread>
#include <utility>
#include <variant>

namespace fv::speech::testing {

FakeEngine::FakeEngine(FakeScript script) : script_(std::move(script)) {}

BackendFactory FakeEngine::factory() {
    std::weak_ptr<FakeEngine> weak = weak_from_this();
    return [weak](const BackendConfig& config) -> std::unique_ptr<Backend> {
        auto engine = weak.lock();
        if (!engine) {
            return nullptr;
        }
        std::size_t number = 0;
        {
            std::lock_guard lock(engine->mutex_);
            number = ++engine->created_;
        }
        return std::make_unique<FakeBackend>(engine, config, number);
    };
}

void FakeEngine::set_script(FakeScript script) {
    std::lock_guard lock(mutex_);
    script_ = std::move(script);
}

FakeScript FakeEngine::script() const {
    std::lock_guard lock(mutex_);
    return script_;
}

std::vector<FakeCall> FakeEngine::calls() const {
    std::lock_guard lock(mutex_);
    return calls_;
}

std::vector<std::string> FakeEngine::ops() const {
    std::lock_guard lock(mutex_);
    std::vector<std::string> ops;
    for (const FakeCall& call : calls_) {
        ops.push_back(call.op);
    }
    return ops;
}

std::size_t FakeEngine::created() const {
    std::lock_guard lock(mutex_);
    return created_;
}

void FakeEngine::hold() {
    std::lock_guard lock(mutex_);
    held_ = true;
}

void FakeEngine::resume() {
    {
        std::lock_guard lock(mutex_);
        held_ = false;
    }
    changed_.notify_all();
}

bool FakeEngine::wait_entered(std::chrono::milliseconds timeout) {
    std::unique_lock lock(mutex_);
    return changed_.wait_for(lock, timeout, [this] { return waiting_ > 0; });
}

bool FakeEngine::wait_calls(std::size_t count, std::chrono::milliseconds timeout) {
    std::unique_lock lock(mutex_);
    return changed_.wait_for(lock, timeout, [&] { return calls_.size() >= count; });
}

std::size_t FakeEngine::record(FakeCall call) {
    std::size_t decode = 0;
    {
        std::lock_guard lock(mutex_);
        if (call.op == "decode") {
            decode = ++decodes_;
        }
        calls_.push_back(std::move(call));
    }
    changed_.notify_all();
    return decode;
}

void FakeEngine::gate() {
    std::unique_lock lock(mutex_);
    if (!held_) {
        return;
    }
    ++waiting_;
    changed_.notify_all();
    changed_.wait(lock, [this] { return !held_; });
    --waiting_;
}

FakeBackend::FakeBackend(std::shared_ptr<FakeEngine> engine, BackendConfig config,
                         std::size_t number)
    : engine_(std::move(engine)), config_(std::move(config)), number_(number),
      id_(engine_->script().id) {}

std::string_view FakeBackend::id() const {
    return id_;
}

Capabilities FakeBackend::capabilities() const {
    return engine_->script().capabilities;
}

const BackendConfig& FakeBackend::config() const {
    return config_;
}

Readiness FakeBackend::load(const CancelToken&) {
    const FakeScript script = engine_->script();
    FakeCall call;
    call.backend = number_;
    call.op = "load";
    call.model_key = config_.model_key;
    engine_->record(std::move(call));
    if (script.load_delay.count() > 0) {
        std::this_thread::sleep_for(script.load_delay);
    }
    if (script.load_error) {
        last_error_ = *script.load_error;
        throw EngineError(*script.load_error);
    }
    Readiness readiness = script.readiness;
    const std::string label = config_.model_label.empty() ? "Fake" : config_.model_label;
    if (script.unsupported_gpu && config_.requested_device == "auto" && config_.device != "cpu") {
        readiness = Readiness{ReadyState::Ready, label + " | cpu",
                              "Parakeet is using the CPU. " + *script.unsupported_gpu};
    } else if (readiness.state == ReadyState::Ready && readiness.device_info.empty()) {
        readiness.device_info = label + " | " + config_.device;
    }
    loaded_ = readiness.state == ReadyState::Ready;
    last_error_ = loaded_ ? std::string() : readiness.device_info;
    return readiness;
}

DecodeResult FakeBackend::decode(const DecodeJob& job) {
    FakeCall call;
    call.backend = number_;
    call.op = "decode";
    call.lane = job.lane;
    call.language = job.options.language;
    std::size_t samples = 0;
    int rate = 16000;
    if (const auto* clip = std::get_if<AudioClip>(&job.input)) {
        samples = clip->end - clip->begin;
        rate = clip->rate;
        call.samples = samples;
        call.offset_s = clip->offset_s;
    }
    Subscription cancel_link;
    if (const auto hook = engine_->script().on_cancel) {
        cancel_link = job.cancel.on_cancel(hook);
    }
    const std::size_t number = engine_->record(std::move(call));
    engine_->gate();
    const FakeScript script = engine_->script();
    if (script.decode_delay.count() > 0) {
        std::this_thread::sleep_for(script.decode_delay);
    }
    if (!loaded_) {
        throw EngineError(last_error_.empty() ? "Speech engine is not loaded" : last_error_);
    }
    if (script.decode_error || (script.fail_call && *script.fail_call == number)) {
        throw EngineError(script.decode_error.value_or("Fake decode failed"), script.decode_retry);
    }
    DecodeResult result;
    result.transcript.text = script.texts.empty()
                                 ? "w" + std::to_string(number)
                                 : script.texts[(number - 1) % script.texts.size()];
    const double duration = static_cast<double>(samples) / rate;
    if (!result.transcript.text.empty()) {
        result.transcript.segments.push_back(Segment{result.transcript.text, 0.0, duration});
    }
    result.transcript.audio_processed = duration;
    return result;
}

void FakeBackend::release() {
    FakeCall call;
    call.backend = number_;
    call.op = "release";
    engine_->record(std::move(call));
    loaded_ = false;
}

bool FakeBackend::warm_up(const CancelToken& superseded) {
    FakeCall call;
    call.backend = number_;
    call.op = "warm_up";
    engine_->record(std::move(call));
    engine_->gate();
    if (!loaded_ || superseded.cancelled()) {
        return false;
    }
    return engine_->script().warm_up_result;
}

} // namespace fv::speech::testing
