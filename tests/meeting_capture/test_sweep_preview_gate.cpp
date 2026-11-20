// MeetingAsrEngine::start_preview previews only on a backend that previews: meeting/asr/engine.py
// start_preview starts a WindowSpeechPreview for a Parakeet model only (local, or on a paired
// host: tests/test_meeting_remote.py::test_whisper_host_needs_no_optional_catalog_preview leaves
// `_preview` None for a Whisper host).

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <thread>
#include <vector>

#include "asr_test_support.h"
#include "meeting_capture/asr_engine.h"

using fv::Json;
using fv::meeting::CaptureBlock;
using fv::meeting::test::SpeechRig;

namespace {

using namespace std::chrono_literals;

// Feeds four seconds of speech-level audio and reports how many windows the backend decoded
// on the Preview lane within two seconds.
int preview_decodes(SpeechRig& speech) {
    auto repository = std::make_shared<fv::meeting::test::RecordingRepository>();
    fv::meeting::MeetingAsrEngine engine(speech.service, repository, "m_test", "auto",
                                         fv::system_clock());
    REQUIRE(engine.load_backend({}));
    std::atomic<int> events{0};
    speech.script->set_fallback({{{"spoken", 0.0, 0.5}}, std::nullopt});
    engine.start_preview([&](const Json&) { ++events; });
    const std::vector<std::int16_t> frames(64000, 3000);
    engine.feed_preview(CaptureBlock{"mic", frames.data(), frames.size(), 16000, 100.0}, 0.0);
    const auto deadline = std::chrono::steady_clock::now() + 2s;
    auto decoded = [&] {
        int count = 0;
        for (const auto& call : speech.script->calls()) {
            count += call.lane == fv::speech::Lane::Preview ? 1 : 0;
        }
        return count;
    };
    while (decoded() == 0 && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(5ms);
    }
    engine.stop_preview();
    engine.stop();
    return decoded();
}

} // namespace

TEST_CASE("a backend without live preview gets no meeting preview", "[asr_preview]") {
    SpeechRig speech;
    speech.script->set_live_preview(false);
    speech.select();
    CHECK(preview_decodes(speech) == 0);
}

TEST_CASE("a backend with live preview gets the meeting preview", "[asr_preview]") {
    SpeechRig speech;
    speech.select();
    CHECK(preview_decodes(speech) > 0);
}
