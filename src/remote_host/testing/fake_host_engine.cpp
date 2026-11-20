#include "remote_host/testing/fake_host_engine.h"

#include <stdexcept>
#include <utility>

namespace fv::remote_host::testing {

FakeHostEngine::FakeHostEngine(std::string family, std::string model, bool available)
    : family_(std::move(family)), model_(std::move(model)), available_(available) {}

std::vector<std::string> FakeHostEngine::identity() const {
    std::lock_guard lock(mutex_);
    return {"fake", family_, model_};
}

bool FakeHostEngine::accepts_phrases() const {
    std::lock_guard lock(mutex_);
    return accepts_phrases_;
}

Json FakeHostEngine::describe() const {
    std::lock_guard lock(mutex_);
    Json out = Json::object();
    out["family"] = family_;
    out["model"] = model_;
    out["label"] = "Fake Parakeet";
    out["device"] = "cuda";
    out["streaming"] = false;
    out["available"] = available_;
    out["status"] = available_ ? "ready" : "not loaded";
    return out;
}

Json FakeHostEngine::transcribe(std::span<const float> audio, std::optional<std::string> language,
                                std::span<const std::string> phrases) {
    std::lock_guard lock(mutex_);
    FakeEngineCall call;
    call.op = "transcribe";
    call.audio.assign(audio.begin(), audio.end());
    call.language = std::move(language);
    call.phrases.assign(phrases.begin(), phrases.end());
    calls_.push_back(std::move(call));
    if (failure_) {
        throw std::runtime_error(*failure_);
    }
    const std::string text = "heard " + std::to_string(audio.size());
    Json segment = Json::object();
    segment["text"] = text;
    segment["start"] = 0.0;
    segment["end"] = static_cast<double>(audio.size()) / 16000.0;
    Json out = Json::object();
    out["text"] = text;
    out["segments"] = Json::array({std::move(segment)});
    return out;
}

Json FakeHostEngine::stream(std::string_view session, std::span<const float> audio,
                            std::optional<std::string> language, bool finish) {
    std::lock_guard lock(mutex_);
    FakeEngineCall call;
    call.op = "stream";
    call.audio.assign(audio.begin(), audio.end());
    call.language = std::move(language);
    call.session = std::string(session);
    call.finish = finish;
    calls_.push_back(std::move(call));
    Json event = Json::object();
    event["text"] = "partial " + std::to_string(audio.size());
    event["final"] = finish;
    Json out = Json::object();
    out["events"] = Json::array({std::move(event)});
    return out;
}

void FakeHostEngine::cancel_stream(std::string_view session) {
    std::lock_guard lock(mutex_);
    canceled_.emplace_back(session);
}

void FakeHostEngine::set_family(std::string family) {
    std::lock_guard lock(mutex_);
    family_ = std::move(family);
}

void FakeHostEngine::set_model(std::string model) {
    std::lock_guard lock(mutex_);
    model_ = std::move(model);
}

void FakeHostEngine::set_available(bool available) {
    std::lock_guard lock(mutex_);
    available_ = available;
}

void FakeHostEngine::set_accepts_phrases(bool accepts) {
    std::lock_guard lock(mutex_);
    accepts_phrases_ = accepts;
}

void FakeHostEngine::set_failure(std::optional<std::string> message) {
    std::lock_guard lock(mutex_);
    failure_ = std::move(message);
}

std::vector<FakeEngineCall> FakeHostEngine::calls() const {
    std::lock_guard lock(mutex_);
    return calls_;
}

std::vector<std::string> FakeHostEngine::canceled() const {
    std::lock_guard lock(mutex_);
    return canceled_;
}

} // namespace fv::remote_host::testing
