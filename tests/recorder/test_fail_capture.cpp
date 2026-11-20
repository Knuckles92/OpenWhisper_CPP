// Recorder::fail_capture is AudioRecorder._fail_capture as a caller reaches it: the transcription
// runtime ends a capture whose stop timed out (services/runtime/transcription.py).
#include <catch2/catch_test_macros.hpp>

#include <mutex>
#include <string>
#include <vector>

#include "recorder_test_support.h"

namespace {

using namespace fv::recorder_test;

} // namespace

TEST_CASE("a caller fails the capture once and later failures are ignored") {
    Rig rig;
    fv::RecorderOptions options;
    options.output_file = rig.file("dictation.wav");
    rig.make(std::move(options));

    std::mutex mutex;
    std::vector<std::string> errors;
    const fv::Subscription link =
        rig.rec().capture_error.connect(rig.inline_executor, [&](const std::string& message) {
            const std::lock_guard lock(mutex);
            errors.push_back(message);
        });

    REQUIRE(rig.rec().start_recording());
    rig.backend.stream(0).fake->deliver(tone(3));
    CHECK(rig.rec().last_capture_error().empty());

    rig.rec().fail_capture("Recording did not finish stopping.");
    rig.rec().fail_capture("a second failure");

    CHECK(rig.rec().last_capture_error() == "Recording did not finish stopping.");
    const std::lock_guard lock(mutex);
    CHECK(errors == std::vector<std::string>{"Recording did not finish stopping."});
}

TEST_CASE("failing a capture that was never started still records the one failure") {
    Rig rig;
    fv::RecorderOptions options;
    options.output_file = rig.file("dictation.wav");
    rig.make(std::move(options));

    rig.rec().fail_capture("Recording did not finish stopping.");

    CHECK(rig.rec().last_capture_error() == "Recording did not finish stopping.");
}
