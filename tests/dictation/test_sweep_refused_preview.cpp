// Sweep H2: open item "dictation/streaming_runtime.cpp: when a recording's preview start is
// refused because the previous window is still decoding, an empty final transcript can show the
// previous recording's kept preview text through the fallback, as Python's StreamingTranscriber
// does". A deliberate difference: a recording whose preview never started has no preview text
// to fall back on. The previous recording keeps its own fallback.
//
// The transcription runtime stops a claimed recording's preview and finalizes it on the job's
// worker, and stops a recording refused as busy on the UI thread; these tests make the same
// calls on the same kinds of threads.

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <future>
#include <optional>
#include <string>
#include <thread>

#include "dictation/streaming_runtime.h"
#include "preview_support.h"
#include "speech/service.h"

using namespace fv::dictation;
using namespace fv::dictation::testing;
using fv::speech::Lane;

namespace {

constexpr int kWindowBlocks = 130; // whole blocks that reach 3.0 s at 44.1 kHz

Pcm blocks(int count) {
    return tone(static_cast<double>(count) * kBlock / kRate);
}

// The first recording: one window decoded ("p1"), the second held in the engine, so the
// preview is still decoding when the first recording's worker stops it.
void first_recording_with_a_window_in_flight(PreviewRig& p) {
    REQUIRE(p.recorder->start_recording());
    p.streaming->start_session("job-1");
    p.recorder->append(blocks(kWindowBlocks));
    REQUIRE(p.drive_partials(1));
    p.engine->hold(Lane::Preview);
    p.recorder->append(blocks(kWindowBlocks));
    REQUIRE(p.drive([&p] {
        std::lock_guard lock(p.engine->mutex);
        return p.engine->waiting > 0;
    }));
    p.recorder->stop_recording();
}

// The first recording's worker: stops the preview, then (once `finalize_now` is ready) finishes
// it, as an empty final transcript does.
struct FirstJob {
    std::promise<void> finalize_now;
    std::promise<std::string> stopped;
    std::optional<std::string> finalized;
    std::thread worker;

    explicit FirstJob(PreviewRig& p) {
        worker = std::thread([this, &p, go = finalize_now.get_future()]() mutable {
            stopped.set_value(p.streaming->stop_session());
            go.wait();
            finalized = p.streaming->finalize();
        });
    }
    ~FirstJob() {
        if (worker.joinable()) {
            finalize_now.set_value();
            worker.join();
        }
    }
    std::optional<std::string> finish() {
        finalize_now.set_value();
        worker.join();
        return finalized;
    }
};

} // namespace

TEST_CASE("a recording whose preview start was refused has no preview to fall back on",
          "[sweep_refused_preview][streaming]") {
    PreviewRig p;
    first_recording_with_a_window_in_flight(p);
    FirstJob first(p);
    CHECK(first.stopped.get_future().get() == "p1");

    // The second recording starts while the first one's window still decodes: no preview.
    REQUIRE(p.recorder->start_recording());
    p.streaming->start_session("job-2");
    CHECK(p.shows() == 1);
    p.recorder->append(blocks(40));
    p.recorder->stop_recording();

    // The first recording's fallback is its own.
    p.engine->open();
    const std::optional<std::string> first_text = first.finish();
    REQUIRE(first_text);
    CHECK(first_text->starts_with("p1 p2"));

    // The second recording's job: its stop and its fallback on its own worker.
    std::optional<std::string> second_stop;
    std::optional<std::string> second_text = "(not run)";
    std::thread([&] {
        second_stop = p.streaming->stop_session();
        second_text = p.streaming->finalize();
    }).join();
    CHECK(second_stop == "");
    CHECK(second_text == std::nullopt);
}

TEST_CASE("a refused recording stopped as busy leaves the previous recording its fallback",
          "[sweep_refused_preview][streaming]") {
    PreviewRig p;
    first_recording_with_a_window_in_flight(p);
    FirstJob first(p);
    CHECK(first.stopped.get_future().get() == "p1");

    REQUIRE(p.recorder->start_recording());
    p.streaming->start_session("job-2");
    p.recorder->append(blocks(40));
    p.recorder->stop_recording();
    // The first recording's job still holds the slot: this stop is refused as busy, on the UI
    // thread, and ends the second recording there.
    CHECK(p.streaming->stop_session() == "");

    p.engine->open();
    const std::optional<std::string> first_text = first.finish();
    REQUIRE(first_text);
    CHECK(first_text->starts_with("p1 p2"));
}

TEST_CASE("a recording after a refused one previews and falls back as usual",
          "[sweep_refused_preview][streaming]") {
    PreviewRig p;
    first_recording_with_a_window_in_flight(p);
    {
        FirstJob first(p);
        CHECK(first.stopped.get_future().get() == "p1");
        REQUIRE(p.recorder->start_recording());
        p.streaming->start_session("job-2");
        p.recorder->stop_recording();
        p.engine->open();
        REQUIRE(first.finish());
    }
    std::thread([&] {
        CHECK(p.streaming->stop_session() == "");
        CHECK(p.streaming->finalize() == std::nullopt);
    }).join();

    // The third recording's preview starts, and its fallback is its own text.
    REQUIRE(p.recorder->start_recording());
    p.streaming->start_session("job-3");
    CHECK(p.shows() == 2);
    p.recorder->append(blocks(kWindowBlocks));
    REQUIRE(p.drive_partials(2));
    p.recorder->stop_recording();
    std::optional<std::string> third;
    std::thread([&] {
        CHECK(p.streaming->stop_session() == "p3");
        third = p.streaming->finalize();
    }).join();
    CHECK(third == "p3");
}
