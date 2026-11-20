#include "remote_host/host_engine.h"

#include <stdexcept>
#include <utility>

#include "models/speech_catalog.h"
#include "settings/keys.h"

namespace fv::remote_host {

Json HostEngine::stream(std::string_view, std::span<const float>, std::optional<std::string>,
                        bool) {
    throw std::runtime_error("The host's engine has no live stream");
}

namespace {

class UnavailableEngine final : public HostEngine {
public:
    explicit UnavailableEngine(std::string reason) : reason_(std::move(reason)) {}

    std::vector<std::string> identity() const override { return {"unavailable", reason_}; }

    Json describe() const override {
        Json out = Json::object();
        out["family"] = "";
        out["model"] = "";
        out["label"] = "";
        out["device"] = "";
        out["streaming"] = false;
        out["available"] = false;
        out["status"] = reason_;
        return out;
    }

    Json transcribe(std::span<const float>, std::optional<std::string>,
                    std::span<const std::string>) override {
        throw std::runtime_error(reason_);
    }

private:
    std::string reason_;
};

class SpeechServiceEngine final : public HostEngine {
public:
    explicit SpeechServiceEngine(SpeechServiceEngineDeps deps) : deps_(std::move(deps)) {}

    std::vector<std::string> identity() const override {
        // Read for every request: only the two settings an identity holds.
        const Json saved = deps_.settings.load_settings(
            {keys::local_asr_devices.key(), keys::local_asr_language.key()});
        const std::string family = deps_.family();
        return {"worker",
                family,
                deps_.model(),
                deps_.device(),
                models::selected_device(saved, family),
                keys::local_asr_language.read(saved)};
    }

    Json describe() const override {
        const std::string model = deps_.model();
        const models::SpeechModel* entry = models::model(model);
        Json out = Json::object();
        out["family"] = deps_.family();
        out["model"] = model;
        out["label"] = deps_.label();
        out["device"] = deps_.device();
        out["streaming"] = entry != nullptr && entry->streaming;
        out["available"] = deps_.available();
        out["status"] = deps_.device_info();
        return out;
    }

    // Parakeet's dictionary support is "after" (text replacements once decoded), so phrases
    // never reach the speech model and transcribe drops them.
    bool accepts_phrases() const override { return false; }

    Json transcribe(std::span<const float> audio, std::optional<std::string> language,
                    std::span<const std::string>) override {
        speech::DecodeJob job;
        auto samples = std::make_shared<const std::vector<float>>(audio.begin(), audio.end());
        speech::AudioClip clip;
        clip.samples = samples;
        clip.begin = 0;
        clip.end = samples->size();
        job.input = std::move(clip);
        // Empty: the engine's own language setting, as `language or request_language()`.
        job.options.language = language.value_or("");
        job.lane = speech::Lane::RemoteClient;
        job.slot = speech::Slot::Dictation;
        const speech::DecodeResult result = deps_.speech.run(std::move(job));
        Json segments = Json::array();
        for (const auto& segment : result.transcript.segments) {
            Json item = Json::object();
            item["text"] = segment.text;
            item["start"] = segment.start;
            item["end"] = segment.end;
            segments.push_back(std::move(item));
        }
        Json out = Json::object();
        out["text"] = result.transcript.text;
        out["segments"] = std::move(segments);
        return out;
    }

private:
    SpeechServiceEngineDeps deps_;
};

} // namespace

std::shared_ptr<HostEngine> unavailable_engine(std::string reason) {
    return std::make_shared<UnavailableEngine>(std::move(reason));
}

std::shared_ptr<HostEngine> speech_service_engine(SpeechServiceEngineDeps deps) {
    return std::make_shared<SpeechServiceEngine>(std::move(deps));
}

std::shared_ptr<HostEngine> host_engine_for(std::string_view backend,
                                            SpeechServiceEngineDeps deps) {
    if (backend.empty()) {
        return unavailable_engine("No transcription engine is selected on the host");
    }
    if (backend == "remote") {
        return unavailable_engine(
            "The host is itself using a remote engine. Select a local engine there.");
    }
    // Every local backend is a speech-service engine, as every one of OpenWhisper's optional
    // engines is a LocalSpeechBackend (models::backends(): Parakeet only in this build).
    if (models::backend(backend) != nullptr) {
        return speech_service_engine(std::move(deps));
    }
    return unavailable_engine("The host's selected engine runs in the cloud and can't be shared. "
                              "Select a local engine there.");
}

} // namespace fv::remote_host
