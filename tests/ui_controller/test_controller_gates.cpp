// change_audio_device's refusal (application_controller.py:969-985; A03-22): a new microphone
// order waits while a recording runs, and applies once it is over.
#include <catch2/catch_test_macros.hpp>

#include "controller_support.h"

using fvtest::ControllerRig;
using fvtest::waitUntil;

TEST_CASE("a new microphone order waits for the recording to finish", "[widget]") {
    ControllerRig rig;
    REQUIRE(rig.startEngine());
    REQUIRE(rig.app().start_recording("ui"));
    REQUIRE(waitUntil([&rig] { return rig.runtime().is_recording(); }));

    rig.app().change_audio_device();
    CHECK(waitUntil([&rig] {
        return rig.hasStatus("Wait for the recording to finish before changing device");
    }));
    CHECK_FALSE(rig.hasStatus("Audio device changed"));

    rig.app().cancel();
    REQUIRE(waitUntil(
        [&rig] { return !rig.runtime().is_recording() && !rig.runtime().has_active_job(); }));
    rig.app().change_audio_device();
    CHECK(waitUntil([&rig] { return rig.hasStatus("Audio device changed"); }));
}

// History's Re-transcribe through the window (A03-86): the dictation gates refuse it during a
// meeting, with the meeting's sentence, and start no job.
TEST_CASE("re-transcribe from the window is refused during a meeting", "[widget]") {
    ControllerRig rig;
    REQUIRE(rig.startEngine());
    rig.context().activity().set_meeting_active(true);
    Q_EMIT rig.window().retranscribeRequested(QStringLiteral("missing.wav"));
    CHECK(waitUntil([&rig] {
        return rig.hasStatus("Meeting Mode is active — end the meeting to use dictation");
    }));
    CHECK_FALSE(rig.runtime().has_active_job());
}
