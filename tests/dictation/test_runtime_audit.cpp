#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "automation_support.h"
#include "dictation/messages.h"
#include "dictation/recovery.h"
#include "dictation/transcription_runtime.h"
#include "runtime_support.h"
#include "walkthrough/library.h"
#include "walkthrough/storage.h"

// Behaviour of services/runtime/transcription.py that the a04 audit (docs/port/map/
// a04-runtime-services.md) found implemented but not pinned by another test: the order of the
// recording start gates, spoken routing of plain dictation, the start failure's default reason,
// the WAV check for a file that was never created, a cleanup profile's status, an action whose
// transform is gone, History keeping the words without the audio, the retranscribe refusals, the
// stop-to-result metric, startup recovery's walkthrough prune and a retranscription kept out of
// Stats.

using namespace fv::dictation;
using namespace fv::dictation::testing;
using fv::Json;

namespace {

const std::string kBusy =
    "A transcription is already in progress — wait before starting another recording";

// FakeRecorder whose save reports success and writes nothing, as a recorder whose WAV write
// vanished would (_saved_recording_size's "Audio file not created").
class NoFileRecorder final : public RecorderPort {
public:
    explicit NoFileRecorder(FakeRecorder& inner) : inner_(inner) {}

    bool is_recording() const override { return inner_.is_recording(); }
    bool recorder_busy() const override { return inner_.recorder_busy(); }
    bool capture_canceled() const override { return inner_.capture_canceled(); }
    bool stream_open() const override { return inner_.stream_open(); }
    bool start_recording() override { return inner_.start_recording(); }
    std::string last_start_error() const override { return inner_.last_start_error(); }
    bool stop_recording() override { return inner_.stop_recording(); }
    bool wait_for_stop_completion() override { return inner_.wait_for_stop_completion(); }
    bool has_recording_data() const override { return inner_.has_recording_data(); }
    bool save_recording() override { return true; }
    double get_recording_duration() const override { return inner_.get_recording_duration(); }
    std::string last_capture_error() const override { return inner_.last_capture_error(); }
    void fail_capture(const std::string& message) override { inner_.fail_capture(message); }
    void cancel_recording() override { inner_.cancel_recording(); }
    void acknowledge_recording() override { inner_.acknowledge_recording(); }
    const std::filesystem::path& output_file() const override { return inner_.output_file(); }
    fv::Signal<std::string>& capture_error() override { return inner_.capture_error(); }

private:
    FakeRecorder& inner_;
};

} // namespace

TEST_CASE("the recording start gates refuse in OpenWhisper's order", "[dictation][audit]") {
    // A04-02: backup, then the host update notice, then a meeting, then the engine's readiness,
    // then a busy slot (by the pointer), and an open capture last, silently.
    RuntimeRig rig;
    REQUIRE(rig.runtime->claim_job()); // the slot is busy throughout
    rig.activity.set_backup_in_progress(true);
    rig.activity.set_host_update_active(true);
    rig.activity.set_meeting_active(true);
    rig.set_readiness("The speech engine is loading");

    CHECK_FALSE(rig.runtime->start_recording());
    CHECK(rig.last_status() == "Wait for backup or restore preparation to finish");
    CHECK(rig.notices().empty());

    rig.activity.set_backup_in_progress(false);
    CHECK_FALSE(rig.runtime->start_recording());
    REQUIRE(rig.notices().size() == 1);
    CHECK(rig.notices().back() == busy_message("", true));

    rig.activity.set_host_update_active(false);
    CHECK_FALSE(rig.runtime->start_recording());
    CHECK(rig.last_status() == "Meeting Mode is active — end the meeting to use dictation");

    rig.activity.set_meeting_active(false);
    CHECK_FALSE(rig.runtime->start_recording());
    CHECK(rig.last_status() == "The speech engine is loading");

    rig.set_readiness(std::nullopt);
    CHECK_FALSE(rig.runtime->start_recording());
    CHECK(rig.notices().back() == kBusy);

    rig.runtime->finish_job();
    rig.recorder.recording = true;
    const auto statuses = rig.statuses().size();
    const auto notices = rig.notices().size();
    CHECK_FALSE(rig.runtime->start_recording());
    CHECK(rig.statuses().size() == statuses);
    CHECK(rig.notices().size() == notices);
    CHECK_FALSE(rig.trace.has("recorder_start"));
}

TEST_CASE("plain dictation is routed by voice only when routing listens to dictation",
          "[dictation][audit]") {
    // A04-06: _dictation_route gives a plain dictation route "auto"; a profile recording keeps
    // its format and is not routed.
    RigOptions options;
    options.settings["voice_routing_enabled"] = true;
    options.settings["voice_routing_on_dictation"] = true;
    RuntimeRig rig(options);

    REQUIRE(rig.runtime->start_recording());
    REQUIRE(rig.runtime->current_job());
    CHECK(rig.runtime->current_job()->route == "auto");
    rig.runtime->cancel();

    REQUIRE(rig.runtime->start_recording(StartOptions{.profile_id = "email"}));
    CHECK(rig.runtime->current_job()->route.empty());
    rig.runtime->cancel();

    rig.settings.update(Json{{"voice_routing_on_dictation", false}});
    REQUIRE(rig.runtime->start_recording());
    CHECK(rig.runtime->current_job()->route.empty());
    rig.runtime->cancel();
}

TEST_CASE("a start failure without a reason says the stream could not open", "[dictation][audit]") {
    // A04-09: _report_start_failure's default reason, and the overlay goes.
    RuntimeRig rig;
    rig.recorder.start_ok = false;
    rig.recorder.start_error.clear();

    CHECK_FALSE(rig.runtime->start_recording());

    CHECK(rig.last_status() == "Failed to start recording: Could not open the audio stream");
    CHECK(rig.trace.with_prefix("overlay:").back() == "none");
    CHECK(rig.trace.has("discard_prefetch"));
    CHECK_FALSE(rig.runtime->current_job());
}

TEST_CASE("a saved recording whose file is missing reports it was not created",
          "[dictation][audit]") {
    // A04-11: "Audio file not created", and nothing is transcribed or kept.
    Trace inner_trace;
    std::optional<FakeRecorder> inner;
    std::optional<NoFileRecorder> recorder;
    RigOptions options;
    options.recorder = [&](const std::filesystem::path& root) -> RecorderPort& {
        inner.emplace(root / "never_written.wav", inner_trace);
        recorder.emplace(*inner);
        return *recorder;
    };
    RuntimeRig rig(options);
    REQUIRE(rig.runtime->claim_job());

    rig.runtime->finish_recording_job();
    rig.run_workers();

    CHECK(rig.last_status() == "Error: Audio file not created");
    CHECK(rig.entries().empty());
    CHECK_FALSE(rig.runtime->has_active_job());
}

TEST_CASE("a cleanup profile's pass says it is formatting", "[dictation][audit]") {
    // A04-17: "Formatting · {profile}..." with the Cleaning overlay, where standard cleanup says
    // "Cleaning up...".
    FakeCleaner cleaner;
    RuntimeRig rig;
    rig.runtime->set_cleaner(&cleaner);

    REQUIRE(rig.runtime->start_recording(StartOptions{.profile_id = "email"}));
    rig.runtime->stop_recording();
    rig.run_workers();
    rig.runtime->set_cleaner(nullptr);

    CHECK(cleaner.calls().size() == 1);
    CHECK(rig.trace.has("status:Formatting · Email..."));
    CHECK_FALSE(rig.trace.has("status:Cleaning up..."));
    const auto formatting = rig.trace.index("status:Formatting · Email...");
    CHECK(rig.trace.index("overlay:cleaning") >= 0);
    CHECK(rig.trace.index("overlay:cleaning") < formatting);
}

TEST_CASE("an action whose transform is gone sends the cleaned words and says so",
          "[dictation][audit]") {
    // A04-21: _shape_action's notice when the action's transform no longer exists.
    AutomationRig rig; // no saved transforms
    rig.with_actions({action_json("a3", "Prompt Claude", "scripts", Json::object(),
                                  "transform:no-such-transform")});
    rig.live_recording();
    REQUIRE(rig.runtime->claim_job(make_job(JobMode::Action, "a3")));
    rig.runtime->transcribe_audio_file(rig.wav());
    rig.run_workers();

    const auto deliveries = rig.port.deliveries();
    REQUIRE(deliveries.size() == 1);
    CHECK(deliveries[0].delivery.text == "ASK CLAUDE WHAT TIME IT IS");
    CHECK(deliveries[0].delivery.notice ==
          "This action's transform no longer exists — sent your cleaned words");
    CHECK(rig.entries().size() == 1);
}

TEST_CASE("History that keeps the words but not the audio says so", "[dictation][audit]") {
    // A04-26: _persist_history's "Transcript saved, but audio could not be retained; recovery
    // copy kept", after the paste; the recording is not acknowledged.
    RuntimeRig rig;
    const auto recordings = rig.root.path() / "recordings";
    std::filesystem::remove_all(recordings);
    std::ofstream(recordings) << "not a folder";
    std::filesystem::copy_file(audio_fixture("tone-16k-mono.wav"), rig.recorder.output_file(),
                               std::filesystem::copy_options::overwrite_existing);
    REQUIRE(rig.runtime->claim_job());
    rig.runtime->set_pending({rig.recorder.output_file(), 3.7, 2048, "Quick Record", 1.0});

    rig.runtime->on_transcription_complete("Hello there.");
    rig.run_workers();

    CHECK(rig.keys.sent() == std::vector<std::string>{"paste"});
    CHECK(rig.last_status() ==
          "Transcript saved, but audio could not be retained; recovery copy kept");
    const auto entries = rig.entries();
    REQUIRE(entries.size() == 1);
    CHECK(entries[0].text == "Hello there.");
    CHECK(rig.recorder.acknowledged == 0);
    CHECK_FALSE(rig.runtime->has_active_job());
}

TEST_CASE("retranscribing is refused during a meeting and for a missing file",
          "[dictation][audit]") {
    // A04-30: the meeting refusal comes first; a missing file says so and clears the overlay.
    RuntimeRig rig;
    const auto path = rig.wav("saved.wav");
    rig.activity.set_meeting_active(true);

    rig.runtime->retranscribe_audio(path);
    CHECK(rig.last_status() == "Meeting Mode is active — end it before retranscribing");
    CHECK_FALSE(rig.runtime->has_active_job());

    rig.activity.set_meeting_active(false);
    rig.runtime->retranscribe_audio(rig.root.path() / "gone.wav");
    CHECK(rig.last_status() == "Error: Audio file not found");
    CHECK(rig.trace.with_prefix("overlay:").back() == "none");
    CHECK_FALSE(rig.runtime->has_active_job());
    CHECK(rig.jobs.queued() == 0);
}

TEST_CASE("a dictation records the time from stop to result", "[dictation][audit]") {
    // A04-45: stop_to_result_s, measured from the stop to the delivered result.
    MetricsLog metrics;
    RuntimeRig rig;

    REQUIRE(rig.runtime->start_recording());
    rig.runtime->stop_recording();
    CHECK_FALSE(metrics.last("stop_to_result_s"));
    rig.run_workers();

    const auto value = metrics.last("stop_to_result_s");
    REQUIRE(value);
    CHECK(*value >= 0.0);
}

TEST_CASE("startup recovery prunes walkthroughs past their retention", "[dictation][audit]") {
    // A04-113: recover_recordings also prunes the walkthrough library; an expired finished
    // walkthrough goes, and with nothing recovered there is no status.
    fv::testkit::TempDataRoot root;
    const auto captures = root.path() / "captures";
    const auto finished = captures / "2026-10-20_09-00-00";
    std::filesystem::create_directories(finished);
    std::ofstream(finished / std::string(fv::walkthrough::MANIFEST_NAME)) << "{}";
    const auto old = std::chrono::system_clock::now() - std::chrono::hours(24 * 10);
    std::filesystem::last_write_time(finished / std::string(fv::walkthrough::MANIFEST_NAME),
                                     std::chrono::clock_cast<std::chrono::file_clock>(old));
    fv::walkthrough::set_retention(captures, 1);
    std::filesystem::create_directories(root.path() / "recordings");

    const std::string status = recover_interrupted_recordings(root.path() / "recorded_audio.wav",
                                                              root.path() / "recordings", captures);

    CHECK(status.empty());
    CHECK_FALSE(std::filesystem::exists(finished));
}

TEST_CASE("a retranscribed file is saved in History but not counted in Stats",
          "[dictation][audit]") {
    // A04-114: the retranscription flag in stats_counts keeps a re-read file out of Stats.
    RuntimeRig rig;
    const auto path = rig.wav("saved.wav");

    rig.runtime->retranscribe_audio(path);
    rig.run_workers();

    const auto entries = rig.entries();
    REQUIRE(entries.size() == 1);
    CHECK(entries[0].text == "Hello there.");
    CHECK(rig.stats->rows().empty());
    CHECK_FALSE(rig.runtime->has_active_job());
}
