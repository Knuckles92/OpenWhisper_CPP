// The live preview at Quit and when its set-up fails, and the preview of a stop refused as busy
// (services/runtime/streaming.py, services/runtime/transcription.py _stop_recording; audit
// A04-63, A04-62, A04-10).
#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <functional>
#include <future>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "base/clock.h"
#include "base/identity.h"
#include "dictation/streaming_runtime.h"
#include "dictation/transcription_runtime.h"
#include "preview_support.h"
#include "speech/service.h"
#include "speech/window_preview.h"

using namespace fv::dictation;
using namespace fv::dictation::testing;
using fv::speech::Lane;
using namespace std::chrono_literals;

namespace {

constexpr int kWindowBlocks = 130; // whole blocks that reach 3.0 s at 44.1 kHz

Pcm blocks(int count) {
    return tone(static_cast<double>(count) * kBlock / kRate);
}

// Runs `fn` when the scope ends, a failed REQUIRE's unwinding included.
struct OnExit {
    std::function<void()> fn;
    ~OnExit() { fn(); }
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

// One second of silence on `lane`.
fv::speech::DecodeJob silent_job(Lane lane) {
    fv::speech::AudioClip clip;
    clip.samples = std::make_shared<const std::vector<float>>(16000, 0.0f);
    clip.end = clip.samples->size();
    fv::speech::DecodeJob job;
    job.input = std::move(clip);
    job.lane = lane;
    return job;
}

// Replaces the rig's runtime with one over `clock` and `make_preview`, and records its status
// lines in `statuses`.
void replace_runtime(PreviewRig& p, fv::Clock& clock,
                     StreamingRuntime::PreviewFactory make_preview = {}) {
    p.rig.runtime->set_streaming(nullptr);
    p.links.clear();
    p.streaming = std::make_unique<StreamingRuntime>(*p.rig.speech, *p.recorder, p.rig.settings,
                                                     p.preview_ui, clock, std::move(make_preview));
    p.links.push_back(p.streaming->status.connect(p.rig.ui, [&p](std::string text) {
        std::lock_guard lock(p.mutex);
        p.statuses.push_back(std::move(text));
    }));
}

} // namespace

// Quit's order (A04-63): the streaming step (StreamingRuntime::cleanup) runs before the jobs
// strand is waited for, and the context, the runtime with it, goes after. A stop's worker inside
// finalize() must leave it at once, without decoding, so that wait is short and nothing of the
// runtime is used once it is gone.
TEST_CASE("quit's streaming cleanup ends a stop finalizing its preview at once", "[streaming]") {
    PreviewRig p;
    WatchedClock clock(p.clock);
    // Declared after the clock: a failed REQUIRE ends the runtime before the clock goes.
    const OnExit teardown{[&p] {
        p.engine->open();
        p.streaming.reset();
    }};
    replace_runtime(p, clock);
    p.streaming->setup();
    REQUIRE(p.streaming->configured());

    // The engine is busy with other work, so the window finalize() submits waits in the queue.
    p.engine->hold(Lane::Background);
    p.rig.speech->submit(silent_job(Lane::Background), p.preview_ui, [](auto) {});
    REQUIRE(p.engine->wait_waiting());

    REQUIRE(p.recorder->start_recording());
    p.streaming->start_session("job-1");
    p.recorder->append(blocks(86)); // two seconds: no live window, all of it left to finalize
    p.preview_ui.advance(300ms);
    p.streaming->stop_session();

    // The stop's worker.
    std::future<std::optional<std::string>> finalized =
        std::async(std::launch::async, [&p] { return p.streaming->finalize(); });
    // Declared after the future: a failed REQUIRE lets the queue move before the future's
    // destructor waits for the worker.
    const OnExit open_gate{[&p] {
        p.engine->open();
    }};
    REQUIRE(finalized.wait_for(300ms) == std::future_status::timeout); // its window is queued

    p.streaming->cleanup(); // ShutdownStep::Streaming

    REQUIRE(finalized.wait_for(2s) == std::future_status::ready);
    CHECK(finalized.get() == std::nullopt);
    // The context goes once the worker has returned, then the clock.
    p.streaming.reset();
    clock.gone = true;
    p.engine->open();
    p.rig.speech->run(silent_job(Lane::Final));
    CHECK(clock.late_reads == 0);
    // The finalize window was dropped from the queue, never decoded: the engine took the held
    // job and the test's last one only.
    CHECK(p.engine->lanes() == std::vector<Lane>{Lane::Background, Lane::Final});
}

TEST_CASE("cancel ends a finalize whose window is queued behind other work", "[streaming]") {
    // The preview itself: cancel() reaches the decode finalize() waits on.
    PreviewRig p;
    p.engine->hold(Lane::Background);
    p.rig.speech->submit(silent_job(Lane::Background), p.preview_ui, [](auto) {});
    REQUIRE(p.engine->wait_waiting());
    fv::speech::WindowPreview preview(*p.rig.speech, p.preview_ui, p.clock);
    REQUIRE(p.recorder->start_recording());
    preview.start(p.recorder->tape(), "job-1", [](std::string, bool) {});
    p.recorder->append(blocks(86));
    p.preview_ui.advance(300ms);
    preview.stop();

    std::future<std::optional<std::string>> finalized =
        std::async(std::launch::async, [&preview] { return preview.finalize(); });
    const OnExit open_gate{[&p] {
        p.engine->open();
    }};
    REQUIRE(finalized.wait_for(300ms) == std::future_status::timeout);

    preview.cancel();

    REQUIRE(finalized.wait_for(2s) == std::future_status::ready);
    CHECK(finalized.get() == std::nullopt);
    p.engine->open();
    p.rig.speech->run(silent_job(Lane::Final));
    CHECK(p.engine->lanes() == std::vector<Lane>{Lane::Background, Lane::Final});
}

// A04-62: _configure_streaming's except clause. Only a reconfigure tells the user; the set-up at
// startup and after a load logs it alone.
TEST_CASE("a preview that fails to set up says so on reconfigure only", "[streaming]") {
    PreviewRigOptions options;
    options.set_up = false;
    PreviewRig p(options);
    int attempts = 0;
    replace_runtime(p, p.clock,
                    [&attempts](const fv::speech::WindowPreviewOptions&)
                        -> std::shared_ptr<fv::speech::WindowPreview> {
                        ++attempts;
                        throw std::runtime_error("no preview");
                    });

    p.streaming->setup();
    CHECK(attempts == 1);
    CHECK_FALSE(p.streaming->configured());
    CHECK_FALSE(p.streaming->enabled());
    CHECK(p.streaming_statuses().empty());

    p.streaming->reconfigure();
    CHECK(attempts == 2);
    CHECK_FALSE(p.streaming->configured());
    CHECK_FALSE(p.streaming->enabled());
    CHECK(p.streaming_statuses() == std::vector<std::string>{"Failed to reconfigure streaming"});
}

// A04-10, a deliberate difference: OpenWhisper's _stop_recording returns on a refused claim
// without stopping the preview, which keeps streaming over the old recording; the next
// recording's start_streaming then refuses ("Streaming worker is still active") and its overlay
// joins the old text. OpenWhisper_CPP stops it, so the next recording previews afresh.
TEST_CASE("a stop refused as busy stops the preview for the next recording", "[streaming]") {
    PreviewRig p;
    REQUIRE(p.rig.runtime->start_recording());
    p.recorder->append(blocks(kWindowBlocks));
    REQUIRE(p.drive_partials(1));
    p.rig.activity.set_host_update_active(true);

    p.rig.runtime->stop_recording();

    CHECK_FALSE(p.rig.runtime->has_active_job());
    CHECK(p.rig.last_status() ==
          std::string(fv::identity().product) + " is preparing an update. Try again shortly.");
    p.rig.activity.set_host_update_active(false);
    REQUIRE(p.rig.runtime->start_recording());
    CHECK(p.shows() == 2);
    p.recorder->append(blocks(kWindowBlocks));
    REQUIRE(p.drive_partials(2));
    // The second recording's own text, not the first's joined with it.
    CHECK(p.partial_texts().back() == std::pair<std::string, bool>{"p2", true});
    p.rig.runtime->cancel();
}
