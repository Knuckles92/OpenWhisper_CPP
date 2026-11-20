// Real devices. Tagged [interactive]: this opens the default microphone and records the default
// speaker's output, so only the owner runs it (ctest --preset windows-interactive).

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <memory>
#include <string>
#include <thread>

#include "audio/backend.h"
#include "capture_test_support.h"
#include "meeting_capture/capture_session.h"
#include "meeting_capture/session_audio.h"
#include "meeting_state/testing/fake_meeting_repository.h"

namespace {

class MemorySink final : public fv::meeting::CaptureStatusSink {
public:
    fv::Json capture_state() override { return state; }
    bool update_capture(const fv::Json& capture) override {
        state = capture;
        return true;
    }
    void emit_status(std::string) override {}
    void emit_error(std::string, std::string) override {}
    fv::Json state = fv::Json::object();
};

} // namespace

TEST_CASE("a 5 s meeting capture of the default microphone and loopback endpoint",
          "[interactive]") {
    fv::meeting::test::TempDir folder;
    fv::meeting::MeetingClock clock;
    clock.start();
    auto repository =
        std::make_shared<fv::meeting_state::testing::FakeMeetingRepository>(fv::system_clock());
    MemorySink sink;
    fv::meeting::MeetingCapture capture(
        {}, clock, repository, fv::audio::make_portaudio_backend(), sink, [] { return true; },
        [](const fv::meeting_state::SpooledChunk&) {}, nullptr);
    const auto note = capture.start("m_interactive", folder.path());
    INFO("note: " << note.value_or(""));
    std::this_thread::sleep_for(std::chrono::seconds(5));
    capture.stop();
    capture.flush();
    CHECK(capture.integrity_error() == std::nullopt);
    for (const char* channel : {"mic", "loopback"}) {
        const auto [frames, rate, origin] =
            fv::meeting::load_channel_session(folder.path(), channel);
        INFO(channel);
        REQUIRE(frames);
        CHECK(rate == 16000);
        // Five seconds, give or take the start-up of each stream.
        CHECK(frames->size() > 4 * 16000);
    }
}
