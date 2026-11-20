// How a file reaches the speech engine: the engine loaded in the dictation slot decides between
// window decoding and the whole file (SpeechService::accepts_files), not the saved engine, and an
// engine switch that lands during the decode ends it with a sentence the user can read.
#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <memory>
#include <string>
#include <variant>
#include <vector>

#include "base/utf8.h"
#include "dictation/transcription_runtime.h"
#include "engine/error.h"
#include "uploads_support.h"

using namespace fv::dictation;
using namespace fv::dictation::testing;
using fv::Json;

namespace {

// Looks like the local engine (clips only) until its first decode, which finds a file-taking
// engine in its place: what a decode sees when an engine switch lands between the runtime's
// question and the decode.
class SwitchingBackend final : public fv::speech::Backend {
public:
    explicit SwitchingBackend(std::shared_ptr<std::atomic<bool>> switched)
        : switched_(std::move(switched)) {}

    std::string_view id() const override { return "parakeet"; }
    fv::speech::Capabilities capabilities() const override {
        fv::speech::Capabilities capabilities;
        capabilities.accepts_files = switched_->load();
        return capabilities;
    }
    fv::speech::Readiness load(const fv::CancelToken&) override {
        return {fv::speech::ReadyState::Ready, "Parakeet TDT 0.6B v3 | cpu", ""};
    }
    fv::speech::DecodeResult decode(const fv::speech::DecodeJob& job) override {
        if (std::holds_alternative<fv::speech::AudioClip>(job.input)) {
            switched_->store(true);
            throw std::invalid_argument("The api speech backend uploads audio files only");
        }
        fv::speech::DecodeResult result;
        result.transcript.text = "file";
        return result;
    }
    void release() override {}

private:
    std::shared_ptr<std::atomic<bool>> switched_;
};

} // namespace

TEST_CASE("a file goes to the engine loaded in the slot, not to the saved engine", "[uploads]") {
    UploadRig u;
    auto& rig = u.rig;

    // The API engine is saved, but its reload has not happened: the local engine still holds
    // the slot and decodes the file window by window.
    rig.settings.update(Json{{"selected_model", "api"}});
    const auto first = rig.wav("first.wav");
    REQUIRE(rig.runtime->upload_audio_file(first, 10.0));
    rig.run_workers();
    CHECK(u.transcripts().back() == "Hello there.");
    CHECK(u.engines.api->paths().empty());

    // Parakeet is saved again while the API engine is still loaded: it takes the whole file.
    rig.load(api_config());
    rig.settings.update(Json{{"selected_model", "parakeet"}});
    const auto second = rig.wav("second.wav");
    REQUIRE(rig.runtime->upload_audio_file(second, 10.0));
    rig.run_workers();
    CHECK(u.transcripts().back() == "api:" + fv::base_utf8::path_to_utf8(second));
    CHECK(u.engines.api->paths() == std::vector<std::string>{fv::base_utf8::path_to_utf8(second)});
}

TEST_CASE("an engine switch during a file's decode fails it with a sentence", "[uploads]") {
    auto switched = std::make_shared<std::atomic<bool>>(false);
    RigOptions options;
    options.factory = [switched](const fv::speech::BackendConfig&) {
        return std::make_unique<SwitchingBackend>(switched);
    };
    RuntimeRig rig(options);
    const auto audio = rig.wav("upload.wav");

    REQUIRE(rig.runtime->upload_audio_file(audio, 10.0));
    rig.run_workers();

    const auto errors = rig.trace.with_prefix("error_dialog:");
    REQUIRE(errors.size() == 1);
    CHECK(errors[0].find(
              "The speech engine changed while this audio was being transcribed. Try again.") !=
          std::string::npos);
    CHECK(errors[0].find("uploads audio files only") == std::string::npos);
}
