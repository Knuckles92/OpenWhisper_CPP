#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "audio/testing/fake_backend.h"
#include "base/env.h"
#include "base/identity.h"
#include "base/paths.h"
#include "base/utf8.h"
#include "dictation/messages.h"
#include "dictation/recorder_port.h"
#include "dictation/recovery.h"
#include "dictation/transcription_runtime.h"
#include "recorder/journal.h"
#include "recorder/recorder.h"
#include "runtime_support.h"
#include "settings/keys.h"
#include "speech/file_windows.h"
#include "speech/parakeet_backend.h"

// A dictation end to end: the real Recorder on audio::FakeBackend (blocks from a WAV), the
// speech service, delivery and History; then the same through the real Parakeet engine.

using namespace fv::dictation;
using namespace fv::dictation::testing;
using Catch::Matchers::ContainsSubstring;

namespace {

// A RuntimeRig whose recorder is a real Recorder on a FakeBackend with the measured Windows
// device layout.
struct FlowRig {
    explicit FlowRig(RigOptions options = {})
        : backend(
              fv::audio::testing::FakeBackend::from_fixture(audio_fixture("devices-windows.json"))),
          rig(with_recorder(std::move(options))) {}
    ~FlowRig() {
        // The recorder lets go of its files before the data root is removed.
        if (recorder) {
            recorder->cleanup();
        }
        rig.reset();
        adapter.reset();
        recorder.reset();
    }

    RigOptions with_recorder(RigOptions options) {
        options.recorder = [this](const std::filesystem::path& root) -> RecorderPort& {
            paths.emplace(fv::Paths::resolve(fv::identity(), fv::system_env()));
            fv::RecorderOptions recorder_options;
            recorder_options.output_file = root / "recorded_audio.wav";
            recorder_options.timings.post_roll = std::chrono::milliseconds(100);
            recorder_options.timings.finalize_grace = std::chrono::milliseconds(400);
            recorder = std::make_unique<fv::Recorder>(*backend, *paths, recorder_options);
            adapter = std::make_unique<RecorderAdapter>(*recorder);
            return *adapter;
        };
        return options;
    }

    // The microphone hears the tone fixture, block by block.
    void speak() {
        const auto pcm = fv::audio::testing::read_pcm(audio_fixture("tone-44k1-mono.wav"));
        REQUIRE_FALSE(backend->streams().empty());
        backend->streams().back()->deliver_blocks(pcm);
        REQUIRE(recorder->sync_capture());
    }

    // Sessions the recovery journal still holds beside the recording.
    std::size_t journal_sessions() const {
        const auto folder = fv::recovery_directory(recorder->output_file());
        std::error_code error;
        if (!std::filesystem::exists(folder, error)) {
            return 0;
        }
        std::size_t count = 0;
        for (const auto& entry : std::filesystem::directory_iterator(folder)) {
            count += entry.is_directory() ? 1 : 0;
        }
        return count;
    }

    std::shared_ptr<fv::audio::testing::FakeBackend> backend;
    std::optional<fv::Paths> paths;
    std::unique_ptr<fv::Recorder> recorder;
    std::unique_ptr<RecorderAdapter> adapter;
    std::optional<RuntimeRig> rig;
};

} // namespace

TEST_CASE("a dictation records, transcribes, pastes, then saves History and lets the journal go",
          "[dictation][flow]") {
    FlowRig flow;
    RuntimeRig& rig = *flow.rig;

    REQUIRE(rig.runtime->start_recording());
    CHECK(rig.runtime->is_recording());
    flow.speak();
    rig.runtime->stop_recording();
    rig.run_workers();

    CHECK(rig.statuses() == std::vector<std::string>{"Recording...", "Processing...",
                                                     "Transcribing...", "Ready (Pasted)"});
    CHECK(rig.trace.with_prefix("overlay:") ==
          std::vector<std::string>{"recording", "processing", "transcribing", "none"});
    CHECK(rig.keys.sent() == std::vector<std::string>{"paste"});
    CHECK(rig.clipboard.inner.stages.at(0).text == "Hello there.");
    REQUIRE(rig.trace.has("history"));
    CHECK(rig.trace.index("paste") < rig.trace.index("history"));

    const auto entries = rig.entries();
    REQUIRE(entries.size() == 1);
    CHECK(entries[0].text == "Hello there.");
    CHECK(entries[0].source_name == "Quick Record");
    REQUIRE(entries[0].audio_file);
    CHECK(std::filesystem::exists(rig.history->recordings_folder() / *entries[0].audio_file));
    CHECK(entries[0].audio_duration > 0.5);
    // The WAV is acknowledged once History retained it: nothing is left to recover.
    CHECK(flow.journal_sessions() == 0);
    CHECK_FALSE(rig.runtime->has_active_job());
    CHECK_FALSE(rig.activity.snapshot().transcribing);
}

TEST_CASE("a failed decode keeps the recording in History for a retry", "[dictation][flow]") {
    RigOptions options;
    options.script.decode_error = "Parakeet could not decode the audio.";
    FlowRig flow(options);
    RuntimeRig& rig = *flow.rig;

    REQUIRE(rig.runtime->start_recording());
    flow.speak();
    rig.runtime->stop_recording();
    rig.run_workers();

    CHECK(rig.last_status() == "Error: Parakeet could not decode the audio. — recording saved in "
                               "History. Select Retry transcription.");
    CHECK(rig.notices() ==
          std::vector<std::string>{"Transcription failed. Retry the recording from History."});
    CHECK(rig.keys.sent().empty());
    const auto entries = rig.entries();
    REQUIRE(entries.size() == 1);
    CHECK(entries[0].entry_kind == std::string(fv::FAILED_RECORDING_KIND));
    REQUIRE(entries[0].audio_file);
    CHECK(flow.journal_sessions() == 0);
    CHECK_FALSE(rig.runtime->has_active_job());

    // Retry transcription from History completes that entry.
    rig.engine->set_script([] {
        fv::speech::FakeScript script;
        script.texts = {"Recovered words."};
        return script;
    }());
    rig.runtime->retranscribe_audio(rig.history->recordings_folder() / *entries[0].audio_file);
    rig.run_workers();
    const auto retried = rig.history->get_entry(entries[0].id);
    REQUIRE(retried);
    CHECK(retried->text == "Recovered words.");
    CHECK(rig.entries().size() == 1);
    CHECK(rig.last_status() == "Transcript ready to copy");
}

TEST_CASE("a cancel while recording throws the capture away", "[dictation][flow]") {
    FlowRig flow;
    RuntimeRig& rig = *flow.rig;
    MetricsLog metrics;

    REQUIRE(rig.runtime->start_recording());
    flow.speak();
    CHECK(rig.runtime->cancelable());
    CHECK(rig.runtime->cancel());

    CHECK(rig.last_status() == "Recording canceled");
    CHECK(rig.trace.with_prefix("overlay:").back() == "canceling");
    CHECK(flow.recorder->capture_canceled());
    CHECK(metrics.last("cancel_s"));
    // A stop now (a push-and-hold release) transcribes nothing.
    rig.runtime->stop_recording();
    CHECK(rig.jobs.queued() == 0);
    CHECK(rig.events.of("recording.canceled").size() == 1);
}

TEST_CASE("a lost microphone keeps its audio as a failed recording", "[dictation][flow]") {
    FlowRig flow;
    RuntimeRig& rig = *flow.rig;

    REQUIRE(rig.runtime->start_recording());
    flow.speak();
    // Every microphone is gone: the recorder reports its one failure.
    flow.backend->keep_devices(0);
    flow.backend->streams().back()->lose("unplugged");
    REQUIRE(wait_until([&] { return rig.runtime->has_active_job(); }, std::chrono::seconds(10)));
    rig.run_workers();

    CHECK(rig.trace.has("status:" + std::string(fv::NO_MICROPHONE_LEFT) +
                        " — preserving captured audio"));
    CHECK(rig.keys.sent().empty());
    const auto entries = rig.entries();
    REQUIRE(entries.size() == 1);
    CHECK(entries[0].entry_kind == std::string(fv::FAILED_RECORDING_KIND));
    CHECK_FALSE(rig.runtime->has_active_job());
}

TEST_CASE("the engine not being ready is said as the engine says it", "[dictation][flow]") {
    RigOptions options;
    options.script.readiness = {fv::speech::ReadyState::ModelMissing,
                                "Download Parakeet TDT 0.6B v3 in Downloads.",
                                "Download Parakeet TDT 0.6B v3 in Downloads."};
    RuntimeRig rig(options);
    const auto source = rig.wav("source.wav");
    REQUIRE(rig.runtime->claim_job());
    PendingAudio pending;
    pending.audio_path = source;
    rig.runtime->set_pending(pending);

    rig.runtime->finish_recording_job();
    rig.run_workers();

    CHECK(rig.trace.has("request_model_consent"));
    CHECK(rig.trace.has("status:Error: Download Parakeet TDT 0.6B v3 in Downloads. — recording "
                        "saved in History. Select Retry transcription."));
}

TEST_CASE("paste_text_now pastes with auto-paste off and is refused while busy",
          "[dictation][flow]") {
    RigOptions options;
    options.settings["auto_paste"] = false;
    RuntimeRig rig(options);

    CHECK(rig.runtime->paste_text_now("um hello"));
    CHECK(rig.keys.sent() == std::vector<std::string>{"paste"});
    CHECK_FALSE(rig.runtime->paste_text_now(""));

    REQUIRE(rig.runtime->claim_job());
    CHECK_FALSE(rig.runtime->paste_text_now("um hello"));
    CHECK(rig.notices().back() == "A transcription is already in progress — wait before pasting");
    // The job the text answers may still hold the slot.
    const auto job = std::make_shared<const DictationJob>();
    rig.runtime->finish_job();
    REQUIRE(rig.runtime->claim_job(job));
    CHECK(rig.runtime->paste_text_now("answer", job.get()));
}

TEST_CASE("a model change saves the engine and reloads it, but not during a meeting",
          "[dictation][flow]") {
    RuntimeRig rig;
    rig.activity.set_meeting_active(true);
    rig.runtime->on_model_changed("api");
    CHECK(rig.last_status() == "End the meeting before changing transcription models");
    CHECK_FALSE(rig.trace.has("request_reload"));

    rig.activity.set_meeting_active(false);
    rig.runtime->on_model_changed("api"); // away from the local engine
    CHECK(rig.settings.get(fv::keys::selected_model) == "api");
    CHECK(rig.trace.count("request_reload") == 1);
    rig.runtime->on_model_changed("whisper"); // no such engine
    CHECK(rig.settings.get(fv::keys::selected_model) == "api");
    CHECK(rig.trace.count("request_reload") == 1);
}

TEST_CASE("startup recovery turns an interrupted journal into a recording", "[dictation][flow]") {
    FlowRig flow;
    RuntimeRig& rig = *flow.rig;
    {
        // A recording whose app died: its journal holds audio and nobody acknowledged it.
        fv::RecordingJournal journal(flow.recorder->output_file(), 44100, 1, 2, {});
        const std::vector<std::int16_t> samples(44100, 1000);
        const auto bytes = std::as_bytes(std::span(samples));
        REQUIRE(journal.append(bytes));
        REQUIRE(journal.finish());
        REQUIRE(journal.close());
    }

    rig.runtime->recover_recordings();

    CHECK(rig.last_status() == "Recovered 1 interrupted recording(s) in Recordings");
    CHECK(recovery_status(0, 0).empty());
    CHECK(recovery_status(2, 1) ==
          "Recovered 1 walkthrough(s) — open Saved Walkthroughs to finish them");
}

TEST_CASE("the test recording re-transcribed through Parakeet is pasted as Parakeet decodes it",
          "[decode]") {
    const auto runtime_dir = fv::env_utf8("FIORIVOCE_TEST_RUNTIME_DIR");
    const auto model = fv::env_utf8("FIORIVOCE_TEST_MODEL");
    const auto wav = fv::env_utf8("FIORIVOCE_TEST_WAV");
    if (!runtime_dir || !model || !wav) {
        SKIP("Set FIORIVOCE_TEST_RUNTIME_DIR, FIORIVOCE_TEST_MODEL and FIORIVOCE_TEST_WAV.");
    }
    RigOptions options;
    options.config = fake_parakeet_config();
    options.config.runtime_dir = fv::base_utf8::path_from_utf8(*runtime_dir);
    options.config.model_path = fv::base_utf8::path_from_utf8(*model);
    options.factory = [](const fv::speech::BackendConfig& config) {
        return std::make_unique<fv::speech::ParakeetBackend>(config);
    };
    RuntimeRig rig(options);
    INFO(rig.speech->readiness(fv::speech::Slot::Dictation).device_info);
    REQUIRE(rig.speech->readiness(fv::speech::Slot::Dictation).state ==
            fv::speech::ReadyState::Ready);
    // A retranscription reads the owner's test recording where it is: nothing copies or
    // changes it (its History entry keeps no audio, since the file is not a recording of ours).
    const auto recording = fv::base_utf8::path_from_utf8(*wav);
    rig.runtime->retranscribe_audio(recording);
    rig.run_workers();

    const auto direct = fv::speech::decode_file(*rig.speech, recording, {});
    REQUIRE_FALSE(direct.transcript.text.empty());
    REQUIRE(rig.clipboard.inner.stages.size() == 1);
    CHECK(rig.clipboard.inner.stages[0].text == direct.transcript.text);
    CHECK(rig.keys.sent() == std::vector<std::string>{"paste"});
    const auto entries = rig.entries();
    REQUIRE(entries.size() == 1);
    CHECK(entries[0].text == direct.transcript.text);
    CHECK_FALSE(entries[0].audio_file);
    CHECK(entries[0].entry_kind == std::string(FILE_ENTRY_KIND));
    CHECK_THAT(entries[0].model, ContainsSubstring("parakeet (Parakeet TDT 0.6B v3 | "));
}

namespace {

// A recording that stops while the paired host is away (Remote selected, the slot offline).
RigOptions remote_offline() {
    RigOptions options;
    options.settings["selected_model"] = "remote";
    options.script.readiness = {fv::speech::ReadyState::Offline, "Couldn't reach devbox.",
                                "Couldn't reach devbox."};
    return options;
}

} // namespace

// tests/test_application_controller.py::test_recording_waits_briefly_for_the_remote_engine
// ("offline"): three reconnects, then the recording is kept in History.
TEST_CASE("a recording waits briefly for the remote engine, then keeps the audio",
          "[dictation][flow]") {
    FlowRig flow(remote_offline());
    RuntimeRig& rig = *flow.rig;
    int retries = 0;
    rig.runtime->set_remote_link([&retries] { ++retries; }, [] { return false; });
    std::vector<std::string> progress;
    auto link = rig.runtime->transcription_retry_progress.connect(
        rig.ui, [&progress](std::string text) { progress.push_back(text); });
    std::vector<std::string> failures;
    auto failed = rig.runtime->transcription_retry_failed.connect(
        rig.ui, [&failures](std::string text) { failures.push_back(text); });

    REQUIRE(rig.runtime->start_recording());
    flow.speak();
    rig.runtime->stop_recording();
    rig.run_workers();

    CHECK(retries == 3);
    CHECK(progress == std::vector<std::string>{"Retrying connection (1/3)...",
                                               "Retrying connection (2/3)...",
                                               "Retrying connection (3/3)..."});
    CHECK_THAT(rig.last_status(), Catch::Matchers::ContainsSubstring(
                                      "Remote engine unavailable after 3 reconnect attempts."));
    // The overlay's retry failure (shown whatever window is in front), not a plain notice.
    CHECK(failures == std::vector<std::string>{"Reconnect failed. Audio saved in History."});
    CHECK(rig.notices().empty());
    const auto entries = rig.entries();
    REQUIRE(entries.size() == 1);
    CHECK(entries[0].entry_kind == std::string(fv::FAILED_RECORDING_KIND));
    CHECK(rig.keys.sent().empty());
}

// The same, "reconnected": the host is back at the second reconnect, the recording is decoded
// there, and its words wait on the clipboard's copy button, since focus may have moved.
TEST_CASE("a recording decodes once the remote engine reconnects", "[dictation][flow]") {
    FlowRig flow(remote_offline());
    RuntimeRig& rig = *flow.rig;
    int retries = 0;
    rig.runtime->set_remote_link(
        [&] {
            if (++retries == 2) {
                fv::speech::FakeScript back;
                back.texts = {"Recovered words."};
                rig.engine->set_script(back);
                rig.load();
            }
        },
        [] { return false; });

    REQUIRE(rig.runtime->start_recording());
    flow.speak();
    rig.runtime->stop_recording();
    rig.run_workers();

    CHECK(retries == 2);
    CHECK(rig.keys.sent().empty());
    const auto entries = rig.entries();
    REQUIRE(entries.size() == 1);
    CHECK(entries[0].text == "Recovered words.");
    CHECK(rig.notices() == std::vector<std::string>{"Transcript recovered. Ready to copy. (done)"});
}

// The same, "canceled": the user cancels during the first reconnect; nothing is kept or decoded.
TEST_CASE("a recording waiting for the remote engine can be canceled", "[dictation][flow]") {
    FlowRig flow(remote_offline());
    RuntimeRig& rig = *flow.rig;
    int retries = 0;
    rig.runtime->set_remote_link(
        [&] {
            ++retries;
            (void)rig.runtime->cancel();
        },
        [] { return false; });
    std::vector<std::string> progress;
    auto link = rig.runtime->transcription_retry_progress.connect(
        rig.ui, [&progress](std::string text) { progress.push_back(text); });

    REQUIRE(rig.runtime->start_recording());
    flow.speak();
    rig.runtime->stop_recording();
    rig.run_workers();

    CHECK(retries == 1);
    CHECK(progress == std::vector<std::string>{"Retrying connection (1/3)..."});
    CHECK(rig.keys.sent().empty());
    CHECK(rig.entries().empty());
    CHECK(rig.notices().empty());
}

// The same, "timeout": a reconnect that never settles uses up the whole wait (12 s here, as
// REMOTE_RECOVERY_WAIT_S); one attempt, then the audio is kept in History.
TEST_CASE("a recording stops waiting for a remote reconnect that never settles",
          "[dictation][flow]") {
    FlowRig flow(remote_offline());
    RuntimeRig& rig = *flow.rig;
    int retries = 0;
    rig.runtime->set_remote_link([&retries] { ++retries; }, [] { return true; });
    std::vector<std::string> progress;
    auto link = rig.runtime->transcription_retry_progress.connect(
        rig.ui, [&progress](std::string text) { progress.push_back(text); });
    std::vector<std::string> failures;
    auto failed = rig.runtime->transcription_retry_failed.connect(
        rig.ui, [&failures](std::string text) { failures.push_back(text); });

    REQUIRE(rig.runtime->start_recording());
    flow.speak();
    rig.runtime->stop_recording();
    rig.run_workers();

    CHECK(retries == 1);
    CHECK(progress == std::vector<std::string>{"Retrying connection (1/3)..."});
    CHECK_THAT(rig.last_status(), ContainsSubstring("after 1 reconnect attempt."));
    // The overlay's retry failure (shown whatever window is in front), not a plain notice.
    CHECK(failures == std::vector<std::string>{"Reconnect failed. Audio saved in History."});
    CHECK(rig.notices().empty());
    const auto entries = rig.entries();
    REQUIRE(entries.size() == 1);
    CHECK(entries[0].entry_kind == std::string(fv::FAILED_RECORDING_KIND));
}

namespace {

// A paired host that refuses the first decode before starting it (it was reloading its engine):
// the link reports it offline, as RemoteSpeechBackend's keeper does when it drops the
// connection, until a reconnect brings it back.
class RefusingHost final : public fv::speech::Backend {
public:
    struct State {
        std::mutex mutex;
        std::function<void(fv::speech::Readiness)> report;
        int decodes = 0;
        void push(fv::speech::ReadyState state, std::string info) {
            std::function<void(fv::speech::Readiness)> to;
            {
                std::lock_guard lock(mutex);
                to = report;
            }
            if (to) {
                to({state, info, state == fv::speech::ReadyState::Ready ? "" : info});
            }
        }
    };
    explicit RefusingHost(std::shared_ptr<State> state) : state_(std::move(state)) {}
    std::string_view id() const override { return "remote"; }
    fv::speech::Capabilities capabilities() const override { return {}; }
    fv::speech::Readiness load(const fv::CancelToken&) override {
        return {fv::speech::ReadyState::Ready, "Parakeet TDT 0.6B v3 on devbox | cpu", ""};
    }
    fv::speech::DecodeResult decode(const fv::speech::DecodeJob&) override {
        int call = 0;
        {
            std::lock_guard lock(state_->mutex);
            call = ++state_->decodes;
        }
        if (call == 1) {
            state_->push(fv::speech::ReadyState::Offline, "devbox is loading its engine.");
            throw fv::EngineError("devbox is loading its engine.",
                                  fv::EngineError::Retry::Connection);
        }
        fv::speech::DecodeResult result;
        result.transcript.text = "Recovered words.";
        return result;
    }
    void release() override {}
    fv::Subscription watch_readiness(fv::Executor&,
                                     std::function<void(fv::speech::Readiness)> report) override {
        std::lock_guard lock(state_->mutex);
        state_->report = std::move(report);
        return {};
    }

private:
    std::shared_ptr<State> state_;
};

} // namespace

// tests/test_application_controller.py::test_recording_retries_a_decode_the_remote_host_refused_before_starting:
// one reconnect, then the recording is decoded again; its words wait on the copy button.
TEST_CASE("test_recording_retries_a_decode_the_remote_host_refused_before_starting",
          "[dictation][flow]") {
    auto host = std::make_shared<RefusingHost::State>();
    RigOptions options;
    options.settings["selected_model"] = "remote";
    options.config.backend = "remote";
    options.factory = [host](const fv::speech::BackendConfig&) {
        return std::make_unique<RefusingHost>(host);
    };
    FlowRig flow(std::move(options));
    RuntimeRig& rig = *flow.rig;
    REQUIRE(rig.speech->readiness(fv::speech::Slot::Dictation).state ==
            fv::speech::ReadyState::Ready);
    int retries = 0;
    rig.runtime->set_remote_link(
        [&] {
            ++retries;
            host->push(fv::speech::ReadyState::Ready, "Parakeet TDT 0.6B v3 on devbox | cpu");
        },
        [] { return false; });

    REQUIRE(rig.runtime->start_recording());
    flow.speak();
    rig.runtime->stop_recording();
    rig.run_workers();

    CHECK(retries == 1);
    CHECK(host->decodes == 2);
    const auto entries = rig.entries();
    REQUIRE(entries.size() == 1);
    CHECK(entries[0].text == "Recovered words.");
    CHECK(rig.keys.sent().empty());
}

// transcription.py _timed_decode: a Remote engine's final pass is timed from just before its
// decode, and the stats carry where the time went (the stats line's "on <host> · N ms network");
// a pass the host refused and that is sent again is timed afresh.
TEST_CASE("a remote pass's request timing reaches the transcription stats", "[dictation][flow]") {
    auto host = std::make_shared<RefusingHost::State>();
    RigOptions options;
    options.settings["selected_model"] = "remote";
    options.config.backend = "remote";
    options.factory = [host](const fv::speech::BackendConfig&) {
        return std::make_unique<RefusingHost>(host);
    };
    FlowRig flow(std::move(options));
    RuntimeRig& rig = *flow.rig;
    rig.runtime->set_remote_link(
        [&] { host->push(fv::speech::ReadyState::Ready, "Parakeet TDT 0.6B v3 on devbox | cpu"); },
        [] { return false; });
    int marks = 0;
    rig.runtime->set_remote_timing(
        [&]() -> std::function<std::optional<fv::remote::RemoteTiming>()> {
            const int mark = ++marks;
            const int decodes = host->decodes;
            return [&, mark, decodes]() -> std::optional<fv::remote::RemoteTiming> {
                // What the backend counted since the mark: this pass's requests only.
                fv::remote::RemoteTiming timing;
                timing.host = "devbox";
                timing.requests = host->decodes - decodes;
                timing.round_trip_s = 0.5 * mark;
                timing.host_s = 0.469 * mark;
                return timing;
            };
        });
    std::vector<TranscriptionStats> stats;
    auto link = rig.runtime->transcription_stats.connect(
        rig.ui, [&stats](TranscriptionStats reported) { stats.push_back(reported); });

    REQUIRE(rig.runtime->start_recording());
    flow.speak();
    rig.runtime->stop_recording();
    rig.run_workers();

    // The first decode was refused, so the pass was timed twice; the second timing counts.
    CHECK(marks == 2);
    REQUIRE(stats.size() == 1);
    REQUIRE(stats[0].remote.has_value());
    CHECK(stats[0].remote->host == "devbox");
    CHECK(stats[0].remote->requests == 1);
    CHECK(stats[0].remote->round_trip_s == 1.0);
    REQUIRE(stats[0].remote->network_s().has_value());
    CHECK(*stats[0].remote->network_s() > 0.06);
    CHECK(*stats[0].remote->network_s() < 0.07);
}

// A local engine's pass carries no remote timing, and the hook is not asked.
TEST_CASE("a local pass carries no remote timing", "[dictation][flow]") {
    FlowRig flow;
    RuntimeRig& rig = *flow.rig;
    int marks = 0;
    rig.runtime->set_remote_timing(
        [&marks]() -> std::function<std::optional<fv::remote::RemoteTiming>()> {
            ++marks;
            return {};
        });
    std::vector<TranscriptionStats> stats;
    auto link = rig.runtime->transcription_stats.connect(
        rig.ui, [&stats](TranscriptionStats reported) { stats.push_back(reported); });
    REQUIRE(rig.runtime->start_recording());
    flow.speak();
    rig.runtime->stop_recording();
    rig.run_workers();
    CHECK(marks == 0);
    REQUIRE(stats.size() == 1);
    CHECK_FALSE(stats[0].remote.has_value());
}
