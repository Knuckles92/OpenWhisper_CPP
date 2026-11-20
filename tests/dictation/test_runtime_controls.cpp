#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "dictation/messages.h"
#include "dictation/transcription_runtime.h"
#include "runtime_support.h"
#include "settings/keys.h"

// The runtime's controls besides the ported OpenWhisper tests: toggle, cancel of a job still
// working on its result and of a running decode, the cancel announcement, shutdown, the live
// preview's window, a stop that cannot take the slot or stop the recorder, and the exact
// strings of the saved-WAV checks (transcription.py 728-1062, 864-905).

using namespace fv::dictation;
using namespace fv::dictation::testing;

namespace {

std::string last_overlay(const RuntimeRig& rig) {
    const auto overlays = rig.trace.with_prefix("overlay:");
    return overlays.empty() ? std::string() : overlays.back();
}

std::size_t decodes(const RuntimeRig& rig) {
    const auto ops = rig.engine->ops();
    return static_cast<std::size_t>(std::count(ops.begin(), ops.end(), "decode"));
}

} // namespace

TEST_CASE("toggle starts a recording, then stops it and transcribes", "[dictation][controls]") {
    RuntimeRig rig;

    CHECK(rig.runtime->toggle_recording());
    CHECK(rig.recorder.is_recording());
    CHECK(rig.runtime->toggle_recording());
    CHECK_FALSE(rig.recorder.is_recording());
    CHECK(rig.jobs.queued() == 1);
    rig.run_workers();

    CHECK(rig.last_status() == "Ready (Pasted)");
    CHECK_FALSE(rig.runtime->has_active_job());
}

TEST_CASE("toggle reports a refused start", "[dictation][controls]") {
    RuntimeRig rig;
    REQUIRE(rig.runtime->claim_job());

    CHECK_FALSE(rig.runtime->toggle_recording());
    CHECK_FALSE(rig.recorder.is_recording());
}

TEST_CASE("a cancel announces the stopped recording as a cancel", "[dictation][controls]") {
    RuntimeRig rig;
    REQUIRE(rig.runtime->start_recording());
    std::vector<bool> canceling_seen;
    auto link = rig.runtime->recording_state_changed().connect(rig.ui, [&](bool on) {
        if (!on) {
            canceling_seen.push_back(rig.runtime->canceling_recording());
        }
    });

    CHECK(rig.runtime->cancel());

    CHECK(canceling_seen == std::vector<bool>{true});
    CHECK_FALSE(rig.runtime->canceling_recording());
    CHECK(rig.trace.has("recorder_cancel"));
    CHECK(rig.last_status() == "Recording canceled");
    CHECK(last_overlay(rig) == "canceling");
    CHECK(rig.runtime->current_job() == nullptr);
    CHECK_FALSE(rig.activity.snapshot().recording);
    // A second Cancel while the discarded stream still closes changes nothing.
    rig.recorder.recording = true;
    const auto statuses = rig.statuses().size();
    CHECK(rig.runtime->cancel());
    CHECK(rig.statuses().size() == statuses);
    CHECK(rig.events.of("recording.canceled").size() == 1);
}

TEST_CASE("a cancel with nothing to stop says nothing", "[dictation][controls]") {
    RuntimeRig rig;

    CHECK_FALSE(rig.runtime->cancelable());
    CHECK_FALSE(rig.runtime->cancel());

    CHECK(rig.statuses().empty());
    CHECK(rig.events.of("recording.canceled").empty());
}

TEST_CASE("a cancel reaches a job still working on its result once", "[dictation][controls]") {
    RuntimeRig rig;
    REQUIRE(rig.runtime->claim_job());
    CHECK(rig.runtime->cancelable());

    CHECK(rig.runtime->cancel());

    CHECK(rig.last_status() == "Canceled");
    CHECK(last_overlay(rig) == "canceling");
    CHECK(rig.runtime->job_slot().cancel_requested());
    CHECK_FALSE(rig.runtime->cancelable());
    const auto statuses = rig.statuses().size();
    CHECK(rig.runtime->cancel()); // the first Cancel is already stopping it
    CHECK(rig.statuses().size() == statuses);
    CHECK(rig.events.of("recording.canceled").size() == 1);
}

TEST_CASE("a job whose result was delivered is not canceled while it is saved",
          "[dictation][controls]") {
    RuntimeRig rig;
    REQUIRE(rig.runtime->claim_job());
    rig.runtime->on_transcription_complete("keep these words");
    REQUIRE(rig.runtime->has_active_job()); // the History save is queued

    CHECK_FALSE(rig.runtime->cancelable());
    CHECK_FALSE(rig.runtime->cancel());
    rig.run_workers();

    CHECK(rig.entries().size() == 1);
    CHECK(rig.last_status() == "Ready (Pasted)");
}

TEST_CASE("a cancel during the decode stops it and pastes nothing", "[dictation][controls]") {
    RuntimeRig rig;
    const auto wav = rig.wav("recorded.wav");
    REQUIRE(rig.runtime->claim_job());
    rig.engine->hold();

    std::thread worker([&] { rig.runtime->transcribe_audio_file(wav); });
    REQUIRE(rig.engine->wait_entered(std::chrono::seconds(5)));
    CHECK(rig.runtime->is_transcribing());
    CHECK(rig.runtime->cancel());
    CHECK(rig.trace.has("status:Transcription canceled"));
    rig.engine->resume();
    worker.join();
    rig.run_workers();

    CHECK_FALSE(rig.runtime->is_transcribing());
    CHECK(rig.trace.with_prefix("error_dialog:") ==
          std::vector<std::string>{"Error: Transcription canceled"});
    CHECK_FALSE(rig.trace.has("stage"));
    CHECK(rig.keys.sent().empty());
    CHECK_FALSE(rig.runtime->has_active_job());
    const auto failures = rig.events.of("transcription.failed");
    REQUIRE(failures.size() == 1);
    CHECK(failures[0].data["error"] == "canceled");
}

TEST_CASE("shutdown cancels the job a stop claimed", "[dictation][controls]") {
    RuntimeRig rig;
    REQUIRE(rig.runtime->start_recording());
    rig.runtime->stop_recording();
    REQUIRE(rig.jobs.queued() == 1);

    rig.runtime->begin_shutdown();
    rig.run_workers();

    CHECK_FALSE(rig.trace.has("saved"));
    CHECK(decodes(rig) == 0);
    CHECK(rig.keys.sent().empty());
    CHECK_FALSE(rig.runtime->has_active_job());
}

TEST_CASE("the live preview is published only while its recording runs", "[dictation][controls]") {
    RuntimeRig rig;
    REQUIRE(rig.runtime->start_recording());
    const auto job = rig.runtime->current_job();
    REQUIRE(job);
    DictationJob other;

    rig.runtime->publish_preview(job.get(), "hel", false);
    rig.runtime->publish_preview(&other, "not this one", false);
    rig.runtime->stop_recording();
    rig.runtime->publish_preview(job.get(), "hello", true);

    const auto previews = rig.events.of("transcription.preview");
    REQUIRE(previews.size() == 1);
    CHECK_FALSE(previews[0].replay);
    CHECK(previews[0].data == fv::Json{{"job_id", job->id}, {"text", "hel"}, {"final", false}});
}

TEST_CASE("a stop while another job holds the slot transcribes nothing", "[dictation][controls]") {
    RuntimeRig rig;
    REQUIRE(rig.runtime->start_recording());
    REQUIRE(rig.runtime->claim_job()); // an upload took the slot meanwhile

    rig.runtime->stop_recording();

    CHECK(rig.jobs.queued() == 0);
    CHECK(rig.last_status() ==
          "A transcription is already in progress — wait before processing this recording");
    CHECK(last_overlay(rig) == "none");
    CHECK(rig.events.of("recording.canceled").size() == 1);
}

TEST_CASE("a stop the recorder refuses ends the recording without a job", "[dictation][controls]") {
    RuntimeRig rig;
    REQUIRE(rig.runtime->start_recording());
    rig.recorder.stop_ok = false;

    rig.runtime->stop_recording();

    CHECK(rig.last_status() == "Failed to stop recording");
    CHECK(last_overlay(rig) == "none");
    CHECK(rig.jobs.queued() == 0);
    CHECK_FALSE(rig.runtime->has_active_job());
    CHECK(rig.runtime->current_job() == nullptr);
}

TEST_CASE("the saved WAV checks report their exact reasons", "[dictation][controls]") {
    const std::string fault =
        GENERATE("no data", "no data, capture error", "save failed", "save failed, capture error");
    RuntimeRig rig;
    if (fault.starts_with("no data")) {
        rig.recorder.has_data = false;
    } else {
        rig.recorder.save_ok = false;
    }
    if (fault.ends_with("capture error")) {
        rig.recorder.set_capture_error("The microphone stopped.");
    }
    REQUIRE(rig.runtime->claim_job());

    rig.runtime->finish_recording_job();
    rig.run_workers();

    const std::string expected = fault.ends_with("capture error") ? "Error: The microphone stopped."
                                 : fault == "no data"
                                     ? "Error: No audio data recorded"
                                     : "Error: Failed to save audio file; recovery copy kept";
    CHECK(rig.last_status() == expected);
    CHECK(rig.entries().empty()); // nothing was saved, so nothing is kept
    CHECK(decodes(rig) == 0);
    CHECK_FALSE(rig.runtime->has_active_job());
}

TEST_CASE("a saved WAV under 100 bytes is reported as empty or corrupted",
          "[dictation][controls]") {
    Trace tiny_trace;
    std::optional<FakeRecorder> tiny;
    RigOptions options;
    options.recorder = [&](const std::filesystem::path& root) -> RecorderPort& {
        std::ofstream(root / "tiny.wav", std::ios::binary)
            << std::string(MIN_RECORDING_BYTES - 1, 'x');
        tiny.emplace(root / "recorded_audio.wav", tiny_trace, root / "tiny.wav");
        return *tiny;
    };
    RuntimeRig rig(options);
    REQUIRE(rig.runtime->claim_job());

    rig.runtime->finish_recording_job();
    rig.run_workers();

    CHECK(rig.last_status() == "Error: Audio file is empty or corrupted");
    CHECK(rig.entries().empty());
    CHECK_FALSE(rig.runtime->has_active_job());
}

TEST_CASE("a model change between remote engines and the API reloads only for a local one",
          "[dictation][controls]") {
    RuntimeRig rig;
    std::vector<std::pair<std::string, std::optional<bool>>> device_info;
    auto link = rig.runtime->device_info_changed.connect(
        rig.ui, [&](std::string text, std::optional<bool> usable) {
            device_info.emplace_back(std::move(text), usable);
        });

    rig.runtime->on_model_changed("api"); // away from Parakeet: reload
    rig.runtime->on_model_changed("api"); // API to API: nothing to load
    CHECK(rig.trace.count("request_reload") == 1);
    rig.runtime->on_model_changed("remote");
    CHECK(rig.trace.count("request_reload") == 2);
    CHECK(rig.settings.get(fv::keys::selected_model) == "remote");
    CHECK(device_info.size() == 3);
    CHECK(device_info.front() == std::pair<std::string, std::optional<bool>>{"", std::nullopt});
}
