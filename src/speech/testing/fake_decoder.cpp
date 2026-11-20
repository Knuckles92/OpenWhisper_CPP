#include "speech/testing/fake_decoder.h"

#include <utility>

#include "engine/error.h"

namespace fv::speech::testing {

class FakeDecoder final : public Decoder {
public:
    explicit FakeDecoder(std::shared_ptr<FakeDecoders> owner) : owner_(std::move(owner)) {}
    ~FakeDecoder() override {
        std::lock_guard lock(owner_->mutex_);
        --owner_->alive_;
    }
    FakeDecoder(const FakeDecoder&) = delete;
    FakeDecoder& operator=(const FakeDecoder&) = delete;

    Transcript transcribe(std::span<const float> samples,
                          const RecognitionOptions& options) override {
        std::size_t number = 0;
        FakeDecoders::Script script;
        {
            std::lock_guard lock(owner_->mutex_);
            owner_->decodes_.push_back(FakeDecoders::Decode{
                std::vector<float>(samples.begin(), samples.end()), options.language});
            number = owner_->decodes_.size();
            script = owner_->script_;
        }
        owner_->changed_.notify_all();
        if (script.on_transcribe) {
            script.on_transcribe();
        }
        owner_->gate(&FakeDecoders::Script::hold_decodes);
        if (script.transcribe_error) {
            throw EngineError(*script.transcribe_error);
        }
        Transcript transcript;
        transcript.text = script.texts.empty() ? "w" + std::to_string(number)
                                               : script.texts[(number - 1) % script.texts.size()];
        const double duration = static_cast<double>(samples.size()) / 16000.0;
        transcript.segments.push_back(Segment{transcript.text, 0.0, duration});
        transcript.audio_processed = duration;
        return transcript;
    }

private:
    std::shared_ptr<FakeDecoders> owner_;
};

FakeDecoders::FakeDecoders() : FakeDecoders(Script{}) {}
FakeDecoders::FakeDecoders(Script script) : script_(std::move(script)) {}

DecoderFactory FakeDecoders::factory() {
    std::weak_ptr<FakeDecoders> weak = weak_from_this();
    return [weak](const RecognizerConfig& config) -> std::unique_ptr<Decoder> {
        auto owner = weak.lock();
        if (!owner) {
            throw EngineError("The fake decoders are gone.");
        }
        Script script;
        {
            std::lock_guard lock(owner->mutex_);
            owner->gpus_.push_back(config.gpu);
            script = owner->script_;
        }
        owner->changed_.notify_all();
        owner->gate(&Script::hold_loads);
        if (script.create_error) {
            throw EngineError(*script.create_error);
        }
        if (script.gpu_create_error && config.gpu != kCpuDevice) {
            throw EngineError(*script.gpu_create_error);
        }
        {
            std::lock_guard lock(owner->mutex_);
            ++owner->alive_;
        }
        return std::make_unique<FakeDecoder>(owner);
    };
}

void FakeDecoders::set_script(Script script) {
    std::lock_guard lock(mutex_);
    script_ = std::move(script);
}

std::vector<int> FakeDecoders::created_gpus() const {
    std::lock_guard lock(mutex_);
    return gpus_;
}

std::vector<FakeDecoders::Decode> FakeDecoders::decodes() const {
    std::lock_guard lock(mutex_);
    return decodes_;
}

std::size_t FakeDecoders::alive() const {
    std::lock_guard lock(mutex_);
    return alive_;
}

void FakeDecoders::resume() {
    {
        std::lock_guard lock(mutex_);
        script_.hold_loads = false;
        script_.hold_decodes = false;
        ++released_;
    }
    changed_.notify_all();
}

bool FakeDecoders::wait_entered(std::chrono::milliseconds timeout) {
    std::unique_lock lock(mutex_);
    return changed_.wait_for(lock, timeout, [this] { return waiting_ > 0; });
}

void FakeDecoders::gate(bool Script::*which) {
    std::unique_lock lock(mutex_);
    if (!(script_.*which)) {
        return;
    }
    const std::size_t released = released_;
    ++waiting_;
    changed_.notify_all();
    changed_.wait(lock, [&] { return released_ != released; });
    --waiting_;
}

} // namespace fv::speech::testing
