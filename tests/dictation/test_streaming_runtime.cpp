#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <thread>
#include <vector>

#include "appcore/engine_reload.h"
#include "base/clock.h"
#include "base/executor.h"
#include "dictation/messages.h"
#include "dictation/streaming_runtime.h"
#include "dictation/transcription_runtime.h"
#include "preview_support.h"
#include "speech/dictation_language.h"
#include "speech/preview_status.h"
#include "speech/service.h"
#include "speech/window_preview.h"

// The live preview of a dictation (services/runtime/streaming.py StreamingRuntime) and the
// transcription runtime's use of it: tests/test_application_controller.py,
// tests/test_preview_lazy_tail.py and tests/test_streaming_transcriber.py.

using namespace fv::dictation;
using namespace fv::dictation::testing;
using fv::speech::Lane;
using namespace std::chrono_literals;

namespace {

constexpr int kWindowBlocks = 130; // whole blocks that reach 3.0 s at 44.1 kHz
constexpr std::string_view kFallbackStatus =
    "No speech detected — showing the live preview (not saved)";

Pcm blocks(int count) {
    return tone(static_cast<double>(count) * kBlock / kRate);
}

// The engine's text: preview windows "p<n>", final decodes `finals` in turn (then "").
void script(LaneEngine& engine, std::vector<std::string> finals,
            std::vector<std::string> previews = {}) {
    std::lock_guard lock(engine.mutex);
    engine.text = [finals = std::move(finals), previews = std::move(previews)](
                      Lane lane, std::size_t call, std::span<const float>) -> std::string {
        if (lane == Lane::Preview) {
            return call <= previews.size() ? previews[call - 1] : "p" + std::to_string(call);
        }
        if (lane == Lane::Final) {
            return call <= finals.size() ? finals[call - 1] : std::string();
        }
        return "";
    };
}

// Answers the reload machine's commands with the speech service, as the controller does.
struct ReloadDriver {
    explicit ReloadDriver(PreviewRig& rig, bool loads = true) : p(rig), load_now(loads) {
        links.push_back(machine.start_load.connect(p.rig.ui, [this] {
            load_requested = true;
            if (this->load_now) {
                finish_load();
            }
        }));
        links.push_back(
            machine.start_warmup.connect(p.rig.ui, [this] { machine.on_warmup_finished(); }));
        links.push_back(machine.streaming_setup_due.connect(p.rig.ui, [this] { ++setups_due; }));
        machine.select("parakeet", "parakeet-v3");
        p.streaming->attach(machine);
    }
    void finish_load() {
        p.rig.load();
        machine.on_load_finished(p.rig.speech->readiness(fv::speech::Slot::Dictation));
    }

    PreviewRig& p;
    bool load_now;
    fv::appcore::EngineReloadMachine machine{p.preview_ui};
    bool load_requested = false;
    int setups_due = 0;
    std::vector<fv::Subscription> links;
};

// A FakeClock's time that counts the reads made after the test declared the clock gone.
// Affinity: thread-safe.
class WatchedClock final : public fv::Clock {
public:
    explicit WatchedClock(fv::FakeClock& inner) : inner_(inner) {}
    std::chrono::steady_clock::time_point steady() const override {
        note();
        return inner_.steady();
    }
    std::chrono::system_clock::time_point utc() const override {
        note();
        return inner_.utc();
    }
    const std::chrono::time_zone* zone() const override {
        note();
        return inner_.zone();
    }

    std::atomic<bool> gone{false};
    mutable std::atomic<int> late_reads{0};

private:
    void note() const {
        if (gone.load()) {
            ++late_reads;
        }
    }
    fv::FakeClock& inner_;
};

// A UI executor whose posts from `gated` (the speech thread) wait at a gate until open(), so the
// speech thread can be held half-way through delivering a window's text. Posts from any other
// thread (the window's preparer, the test's) pass.
// Affinity: thread-safe.
class GatedExecutor final : public fv::Executor {
public:
    GatedExecutor(fv::ManualExecutor& inner, std::thread::id gated)
        : inner_(inner), gated_(gated) {}
    void post(fv::Task task) override {
        if (std::this_thread::get_id() == gated_) {
            std::unique_lock lock(mutex_);
            ++entered_;
            changed_.notify_all();
            changed_.wait(lock, [this] { return open_; });
        }
        inner_.post(std::move(task));
    }
    fv::TimerHandle post_after(std::chrono::milliseconds delay, fv::Task task) override {
        return inner_.post_after(delay, std::move(task));
    }
    bool is_current() const override { return inner_.is_current(); }

    bool wait_entered() {
        std::unique_lock lock(mutex_);
        return changed_.wait_for(lock, 10s, [this] { return entered_ > 0; });
    }
    void open() {
        {
            std::lock_guard lock(mutex_);
            open_ = true;
        }
        changed_.notify_all();
    }
    bool opened() {
        std::lock_guard lock(mutex_);
        return open_;
    }

private:
    fv::ManualExecutor& inner_;
    const std::thread::id gated_;
    std::mutex mutex_;
    std::condition_variable changed_;
    int entered_ = 0;
    bool open_ = false;
};

// One second of silence on the Final lane: the speech thread runs one job at a time and
// delivers each result before it takes the next, so once this returns, every result of a job
// that was running has been delivered or dropped.
fv::speech::DecodeJob final_job() {
    fv::speech::AudioClip clip;
    clip.samples = std::make_shared<const std::vector<float>>(16000, 0.0f);
    clip.end = clip.samples->size();
    fv::speech::DecodeJob job;
    job.input = std::move(clip);
    job.lane = Lane::Final;
    return job;
}

// Runs `fn` when the scope ends, a failed REQUIRE's unwinding included.
struct OnExit {
    std::function<void()> fn;
    ~OnExit() { fn(); }
};

// The id of the service's speech thread, from a decode it runs for the asking.
std::thread::id speech_thread(PreviewRig& p) {
    auto seen = std::make_shared<std::atomic<std::thread::id>>();
    {
        std::lock_guard lock(p.engine->mutex);
        p.engine->text = [seen](Lane lane, std::size_t call, std::span<const float>) {
            seen->store(std::this_thread::get_id());
            return lane == Lane::Preview ? "p" + std::to_string(call) : std::string();
        };
    }
    p.rig.speech->run(final_job());
    {
        // The engine's calls start counting again from the test's first.
        std::lock_guard lock(p.engine->mutex);
        p.engine->calls.clear();
    }
    return seen->load();
}

} // namespace

// --- set-up -----------------------------------------------------------------------------

TEST_CASE("streaming reconfigure can disable runtime", "[test_application_controller]") {
    // from tests/test_application_controller.py::test_streaming_reconfigure_can_disable_runtime
    PreviewRig p;
    REQUIRE(p.streaming->configured());
    REQUIRE(p.streaming->enabled());

    p.rig.settings.save_setting("streaming_enabled", false);
    p.streaming->reconfigure();

    CHECK_FALSE(p.streaming->configured());
    CHECK_FALSE(p.streaming->enabled());
    CHECK(p.streaming_statuses().empty());
}

TEST_CASE("streaming preview shares the loaded parakeet engine", "[test_application_controller]") {
    // from
    // tests/test_application_controller.py::test_streaming_preview_shares_the_loaded_parakeet_engine
    PreviewRigOptions options;
    options.settings["streaming_chunk_duration"] = 4.0;
    PreviewRig p(options);

    p.streaming->reconfigure();

    CHECK(p.streaming->enabled());
    REQUIRE(p.streaming->configured());
    CHECK(p.streaming->chunk_duration() == 4.0);
    // Its windows go to the dictation engine already loaded: no second engine is made.
    REQUIRE(p.rig.runtime->start_recording());
    p.recorder->append(blocks(173)); // the whole blocks that reach 4.0 s
    REQUIRE(p.drive_partials(1));
    CHECK(p.engine->lanes() == std::vector<Lane>{Lane::Preview});
    CHECK(p.engine->all()[0].samples == preview_samples(173 * kBlock));
    p.rig.runtime->cancel();
    // So the preview owns nothing to release.
    p.streaming->reconfigure();
    p.streaming->cleanup();
    std::lock_guard lock(p.engine->mutex);
    CHECK(p.engine->created == 1);
    CHECK(p.engine->releases == 0);
}

TEST_CASE("streaming preview waits for the optional engine to load",
          "[test_application_controller]") {
    // from
    // tests/test_application_controller.py::test_streaming_preview_waits_for_the_optional_engine_to_load
    PreviewRigOptions options;
    options.load_engine = false;
    options.set_up = false;
    PreviewRig p(options);
    ReloadDriver driver(p);

    p.streaming->reconfigure();

    CHECK_FALSE(p.streaming->configured());
    CHECK_FALSE(p.streaming->enabled());

    driver.machine.request();
    p.preview_ui.advance(fv::appcore::kReloadDebounce + 100ms);

    CHECK(driver.load_requested);
    CHECK(driver.setups_due == 1);
    CHECK(p.streaming->enabled());
    CHECK(p.streaming->configured());
}

TEST_CASE("startup sets up the preview after the optional engine loads",
          "[test_application_controller]") {
    // from
    // tests/test_application_controller.py::test_startup_sets_up_the_preview_after_the_optional_engine_loads
    PreviewRigOptions options;
    options.load_engine = false;
    options.set_up = false;
    PreviewRig p(options);
    ReloadDriver driver(p, false);

    driver.machine.initial_load();
    REQUIRE(driver.load_requested);
    CHECK_FALSE(p.streaming->configured());

    driver.finish_load();
    p.preview_ui.run_all(); // streaming_setup_due, delivered on the preview's UI executor

    CHECK(p.streaming->configured());
    CHECK(p.streaming->enabled());
}

TEST_CASE("streaming preview is unavailable for other optional engines",
          "[test_application_controller]") {
    // from
    // tests/test_application_controller.py::test_streaming_preview_is_unavailable_for_other_optional_engines
    // (the API engine)
    PreviewRigOptions options;
    options.settings["selected_model"] = "api";
    options.set_up = false;
    PreviewRig p(options);
    ReloadDriver driver(p);

    p.streaming->reconfigure();

    CHECK_FALSE(p.streaming->configured());
    CHECK_FALSE(p.streaming->enabled());
    REQUIRE_FALSE(p.streaming_statuses().empty());
    CHECK(p.streaming_statuses().back() == "Live preview needs Parakeet");
    // Not waiting for a load either: the next one sets nothing up.
    driver.machine.request();
    p.preview_ui.advance(fv::appcore::kReloadDebounce + 100ms);
    CHECK(driver.load_requested);
    CHECK(driver.setups_due == 0);
}

TEST_CASE("startup set-up stays quiet for an engine that cannot preview", "[streaming]") {
    // _preview_backend: only a reconfigure from Settings says why.
    PreviewRigOptions options;
    options.settings["selected_model"] = "api";
    PreviewRig p(options);

    CHECK_FALSE(p.streaming->configured());
    CHECK(p.streaming_statuses().empty());
}

TEST_CASE("a preview that still fits the engine stays set up", "[streaming]") {
    // setup_streaming keeps a preview set up for the same engine, or one a recording uses.
    PreviewRig p;
    REQUIRE(p.rig.runtime->start_recording());
    p.rig.load(); // a reload while recording: the engine's generation moves on

    p.streaming->setup();
    p.streaming->reconfigure();

    CHECK(p.streaming->configured());
    CHECK(p.streaming_statuses() ==
          std::vector<std::string>{"Stop recording before changing streaming mode"});
    p.rig.runtime->cancel();
}

TEST_CASE("changing the engine reconfigures the preview", "[streaming]") {
    // on_model_changed reconfigures the preview: the API engine cannot preview.
    PreviewRig p;
    REQUIRE(p.streaming->configured());

    p.rig.runtime->on_model_changed("api");

    CHECK_FALSE(p.streaming->configured());
    CHECK(p.streaming_statuses() == std::vector<std::string>{"Live preview needs Parakeet"});
}

// --- the Remote engine (tests/test_remote_engine.py) --------------------------------------

namespace {

// Dictation through a paired computer; `family` is the engine it said it runs once connected.
PreviewRigOptions remote_options(const std::string& fingerprint) {
    PreviewRigOptions options;
    options.settings["selected_model"] = "remote";
    options.settings["remote_engine_client"] = fv::Json{{"host", "127.0.0.1"},
                                                        {"port", 47821},
                                                        {"host_name", "devbox"},
                                                        {"device_id", "me"},
                                                        {"fingerprint", fingerprint}};
    return options;
}

// What the remote backend notes when it adopts the host's engine, forgotten with the test.
struct HostRuns {
    HostRuns(std::string fingerprint, std::string family) : fingerprint_(std::move(fingerprint)) {
        fv::speech::RemoteVoiceCapabilities host;
        host.host = "devbox";
        host.engine_label = "Fake";
        host.family = std::move(family);
        fv::speech::note_remote_runtime(fingerprint_, std::move(host));
    }
    ~HostRuns() { fv::speech::forget_remote_runtime(fingerprint_); }
    std::string fingerprint_;
};

} // namespace

TEST_CASE("test_live_preview_waits_for_the_host_then_decodes_there", "[test_remote_engine]") {
    const std::string pin(64, 'B');
    PreviewRigOptions options = remote_options(pin);
    options.settings["streaming_chunk_duration"] = 3.0;
    options.load_engine = false;
    options.set_up = false;
    PreviewRig p(options);
    ReloadDriver driver(p);

    // Selecting the engine reconfigures the preview before the reload has connected: it
    // waits, and says nothing, since the host has not said which engine it runs.
    p.streaming->reconfigure();
    CHECK_FALSE(p.streaming->configured());
    CHECK_FALSE(p.streaming->enabled());
    CHECK(p.streaming_statuses().empty());

    // The reload connects (the host runs Parakeet) and asks for the set-up again.
    const HostRuns host(pin, "parakeet");
    driver.machine.request();
    p.preview_ui.advance(fv::appcore::kReloadDebounce + 100ms);
    CHECK(driver.setups_due == 1);
    REQUIRE(p.streaming->configured());
    CHECK(p.streaming->enabled());

    // Its windows are decoded by the selected engine, the one the host is behind.
    REQUIRE(p.rig.runtime->start_recording());
    p.recorder->append(blocks(kWindowBlocks));
    REQUIRE(p.drive_partials(1));
    CHECK(p.engine->lanes() == std::vector<Lane>{Lane::Preview});
    p.rig.runtime->cancel();
}

TEST_CASE("test_a_remote_reload_keeps_a_fitting_preview_and_rebuilds_a_stale_one",
          "[test_remote_engine]") {
    // The preview here holds no engine of its own (its windows go to whatever the dictation slot
    // has loaded), so it is "stale" exactly when a reload replaced that engine. A preview that
    // was kept still has the window length it was set up with; a rebuilt one takes the saved one.
    const std::string pin(64, 'C');
    const HostRuns host(pin, "parakeet");
    PreviewRig p(remote_options(pin));
    REQUIRE(p.streaming->configured());
    REQUIRE(p.streaming->chunk_duration() == 3.0);
    p.rig.settings.save_setting("streaming_chunk_duration", 4.0);

    p.streaming->setup(); // the same connection: it stays
    CHECK(p.streaming->chunk_duration() == 3.0);

    REQUIRE(p.rig.runtime->start_recording());
    p.rig.load(); // the host came back with another model: a reload while recording
    p.streaming->setup();
    CHECK(p.streaming->chunk_duration() == 3.0); // a recording keeps its preview
    p.rig.runtime->cancel();

    p.streaming->setup();
    REQUIRE(p.streaming->configured());
    CHECK(p.streaming->chunk_duration() == 4.0);
}

TEST_CASE("test_a_host_without_a_preview_engine_says_so", "[test_remote_engine]") {
    const std::string pin(64, 'D');
    const HostRuns host(pin, "local_whisper");
    PreviewRigOptions options = remote_options(pin);
    options.set_up = false;
    PreviewRig p(options);

    p.streaming->reconfigure();

    CHECK_FALSE(p.streaming->configured());
    CHECK_FALSE(p.streaming->enabled());
    // The engine card grays out Live preview with the same words (speech::preview_status).
    CHECK(p.streaming_statuses() ==
          std::vector<std::string>{"Live preview needs Parakeet on devbox"});
    CHECK(p.streaming_statuses().back() ==
          fv::speech::preview_unavailable_reason("remote", "devbox", std::string("local_whisper")));
}

// --- the dictation's preview ------------------------------------------------------------

TEST_CASE("live preview text arrives every window while dictating", "[streaming]") {
    // Seven seconds of speech: the preview's text arrives at 3 s and at 6 s, and is published
    // as transcription.preview for the recording's job.
    PreviewRig p;
    REQUIRE(p.rig.runtime->start_recording());
    CHECK(p.shows() == 1);
    const auto job = p.rig.runtime->current_job();
    REQUIRE(job);

    p.recorder->append(blocks(kWindowBlocks - 1));
    for (int tick = 0; tick < 5; ++tick) {
        p.preview_ui.advance(100ms);
    }
    CHECK(p.engine->all().empty()); // not 3 s yet
    p.recorder->append(blocks(1));
    REQUIRE(p.drive_partials(1));
    p.recorder->append(blocks(kWindowBlocks));
    REQUIRE(p.drive_partials(2));
    p.recorder->append(blocks(41)); // to 7 s

    using Update = std::pair<std::string, bool>;
    CHECK(p.partial_texts() == std::vector<Update>{{"p1", true}, {"p1 p2", true}});
    CHECK(p.updates() == std::vector<Update>{{"p1", true}, {"p1 p2", true}});
    const auto samples = p.engine->all();
    REQUIRE(samples.size() == 2);
    CHECK(samples[0].samples == preview_samples(kWindowBlocks * kBlock));
    // The second window decodes 0.75 s of the first again in front of its own audio.
    CHECK(samples[1].samples == preview_samples(33075 + kWindowBlocks * kBlock));
    const auto previews = p.rig.events.of("transcription.preview");
    REQUIRE(previews.size() == 2);
    CHECK(previews[1].data == fv::Json{{"job_id", job->id}, {"text", "p1 p2"}, {"final", true}});
    CHECK_FALSE(previews[1].replay);
    p.rig.runtime->cancel();
}

TEST_CASE("stop returns at once and the final pass overtakes the queued preview window",
          "[streaming]") {
    // The engine is busy (another lane's job) when the 6 s window is submitted, so that window
    // waits on the Preview lane. The stop never waits for it; the final pass goes in on the Final
    // lane and runs first. It comes back empty, so the fallback waits for the queued window,
    // decodes the second the stop left, and shows the preview, unsaved.
    PreviewRig p;
    script(*p.engine, {"", "tail"});
    REQUIRE(p.rig.runtime->start_recording());
    p.recorder->append(blocks(kWindowBlocks));
    REQUIRE(p.drive_partials(1));

    p.engine->hold(Lane::Background);
    fv::speech::DecodeJob busy;
    fv::speech::AudioClip clip;
    clip.samples = std::make_shared<const std::vector<float>>(16000, 0.0f);
    clip.end = 16000;
    busy.input = clip;
    busy.lane = Lane::Background;
    p.rig.speech->submit(busy, p.rig.notify, [](auto) {});
    REQUIRE(p.engine->wait_waiting());
    p.recorder->append(blocks(kWindowBlocks));
    for (int tick = 0; tick < 5; ++tick) {
        p.preview_ui.advance(100ms);
    }
    std::this_thread::sleep_for(500ms); // the window is resampled and submitted off the UI
    p.recorder->append(blocks(41));

    const auto stop_started = std::chrono::steady_clock::now();
    p.rig.runtime->stop_recording();
    CHECK(std::chrono::steady_clock::now() - stop_started < 1s);
    CHECK(p.rig.last_status() == "Processing...");
    CHECK(p.hides() == 1);
    std::thread worker([&] { p.rig.jobs.run_all(); });
    const auto deadline = std::chrono::steady_clock::now() + 10s;
    while (!p.rig.trace.has("overlay:transcribing") &&
           std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(5ms);
    }
    // The final pass reads the file and goes in; nothing shows when it is queued, so this waits
    // generously for a machine busy with other tests.
    std::this_thread::sleep_for(1s);
    p.engine->open();
    worker.join();
    p.rig.run_workers();

    CHECK(p.engine->lanes() == std::vector<Lane>{Lane::Preview, Lane::Background, Lane::Final,
                                                 Lane::Preview, Lane::Final});
    // The window decoded after the stop never reached the screen.
    CHECK(p.partial_texts() == std::vector<std::pair<std::string, bool>>{{"p1", true}});
    CHECK(p.rig.transcript_text == "p1 p2 tail");
    CHECK(p.rig.last_status() == kFallbackStatus);
    CHECK(p.rig.entries().empty());
    CHECK(p.rig.keys.sent().empty());
    CHECK_FALSE(p.rig.runtime->has_active_job());
}

TEST_CASE("empty asr falls back to the streaming preview", "[test_application_controller]") {
    // from tests/test_application_controller.py::test_empty_asr_falls_back_to_the_streaming_preview
    PreviewRig p;
    script(*p.engine, {"   ", "fox  "}, {"  the quick brown"});
    REQUIRE(p.rig.runtime->start_recording());
    p.recorder->append(blocks(kWindowBlocks + 40));
    REQUIRE(p.drive_partials(1));

    p.rig.runtime->stop_recording();
    p.rig.run_workers();

    CHECK(p.rig.transcript_text == "the quick brown fox");
    CHECK(p.rig.last_status() == kFallbackStatus);
    // Never persisted, never pasted.
    CHECK(p.rig.entries().empty());
    CHECK(p.rig.clipboard.inner.writes.empty());
    CHECK(p.rig.clipboard.inner.stages.empty());
    CHECK(p.rig.keys.sent().empty());
    CHECK(p.rig.runtime->pending_streaming_text().empty());
}

TEST_CASE("streaming preview does not leak into a later job", "[test_application_controller]") {
    // from
    // tests/test_application_controller.py::test_streaming_preview_does_not_leak_into_a_later_job
    PreviewRig p;
    script(*p.engine, {"   ", "", "   "}, {"stale preview", ""});
    REQUIRE(p.rig.runtime->start_recording());
    p.recorder->append(blocks(kWindowBlocks));
    REQUIRE(p.drive_partials(1));
    p.rig.runtime->stop_recording();
    p.rig.run_workers();
    REQUIRE(p.rig.transcript_text == "stale preview");
    CHECK(p.rig.runtime->pending_streaming_text().empty());

    // The next dictation hears nothing, in its preview either.
    REQUIRE(p.rig.runtime->start_recording());
    p.recorder->append(blocks(kWindowBlocks));
    REQUIRE(p.drive_windows(2));
    p.rig.runtime->stop_recording();
    p.rig.run_workers();

    CHECK(p.rig.transcript_text == EMPTY_ASR_MESSAGE);
    CHECK(p.rig.last_status() == EMPTY_ASR_MESSAGE);
}

TEST_CASE("a real transcript ignores the streaming preview", "[test_application_controller]") {
    // from
    // tests/test_application_controller.py::test_a_real_transcript_ignores_the_streaming_preview
    PreviewRig p;
    script(*p.engine, {"The real transcript."}, {"rough preview"});
    REQUIRE(p.rig.runtime->start_recording());
    p.recorder->append(blocks(kWindowBlocks));
    REQUIRE(p.drive_partials(1));

    p.rig.runtime->stop_recording();
    p.rig.run_workers();

    CHECK(p.rig.transcript_text == "The real transcript.");
    CHECK_FALSE(p.rig.entries().empty());
}

TEST_CASE("stop recording keeps the streaming preview text", "[test_application_controller]") {
    // from
    // tests/test_application_controller.py::test_stop_recording_keeps_the_streaming_preview_text
    // and the preview half of test_stop_recording_does_not_block_the_qt_thread: the preview
    // stops on the worker, after the post-roll and before the save.
    PreviewRig p;
    script(*p.engine, {"Hello there."}, {"partial text"});
    REQUIRE(p.rig.runtime->start_recording());
    p.recorder->append(blocks(kWindowBlocks));
    REQUIRE(p.drive_partials(1));
    std::optional<std::string> at_wait;
    std::optional<std::string> at_save;
    p.recorder->on_wait = [&] {
        at_wait = p.rig.runtime->pending_streaming_text();
    };
    p.recorder->on_save = [&] {
        at_save = p.rig.runtime->pending_streaming_text();
    };

    p.rig.runtime->stop_recording();
    CHECK(p.rig.runtime->pending_streaming_text().empty());
    CHECK_FALSE(p.recorder_trace.has("waited"));
    p.rig.run_workers();

    CHECK(at_wait == "");
    CHECK(at_save == "partial text");
    CHECK(p.recorder_trace.index("waited") < p.recorder_trace.index("saved"));
}

TEST_CASE("a stop that claims no job leaves the next recording its preview", "[streaming]") {
    // A backup began while recording, so the stop claims no job and no worker stops the preview:
    // the stop does, or the preview would keep following the old tape and the next recording's
    // overlay would stay empty.
    PreviewRig p;
    REQUIRE(p.rig.runtime->start_recording());
    p.recorder->append(blocks(kWindowBlocks));
    REQUIRE(p.drive_partials(1));
    p.rig.activity.set_backup_in_progress(true);

    p.rig.runtime->stop_recording();

    CHECK_FALSE(p.rig.runtime->has_active_job());
    p.rig.activity.set_backup_in_progress(false);
    REQUIRE(p.rig.runtime->start_recording());
    CHECK(p.shows() == 2);
    p.recorder->append(blocks(kWindowBlocks));
    REQUIRE(p.drive_partials(2));
    CHECK(p.partial_texts().back() == std::pair<std::string, bool>{"p2", true});
    p.rig.runtime->cancel();
}

// --- the lazy tail ----------------------------------------------------------------------

TEST_CASE("runtime finalize uses the window preview and skips the rest",
          "[test_preview_lazy_tail]") {
    // from
    // tests/test_preview_lazy_tail.py::test_runtime_finalize_uses_the_window_preview_and_skips_the_rest
    PreviewRig p;
    {
        std::lock_guard lock(p.engine->mutex);
        p.engine->text = [](Lane, std::size_t, std::span<const float> samples) {
            return "n" + std::to_string(samples.size());
        };
    }
    REQUIRE(p.recorder->start_recording());
    p.streaming->start_session("job-1");
    p.recorder->append(blocks(43)); // one second
    p.preview_ui.advance(300ms);

    CHECK(p.streaming->stop_session() == "");
    CHECK(p.streaming->finalize() == "n" + std::to_string(preview_samples(43 * kBlock)));

    // Without a preview there is nothing to finish.
    PreviewRigOptions off;
    off.settings["streaming_enabled"] = false;
    PreviewRig none(off);
    CHECK(none.streaming->finalize() == std::nullopt);
}

TEST_CASE("empty dictation shows the lazily completed preview", "[test_preview_lazy_tail]") {
    // from tests/test_preview_lazy_tail.py::test_empty_dictation_shows_the_lazily_completed_preview
    PreviewRig p;
    script(*p.engine, {"  ", "the tail"});
    REQUIRE(p.rig.runtime->start_recording());
    p.recorder->append(blocks(86)); // two seconds: no window yet
    p.preview_ui.advance(300ms);

    p.rig.runtime->stop_recording();
    p.rig.run_workers();

    const auto calls = p.engine->all();
    REQUIRE(calls.size() == 2);
    CHECK(calls[1].lane == Lane::Final);
    CHECK(calls[1].samples == preview_samples(86 * kBlock));
    CHECK(p.rig.transcript_text == "the tail");
    CHECK(p.rig.last_status() == kFallbackStatus);
    CHECK(p.rig.runtime->pending_streaming_text().empty());
}

TEST_CASE("non empty dictation never decodes the kept tail", "[test_preview_lazy_tail]") {
    // from tests/test_preview_lazy_tail.py::test_non_empty_dictation_never_decodes_the_kept_tail
    PreviewRig p;
    REQUIRE(p.rig.runtime->start_recording());
    p.recorder->append(blocks(86));
    p.preview_ui.advance(300ms);

    p.rig.runtime->stop_recording();
    p.rig.run_workers();

    CHECK(p.engine->lanes() == std::vector<Lane>{Lane::Final});
    CHECK(p.rig.runtime->pending_streaming_text().empty());
    CHECK(p.rig.transcript_text == "Hello there.");
}

TEST_CASE("an upload never finishes an earlier dictation preview", "[test_preview_lazy_tail]") {
    // from
    // tests/test_preview_lazy_tail.py::test_an_upload_never_finishes_an_earlier_dictation_preview
    PreviewRig p;
    script(*p.engine, {"  ", "tail"});
    REQUIRE(p.rig.runtime->start_recording());
    p.recorder->append(blocks(86));
    p.preview_ui.advance(300ms);
    // The dictation fails before its fallback (the save), leaving the tail kept.
    p.recorder->save_ok = false;
    p.rig.runtime->stop_recording();
    p.rig.run_workers();
    REQUIRE(p.engine->all().empty());

    REQUIRE(p.rig.runtime->upload_audio_file(p.rig.wav("upload.wav")));
    p.rig.run_workers();

    CHECK(p.engine->lanes() == std::vector<Lane>{Lane::Final});
    CHECK(p.rig.runtime->pending_streaming_text().empty());
    CHECK(p.rig.transcript_text == EMPTY_ASR_MESSAGE);
}

TEST_CASE("a canceled job skips the fallback decode", "[test_preview_lazy_tail]") {
    // from tests/test_preview_lazy_tail.py::test_a_canceled_job_skips_the_fallback_decode
    PreviewRig p;
    script(*p.engine, {""});
    REQUIRE(p.rig.runtime->start_recording());
    p.recorder->append(blocks(86));
    p.preview_ui.advance(300ms);
    p.rig.runtime->stop_recording();
    REQUIRE(p.rig.runtime->cancel()); // during the post-roll

    p.rig.run_workers();

    CHECK(p.engine->all().empty());
    CHECK_FALSE(p.rig.runtime->has_active_job());
}

// --- StreamingRuntime -------------------------------------------------------------------

TEST_CASE("runtime stop mutes updates but preserves postroll until worker drains",
          "[test_streaming_transcriber]") {
    // from
    // tests/test_streaming_transcriber.py::test_runtime_stop_mutes_updates_but_preserves_postroll_until_worker_drains
    PreviewRig p;
    REQUIRE(p.recorder->start_recording());
    p.streaming->start_session("job-1");
    p.recorder->append(blocks(kWindowBlocks));
    REQUIRE(p.drive_partials(1));

    p.streaming->begin_stop();
    p.streaming->on_partial_transcription("tail", true);
    // The post-roll still feeds the preview: a window it completes is decoded, unseen.
    p.recorder->append(blocks(kWindowBlocks));
    REQUIRE(p.drive_windows(2));
    std::this_thread::sleep_for(100ms); // the window's text joins the preview after its decode
    p.preview_ui.advance(100ms);

    CHECK(p.partial_texts().size() == 1);
    CHECK(p.updates().size() == 1);
    CHECK(p.streaming->stop_session() == "p1 p2");
}

TEST_CASE("runtime cancel detaches recorder and uses nonblocking preview cancel",
          "[test_streaming_transcriber]") {
    // from
    // tests/test_streaming_transcriber.py::test_runtime_cancel_detaches_recorder_and_uses_nonblocking_preview_cancel
    PreviewRig p;
    p.engine->hold(Lane::Preview);
    REQUIRE(p.recorder->start_recording());
    p.streaming->start_session("job-1");
    p.recorder->append(blocks(kWindowBlocks));
    p.preview_ui.advance(100ms);
    REQUIRE(p.engine->wait_waiting()); // a window is decoding

    const auto started = std::chrono::steady_clock::now();
    p.streaming->cancel_session();
    CHECK(std::chrono::steady_clock::now() - started < 1s);

    CHECK(p.hides() == 1);
    // Cancel, not a stop: nothing is kept to finish later, and the window's text never shows.
    CHECK(p.streaming->finalize() == std::nullopt);
    p.engine->open();
    REQUIRE(p.engine->wait_calls(1));
    std::this_thread::sleep_for(50ms);
    p.preview_ui.advance(100ms);
    CHECK(p.partial_texts().empty());
}

// --- teardown ---------------------------------------------------------------------------

TEST_CASE("a preview the stop released does not outlive the runtime", "[streaming]") {
    // The stop's worker lets go of the preview through the UI executor. The runtime's owner may
    // destroy the clock right after the runtime (PreviewRig destroys it before the executor), so
    // the window still decoding must not reach the clock once the runtime is gone.
    PreviewRig p;
    WatchedClock clock(p.clock);
    // Declared after the clock, so a failed REQUIRE ends the runtime that reads the clock (and
    // lets the held window go) before the clock goes.
    const OnExit teardown{[&p] {
        p.engine->open();
        p.streaming.reset();
    }};
    p.rig.runtime->set_streaming(nullptr);
    p.links.clear();
    p.streaming = std::make_unique<StreamingRuntime>(*p.rig.speech, *p.recorder, p.rig.settings,
                                                     p.preview_ui, clock);
    p.streaming->setup();
    REQUIRE(p.streaming->configured());
    p.engine->hold(Lane::Preview);
    REQUIRE(p.recorder->start_recording());
    p.streaming->start_session("job-1");
    p.recorder->append(blocks(kWindowBlocks));
    p.preview_ui.advance(100ms);
    REQUIRE(p.engine->wait_waiting()); // the window is decoding
    p.streaming->stop_session();       // off the UI executor, as the stop's worker calls it

    p.streaming.reset();
    clock.gone = true;
    p.engine->open();
    p.rig.speech->run(final_job());

    CHECK(clock.late_reads == 0);
}

TEST_CASE("a destroyed window preview waits for the text being delivered", "[streaming]") {
    // The speech thread delivers a window's text through the UI executor; the preview's owner
    // may destroy that executor once the preview is gone, so the destructor waits for a
    // delivery under way (not for a decode).
    PreviewRig p;
    GatedExecutor ui(p.preview_ui, speech_thread(p));
    auto preview = std::make_unique<fv::speech::WindowPreview>(*p.rig.speech, ui, p.clock);
    // Declared after the preview, so a failed REQUIRE opens the gate before the destructor
    // waits on the delivery held at it.
    const OnExit open_gate{[&ui] {
        ui.open();
    }};
    REQUIRE(p.recorder->start_recording());
    preview->start(p.recorder->tape(), "job-1", [](std::string, bool) {});
    p.recorder->append(blocks(kWindowBlocks));
    p.preview_ui.advance(100ms);
    REQUIRE(ui.wait_entered()); // the speech thread is posting the window's text

    bool opened_first = false;
    std::thread opener([&] {
        std::this_thread::sleep_for(100ms);
        ui.open();
    });
    preview.reset();
    opened_first = ui.opened();
    opener.join();

    CHECK(opened_first);
}
