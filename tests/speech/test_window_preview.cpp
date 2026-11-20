// WindowPreview (services/streaming_transcriber.py StreamingTranscriber) over a real
// SpeechService with a scripted engine: windows, overlap, the lazy tail, cancel and deadlines.
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <atomic>
#include <chrono>
#include <cmath>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <tuple>
#include <vector>

#include "golden/golden.h"
#include "preview_test_support.h"
#include "speech/parakeet_backend.h"
#include "speech/testing/fake_decoder.h"
#include "speech/window_preview.h"

using fv::speech::append_preview_text;
using fv::speech::Lane;
using fv::speech::ReadyState;
using fv::speech::Slot;
using fv::speech::WindowPreview;
using fv::speech::WindowPreviewOptions;
using fv::speech::test::Block;
using fv::speech::test::content_text;
using fv::speech::test::kBlock;
using fv::speech::test::kOverlapFrames;
using fv::speech::test::kRate;
using fv::speech::test::kWindowBlocks;
using fv::speech::test::noise_blocks;
using fv::speech::test::old_drain;
using fv::speech::test::PreviewHarness;
using fv::speech::test::resampled;
using fv::speech::test::slice;
using fv::speech::test::tone;
using fv::speech::test::tone_blocks;
using fv::speech::test::windows_published;
using namespace std::chrono_literals;

namespace {

using Updates = std::vector<std::pair<std::string, bool>>;

// A ParakeetBackend over FakeDecoders behind a service: the dictation engine's own preview path.
struct ParakeetHarness {
    explicit ParakeetHarness(fv::speech::testing::FakeDecoders::Script script)
        : decoders(std::make_shared<fv::speech::testing::FakeDecoders>(std::move(script))),
          service(
              [d = decoders](const fv::speech::BackendConfig& config) {
                  return std::make_unique<fv::speech::ParakeetBackend>(config, d->factory());
              },
              notify),
          preview(service, ui, clock) {
        service.select(Slot::Dictation, fv::speech::test::parakeet_config("cpu"));
        REQUIRE(fv::speech::test::pump_until(notify, [this] {
            return service.readiness(Slot::Dictation).state == ReadyState::Ready;
        }));
        tape = std::make_shared<fv::audio::PcmTape>(kRate);
        preview.start(tape, "job-1", [this](std::string text, bool final) {
            updates.emplace_back(std::move(text), final);
        });
    }

    bool drive(const std::function<bool()>& done) {
        const auto deadline = std::chrono::steady_clock::now() + 10s;
        while (std::chrono::steady_clock::now() < deadline) {
            ui.advance(100ms);
            notify.run_all();
            if (done()) {
                return true;
            }
            std::this_thread::sleep_for(1ms);
        }
        return false;
    }

    std::shared_ptr<fv::speech::testing::FakeDecoders> decoders;
    fv::ManualExecutor notify;
    fv::ManualExecutor ui;
    fv::FakeClock clock;
    fv::speech::SpeechService service;
    WindowPreview preview;
    std::shared_ptr<fv::audio::PcmTape> tape;
    Updates updates;
};

} // namespace

// --- test_streaming_transcriber.py -------------------------------------------------------------

TEST_CASE("test_window_decode_uses_the_backend_decoder_and_appends_text",
          "[test_streaming_transcriber]") {
    // from
    // tests/test_streaming_transcriber.py::test_window_decode_uses_the_backend_decoder_and_appends_text
    PreviewHarness h;
    h.start();
    h.feed(tone_blocks(2 * kWindowBlocks));
    REQUIRE(h.settle(2));

    CHECK(h.preview.preview_text() == "hello there hello there");
    REQUIRE_FALSE(h.updates.empty());
    CHECK(h.updates.back() == std::pair<std::string, bool>("hello there hello there", true));
    CHECK(h.preview.chunk_count() == 2);
    const auto lengths = h.engine->call_lengths();
    // A window is 130 whole recorder blocks (3.02 s); the second carries the 0.75 s overlap
    // tail in front of its new audio.
    CHECK(lengths == std::vector<std::size_t>{resampled(kWindowBlocks * kBlock),
                                              resampled(kWindowBlocks * kBlock + kOverlapFrames)});
    CHECK(h.engine->languages == std::vector<std::string>{"auto", "auto"});
    CHECK(h.engine->lanes == std::vector<Lane>{Lane::Preview, Lane::Preview});
}

TEST_CASE("test_window_is_skipped_while_the_engine_is_unloaded", "[test_streaming_transcriber]") {
    // from tests/test_streaming_transcriber.py::test_window_is_skipped_while_the_engine_is_unloaded
    PreviewHarness h({}, /*load=*/false);
    h.start();
    h.feed(tone_blocks(kWindowBlocks));
    h.drive([] { return false; }, 300ms);

    CHECK(h.preview.preview_text().empty());
    CHECK(h.preview.chunk_count() == 0);
    CHECK(h.updates.empty());
    CHECK(h.engine->calls() == 0);
    CHECK_FALSE(h.preview.decoding());
}

TEST_CASE("test_optional_engine_worker_decodes_preview_windows", "[test_streaming_transcriber]") {
    // from tests/test_streaming_transcriber.py::test_optional_engine_worker_decodes_preview_windows
    fv::speech::testing::FakeDecoders::Script script;
    script.texts = {"mister quilter"};
    ParakeetHarness h(script);
    for (const Block& block : tone_blocks(kWindowBlocks)) {
        h.tape->append(block);
    }
    REQUIRE(h.drive([&] { return !h.updates.empty() && !h.preview.decoding(); }));

    CHECK(h.preview.preview_text() == "mister quilter");
    CHECK(h.updates == Updates{{"mister quilter", true}});
    const auto decodes = h.decoders->decodes();
    REQUIRE(decodes.size() == 1);
    CHECK(decodes[0].language == "auto");
}

TEST_CASE("test_quiet_window_never_reaches_the_optional_engine_worker",
          "[test_streaming_transcriber]") {
    // from
    // tests/test_streaming_transcriber.py::test_quiet_window_never_reaches_the_optional_engine_worker
    ParakeetHarness h({});
    h.tape->append(Block(static_cast<std::size_t>(kWindowBlocks) * kBlock, 0));
    REQUIRE(h.drive([&] { return h.preview.chunk_count() == 1; }));

    CHECK(h.decoders->decodes().empty());
    CHECK(h.preview.preview_text().empty());
    CHECK(h.preview.chunk_count() == 1);
    CHECK(h.updates.empty());
}

TEST_CASE("test_overlap_join_is_bounded_and_preserves_ambiguous_repetitions",
          "[test_streaming_transcriber]") {
    // from
    // tests/test_streaming_transcriber.py::test_overlap_join_is_bounded_and_preserves_ambiguous_repetitions
    const auto [before, chunk, maximum, expected] =
        GENERATE(table<std::string, std::string, int, std::string>({
            {"Meet the Alpha team.", "alpha TEAM, tomorrow morning.", 6,
             "Meet the Alpha team. tomorrow morning."},
            {"one two three", "two three four", 6, "one two three four"},
            {"very", "very good", 6, "very very good"},
            {"yes yes", "yes yes", 6, "yes yes yes yes"},
            {"one two", "one two three", 0, "one two one two three"},
            {"we don't know", "don’t know yet", 6, "we don't know yet"},
            {"one two three", "one two three four", 2, "one two three one two three four"},
            {"one two", "", 6, "one two"},
        }));
    CHECK(append_preview_text(before, chunk, maximum) == expected);
}

TEST_CASE("test_window_overlap_removes_redecoded_phrase_but_zero_overlap_preserves_it",
          "[test_streaming_transcriber]") {
    // from
    // tests/test_streaming_transcriber.py::test_window_overlap_removes_redecoded_phrase_but_zero_overlap_preserves_it
    const auto [overlap, expected] = GENERATE(table<double, std::string>({
        {0.75, "one two three four"},
        {0.0, "one two three two three four"},
    }));
    WindowPreviewOptions options;
    options.overlap_s = overlap;
    PreviewHarness h(options);
    h.engine->text = [](std::size_t call, std::span<const float>) {
        return std::string(call % 2 == 1 ? "one two three" : "two three four");
    };
    h.start();
    h.feed(tone_blocks(2 * kWindowBlocks));
    REQUIRE(h.settle(2));
    CHECK(h.preview.preview_text() == expected);
}

TEST_CASE("test_window_finalize_flushes_recording_shorter_than_one_window",
          "[test_streaming_transcriber]") {
    // from
    // tests/test_streaming_transcriber.py::test_window_finalize_flushes_recording_shorter_than_one_window
    PreviewHarness h;
    h.start();
    h.feed(tone(4410));
    h.drive([] { return false; }, 200ms);
    // Stop keeps the partial window; only the empty-result fallback decodes it.
    CHECK(h.preview.stop().empty());
    CHECK(h.preview.chunk_count() == 0);
    CHECK(h.preview.finalize() == std::optional<std::string>("hello there"));
    // The overlay is gone by then, so the late window never reaches the UI.
    h.ui.run_all();
    CHECK(h.updates.empty());
    CHECK(h.preview.chunk_count() == 1);
    CHECK(h.engine->call_lengths() == std::vector<std::size_t>{resampled(4410)});
}

TEST_CASE("test_window_finalize_consumes_inflight_generator_and_partial_tail",
          "[test_streaming_transcriber]") {
    // from
    // tests/test_streaming_transcriber.py::test_window_finalize_consumes_inflight_generator_and_partial_tail
    WindowPreviewOptions options;
    options.overlap_s = 0.0;
    PreviewHarness h(options);
    h.engine->stall_on = 1;
    h.engine->text = [](std::size_t call, std::span<const float>) {
        return std::string(call == 1 ? "first window" : "last word");
    };
    h.start();
    h.feed(tone_blocks(kWindowBlocks));
    REQUIRE(h.drive([&] { return h.preview.decoding(); }));
    REQUIRE(h.engine->wait_entered());
    h.feed(tone(4410));
    // Stop returns while the window is still decoding.
    CHECK(h.preview.stop().empty());
    h.engine->release();
    CHECK(h.preview.finalize() == std::optional<std::string>("first window last word"));
    CHECK(h.engine->calls() == 2);
}

TEST_CASE("test_preview_buffer_absorbs_a_decode_stall_without_dropping_recorder_blocks",
          "[test_streaming_transcriber]") {
    // from
    // tests/test_streaming_transcriber.py::test_preview_buffer_absorbs_a_decode_stall_without_dropping_recorder_blocks
    PreviewHarness h;
    h.engine->stall_on = 1;
    h.engine->text = content_text;
    h.start();
    const auto blocks = noise_blocks(3.6, 41);
    h.feed(slice(blocks, 0, kWindowBlocks));
    REQUIRE(h.drive([&] { return h.preview.decoding(); }));
    REQUIRE(h.engine->wait_entered());
    // Forty recorder blocks arrive while the engine is stalled.
    for (int i = 0; i < 40; ++i) {
        h.feed(Block(kBlock, 0));
    }
    // The buffer holds at least 3 s of blocks: max(10, ceil(3.0 * 44100 / 1024)).
    const WindowPreviewOptions defaults;
    const auto buffer_blocks = std::max<std::int64_t>(
        defaults.buffer_blocks,
        static_cast<std::int64_t>(std::ceil(defaults.buffer_s * kRate / defaults.block_frames)));
    CHECK(buffer_blocks * defaults.block_frames >= 3 * kRate);
    h.engine->release();
    REQUIRE(h.settle(1));
    h.preview.stop();
    REQUIRE(h.preview.finalize().has_value());
    // None of the forty was dropped: the tail window holds all of them behind the overlap.
    CHECK(h.engine->call_lengths() ==
          std::vector<std::size_t>{resampled(kWindowBlocks * kBlock),
                                   resampled(40 * kBlock + kOverlapFrames)});
}

TEST_CASE("blocks past the buffer are dropped while a window decodes",
          "[test_streaming_transcriber]") {
    // STREAMING_QUEUE_SEC: feed_audio drops blocks while the queue is full.
    PreviewHarness h;
    h.engine->stall_on = 1;
    h.engine->text = content_text;
    h.start();
    const auto blocks = noise_blocks(10.0, 42); // 431 blocks
    h.feed(slice(blocks, 0, kWindowBlocks));
    REQUIRE(h.drive([&] { return h.preview.decoding(); }));
    REQUIRE(h.engine->wait_entered());
    h.feed(slice(blocks, kWindowBlocks));
    h.engine->release();
    REQUIRE(h.settle(2));
    h.preview.stop();
    // The 130 blocks after the first window filled the buffer; the other 171 were dropped.
    CHECK(h.preview.finalize() == std::optional<std::string>(old_drain(slice(blocks, 0, 260))));
    CHECK(h.engine->calls() == 2);
}

TEST_CASE("test_timed_out_worker_cannot_publish_late_or_join_the_next_recording",
          "[test_streaming_transcriber]") {
    // from
    // tests/test_streaming_transcriber.py::test_timed_out_worker_cannot_publish_late_or_join_the_next_recording
    PreviewHarness h;
    h.engine->stall_on = 1;
    h.engine->text = [](std::size_t, std::span<const float>) {
        return std::string("late old text");
    };
    h.start();
    h.feed(tone_blocks(kWindowBlocks));
    REQUIRE(h.drive([&] { return h.preview.decoding(); }));
    REQUIRE(h.engine->wait_entered());
    CHECK(h.preview.stop().empty());
    // The deadline bounds the fallback's wait for the window.
    CHECK(h.preview.finalize(50ms) == std::optional<std::string>(""));
    h.start();
    CHECK(h.preview.decoding());
    CHECK_FALSE(h.preview.running());
    h.engine->release();
    REQUIRE(h.drive([&] { return !h.preview.decoding(); }));
    CHECK(h.updates.empty());
    CHECK(h.preview.preview_text().empty());

    h.engine->text = [](std::size_t, std::span<const float>) {
        return std::string("hello there");
    };
    h.start();
    CHECK(h.preview.running());
    h.feed(tone(4410));
    h.preview.stop();
    CHECK(h.preview.finalize() == std::optional<std::string>("hello there"));
}

TEST_CASE("test_abandoned_preview_suppresses_late_output_and_releases_native_session",
          "[test_streaming_transcriber]") {
    // from
    // tests/test_streaming_transcriber.py::test_abandoned_preview_suppresses_late_output_and_releases_native_session
    // (the window preview's half; the native stream is dropped)
    const std::string action = GENERATE("cancel", "timeout");
    PreviewHarness h;
    h.engine->stall_on = 1;
    h.engine->text = [](std::size_t, std::span<const float>) {
        return std::string("old recording");
    };
    h.start();
    h.feed(tone_blocks(kWindowBlocks));
    REQUIRE(h.drive([&] { return h.preview.decoding(); }));
    REQUIRE(h.engine->wait_entered());
    if (action == "timeout") {
        h.preview.stop();
        CHECK(h.preview.finalize(50ms) == std::optional<std::string>(""));
    } else {
        h.preview.cancel();
    }
    // A second recording cannot share the old window's decode while it is blocked.
    h.start();
    CHECK(h.preview.decoding());
    CHECK_FALSE(h.preview.running());
    h.engine->release();
    REQUIRE(h.drive([&] { return !h.preview.decoding(); }));
    CHECK(h.updates.empty());
    CHECK(h.preview.preview_text().empty());

    h.start();
    h.feed(tone(4410));
    h.preview.stop();
    CHECK(h.preview.finalize() == std::optional<std::string>("old recording"));
}

// --- test_preview_lazy_tail.py -----------------------------------------------------------------

TEST_CASE("test_fast_stop_returns_without_decoding_and_wakes_the_worker",
          "[test_preview_lazy_tail]") {
    // from
    // tests/test_preview_lazy_tail.py::test_fast_stop_returns_without_decoding_and_wakes_the_worker
    WindowPreviewOptions options;
    options.poll = 30s;
    PreviewHarness h(options);
    h.engine->text = content_text;
    h.start();
    const auto blocks = noise_blocks(1.0, 0);
    h.feed(blocks);

    CHECK(h.preview.stop().empty());
    CHECK_FALSE(h.preview.decoding());
    CHECK(h.engine->calls() == 0);
    CHECK(h.preview.finalize() == std::optional<std::string>(old_drain(blocks)));
    CHECK(h.engine->call_lengths() ==
          std::vector<std::size_t>{resampled(static_cast<std::int64_t>(blocks.size()) * kBlock)});
    h.ui.run_all();
    CHECK(h.updates.empty());
}

TEST_CASE("test_finalize_matches_the_old_drain_when_stop_finds_the_worker_idle",
          "[test_preview_lazy_tail]") {
    // from
    // tests/test_preview_lazy_tail.py::test_finalize_matches_the_old_drain_when_stop_finds_the_worker_idle
    // 431 blocks: three windows decoded live and a 41-block partial window.
    const auto blocks = noise_blocks(10.0, 7);
    REQUIRE(blocks.size() == 431);
    PreviewHarness h;
    h.engine->text = content_text;
    h.start();
    // Paced like the recorder, so the 130-block buffer never overflows.
    for (std::size_t start = 0; start < blocks.size(); start += 100) {
        h.feed(slice(blocks, start, start + 100));
        REQUIRE(h.drive([&] { return !h.preview.decoding(); }));
    }
    REQUIRE(h.settle(3));

    h.preview.stop();

    CHECK(h.preview.finalize() == std::optional<std::string>(old_drain(blocks)));
    CHECK(h.updates == windows_published(blocks, 3));
}

TEST_CASE("test_finalize_matches_the_old_drain_across_a_kept_window_and_queued_blocks",
          "[test_preview_lazy_tail]") {
    // from
    // tests/test_preview_lazy_tail.py::test_finalize_matches_the_old_drain_across_a_kept_window_and_queued_blocks
    const auto blocks = noise_blocks(500.0 * kBlock / kRate, 8);
    REQUIRE(blocks.size() == 500);
    PreviewHarness h;
    h.engine->text = content_text;
    h.start();
    // Two windows and a 40-block partial are taken; 200 more are left on the tape.
    h.feed(slice(blocks, 0, 130));
    REQUIRE(h.settle(1));
    h.feed(slice(blocks, 130, 260));
    REQUIRE(h.settle(2));
    h.feed(slice(blocks, 260, 300));
    h.drive([] { return false; }, 200ms);
    h.feed(slice(blocks, 300));

    CHECK(h.preview.stop() == old_drain(slice(blocks, 0, 260)));
    // Finalize cuts [260:390] from the kept and queued blocks, then [390:500].
    CHECK(h.preview.finalize() == std::optional<std::string>(old_drain(blocks)));
    const auto lengths = h.engine->call_lengths();
    REQUIRE(lengths.size() == 4);
    CHECK(lengths[2] == resampled(130 * kBlock + kOverlapFrames));
    CHECK(lengths[3] == resampled(110 * kBlock + kOverlapFrames));
    CHECK(h.updates == windows_published(blocks, 2));
}

TEST_CASE("test_finalize_matches_the_old_drain_after_a_window_in_flight_at_stop",
          "[test_preview_lazy_tail]") {
    // from
    // tests/test_preview_lazy_tail.py::test_finalize_matches_the_old_drain_after_a_window_in_flight_at_stop
    const auto blocks = noise_blocks(360.0 * kBlock / kRate, 6);
    REQUIRE(blocks.size() == 360);
    PreviewHarness h;
    h.engine->text = content_text;
    h.engine->stall_on = 2;
    h.start();
    // Let the first window finish before feeding the second.
    h.feed(slice(blocks, 0, 130));
    REQUIRE(h.settle(1));
    // Window 2 stalls in the engine; 100 blocks wait behind it.
    h.feed(slice(blocks, 130, 260));
    REQUIRE(h.drive([&] { return h.engine->calls() == 2; }));
    REQUIRE(h.engine->wait_entered());
    h.feed(slice(blocks, 260));

    CHECK(h.preview.stop() == old_drain(slice(blocks, 0, 130)));
    h.engine->release();

    CHECK(h.preview.finalize() == std::optional<std::string>(old_drain(blocks)));
    // Window 2 finished after stop, so only window 1 reached the UI.
    h.ui.run_all();
    CHECK(h.updates == windows_published(blocks, 1));
}

TEST_CASE("test_in_flight_window_lands_for_the_fallback_but_never_reaches_the_ui",
          "[test_preview_lazy_tail]") {
    // from
    // tests/test_preview_lazy_tail.py::test_in_flight_window_lands_for_the_fallback_but_never_reaches_the_ui
    PreviewHarness h;
    h.engine->text = content_text;
    h.engine->stall_on = 1;
    h.start();
    const auto blocks = noise_blocks(3.5, 3);
    h.feed(slice(blocks, 0, 130));
    REQUIRE(h.drive([&] { return h.preview.decoding(); }));
    REQUIRE(h.engine->wait_entered());
    // The tail arrives after the engine started decoding, so none of it is dropped.
    h.feed(slice(blocks, 130));

    const auto started = std::chrono::steady_clock::now();
    CHECK(h.preview.stop().empty());
    // Stop did not wait for the stalled engine.
    CHECK(std::chrono::steady_clock::now() - started < 1s);
    h.engine->release();
    REQUIRE(h.drive([&] { return !h.preview.decoding(); }));

    CHECK(h.preview.preview_text() == old_drain(slice(blocks, 0, 130)));
    CHECK(h.updates.empty());
    CHECK(h.preview.finalize() == std::optional<std::string>(old_drain(blocks)));
    h.ui.run_all();
    CHECK(h.updates.empty());
}

TEST_CASE("test_next_recording_starts_right_after_a_fast_stop_and_drops_the_old_tail",
          "[test_preview_lazy_tail]") {
    // from
    // tests/test_preview_lazy_tail.py::test_next_recording_starts_right_after_a_fast_stop_and_drops_the_old_tail
    WindowPreviewOptions options;
    options.poll = 30s;
    PreviewHarness h(options);
    h.engine->text = content_text;
    h.start();
    h.feed(noise_blocks(1.5, 1));
    h.preview.stop();
    CHECK_FALSE(h.preview.decoding());

    // No finalize: a non-empty transcript never asks for the old tail.
    h.start();

    CHECK(h.preview.running());
    const auto fresh = noise_blocks(0.5, 2);
    h.feed(fresh);
    h.preview.stop();
    CHECK(h.preview.finalize() == std::optional<std::string>(old_drain(fresh)));
    // Only the new recording's audio was ever decoded.
    CHECK(h.engine->call_lengths() ==
          std::vector<std::size_t>{resampled(static_cast<std::int64_t>(fresh.size()) * kBlock)});
}

TEST_CASE("test_cancel_discards_the_kept_tail_and_late_output_without_waiting",
          "[test_preview_lazy_tail]") {
    // from
    // tests/test_preview_lazy_tail.py::test_cancel_discards_the_kept_tail_and_late_output_without_waiting
    PreviewHarness h;
    h.engine->text = content_text;
    h.engine->stall_on = 1;
    h.start();
    h.feed(noise_blocks(4.0, 4));
    REQUIRE(h.drive([&] { return h.preview.decoding(); }));
    REQUIRE(h.engine->wait_entered());
    h.preview.stop();

    const auto started = std::chrono::steady_clock::now();
    h.preview.cancel();
    CHECK_FALSE(h.preview.finalize().has_value());
    // Neither waited on the stalled engine.
    CHECK(std::chrono::steady_clock::now() - started < 1s);
    CHECK(h.preview.decoding());

    h.engine->release();
    REQUIRE(h.drive([&] { return !h.preview.decoding(); }));
    CHECK(h.preview.preview_text().empty());
    CHECK(h.updates.empty());
    CHECK_FALSE(h.preview.finalize().has_value());
    CHECK(h.engine->calls() == 1);
}

TEST_CASE("test_cancel_wakes_an_idle_worker_so_the_next_preview_can_start",
          "[test_preview_lazy_tail]") {
    // from
    // tests/test_preview_lazy_tail.py::test_cancel_wakes_an_idle_worker_so_the_next_preview_can_start
    WindowPreviewOptions options;
    options.poll = 30s;
    PreviewHarness h(options);
    h.start();
    h.preview.cancel();
    CHECK_FALSE(h.preview.decoding());
    h.start();
    CHECK(h.preview.running());
}

TEST_CASE("test_finalize_deadline_ignores_late_output_and_holds_off_a_replacement",
          "[test_preview_lazy_tail]") {
    // from
    // tests/test_preview_lazy_tail.py::test_finalize_deadline_ignores_late_output_and_holds_off_a_replacement
    PreviewHarness h;
    h.engine->text = content_text;
    h.engine->stall_on = 1;
    h.start();
    h.feed(noise_blocks(4.0, 5));
    REQUIRE(h.drive([&] { return h.preview.decoding(); }));
    REQUIRE(h.engine->wait_entered());
    h.preview.stop();

    CHECK(h.preview.finalize(50ms) == std::optional<std::string>(""));

    // The stalled window still owns the preview's state.
    h.start();
    CHECK(h.preview.decoding());
    CHECK_FALSE(h.preview.running());
    h.engine->release();
    REQUIRE(h.drive([&] { return !h.preview.decoding(); }));
    CHECK(h.preview.preview_text().empty());
    CHECK(h.updates.empty());
    CHECK_FALSE(h.preview.finalize().has_value());
    CHECK(h.engine->calls() == 1);

    h.start();
    CHECK(h.preview.running());
    h.preview.cancel();
}

TEST_CASE("test_cleanup_drops_the_kept_tail", "[test_preview_lazy_tail]") {
    // from tests/test_preview_lazy_tail.py::test_cleanup_drops_the_kept_tail
    PreviewHarness h;
    h.start();
    h.feed(noise_blocks(1.0, 0));
    h.preview.stop();
    h.preview.cleanup();
    CHECK_FALSE(h.preview.finalize().has_value());
    CHECK(h.engine->calls() == 0);
}

TEST_CASE("test_finalize_decodes_once_and_leaves_a_live_preview_alone",
          "[test_preview_lazy_tail]") {
    // from
    // tests/test_preview_lazy_tail.py::test_finalize_decodes_once_and_leaves_a_live_preview_alone
    PreviewHarness h;
    h.engine->text = content_text;
    h.start();
    // Still recording: nothing was stopped, so there is nothing to finish.
    CHECK(h.preview.finalize() == std::optional<std::string>(""));
    const auto blocks = noise_blocks(1.0, 0);
    h.feed(blocks);
    h.preview.stop();
    const auto text = h.preview.finalize();
    CHECK(text == std::optional<std::string>(old_drain(blocks)));
    CHECK(h.preview.finalize() == text);
    CHECK(h.engine->calls() == 1);
}

// --- behaviour without an OpenWhisper test -----------------------------------------------------

TEST_CASE("windows slower than 5 s warn after three, at most every 30 s", "[window_preview]") {
    PreviewHarness h;
    h.engine->on_decode = [&clock = h.clock] {
        clock.advance(6s);
    };
    fv::speech::test::LogCapture log;
    h.start();
    const auto blocks = noise_blocks(5 * 3.02, 9);
    for (std::size_t n = 1; n <= 5; ++n) {
        h.feed(slice(blocks, (n - 1) * kWindowBlocks, n * kWindowBlocks));
        REQUIRE(h.settle(n));
        const auto warnings = [&] {
            std::size_t count = 0;
            const std::string text = log.text();
            for (auto at = text.find("falling behind"); at != std::string::npos;
                 at = text.find("falling behind", at + 1)) {
                ++count;
            }
            return count;
        };
        // Window 3 is the third slow one; windows 4 and 5 come 6 and 12 s later.
        CHECK(warnings() == (n >= 3 ? 1u : 0u));
    }
    CHECK_THAT(log.text(), Catch::Matchers::ContainsSubstring(
                               "WARNING services.streaming_transcriber: Incremental transcription "
                               "falling behind (3+ slow chunks)"));
    CHECK_THAT(log.text(), Catch::Matchers::ContainsSubstring(
                               "INFO services.streaming_transcriber: Incremental transcription "
                               "#1: 3.0s new (+0.0s overlap) -> 6.00s processing (11 chars)"));
}

TEST_CASE("a window job is replaced while queued and never outlives the preview",
          "[window_preview]") {
    PreviewHarness h;
    h.start();
    h.feed(tone_blocks(kWindowBlocks));
    // Hold the speech thread with a Final job, so the preview's window waits in its lane.
    h.engine->stall_on = 1;
    auto blocker = std::make_shared<std::optional<bool>>();
    h.service.submit(fv::speech::test::job(fv::speech::test::clip(std::vector<float>(16000, 0.2f))),
                     h.notify, [blocker](auto outcome) { *blocker = outcome.has_value(); });
    REQUIRE(h.engine->wait_entered());
    REQUIRE(h.drive([&] { return h.preview.decoding(); }));
    // Cancelling drops the queued window (or one still being prepared) while the engine is
    // still held by the Final job.
    h.preview.cancel();
    CHECK(h.drive([&] { return !h.preview.decoding(); }, 3s));
    h.engine->release();
    REQUIRE(fv::speech::test::pump_until(h.notify, [&] { return blocker->has_value(); }));
    CHECK(h.engine->calls() == 1);
    CHECK(h.updates.empty());
}

TEST_CASE("a preview destroyed while finalize waits ends the wait without freeing its state",
          "[window_preview]") {
    PreviewHarness h;
    h.engine->stall_on = 1;
    auto preview = std::make_unique<WindowPreview>(h.service, h.ui, h.clock);
    auto tape = std::make_shared<fv::audio::PcmTape>(kRate);
    preview->start(tape, "job-2", [](std::string, bool) {});
    for (const Block& block : tone_blocks(kWindowBlocks)) {
        tape->append(block);
    }
    const auto deadline = std::chrono::steady_clock::now() + 10s;
    while (!preview->decoding() && std::chrono::steady_clock::now() < deadline) {
        h.ui.advance(100ms);
        h.notify.run_all();
    }
    REQUIRE(preview->decoding());
    REQUIRE(h.engine->wait_entered());
    preview->stop();
    std::optional<std::string> result = std::string("not returned");
    std::atomic<bool> returned{false};
    std::thread waiter([&] {
        result = preview->finalize(10s);
        returned = true;
    });
    // finalize() is waiting for the stalled window.
    std::this_thread::sleep_for(100ms);
    CHECK_FALSE(returned.load());
    const auto started = std::chrono::steady_clock::now();
    preview.reset();
    waiter.join();
    // The destruction took the audio: nullopt, at once rather than at the deadline.
    CHECK(std::chrono::steady_clock::now() - started < 5s);
    CHECK_FALSE(result.has_value());
    h.engine->release();
    REQUIRE(fv::speech::test::pump_until(h.notify, [&] { return h.engine->calls() == 1; }));
}

// --- golden vectors ------------------------------------------------------------------------------

TEST_CASE("append_preview_text replays OpenWhisper's seams", "[golden]") {
    fv::golden::for_each("services.streaming_transcriber", "append_preview_text",
                         [](const fv::golden::Case& c) {
                             CHECK(append_preview_text(c.arg<std::string>(0), c.arg<std::string>(1),
                                                       static_cast<int>(c.arg<std::int64_t>(2))) ==
                                   c.result<std::string>());
                         });
}

TEST_CASE("prepare_preview_audio replays OpenWhisper's resampling", "[golden]") {
    fv::golden::for_each(
        "services.streaming_transcriber", "prepare_preview_audio", [](const fv::golden::Case& c) {
            const auto dtype = c.arg<std::string>(0);
            const auto channels = static_cast<int>(c.arg<std::int64_t>(1));
            const auto rate = static_cast<int>(c.arg<std::int64_t>(2));
            const auto frames = static_cast<std::size_t>(c.arg<std::int64_t>(3));
            const auto seed = static_cast<std::uint64_t>(c.arg<std::int64_t>(4));
            const auto pcm =
                fv::speech::test::lcg_int16(frames * static_cast<std::size_t>(channels), seed);
            std::optional<std::vector<float>> got;
            if (dtype == "float32") {
                std::vector<float> values(pcm.size());
                for (std::size_t i = 0; i < pcm.size(); ++i) {
                    values[i] = static_cast<float>(pcm[i]) / 32768.0f;
                }
                got = fv::speech::prepare_preview_audio(values, channels, rate);
            } else {
                got = fv::speech::prepare_preview_audio(pcm, channels, rate);
            }
            const auto expected = c.result<std::optional<std::vector<double>>>();
            REQUIRE(got.has_value() == expected.has_value());
            if (!expected) {
                return;
            }
            REQUIRE(got->size() == expected->size());
            for (std::size_t i = 0; i < got->size(); ++i) {
                INFO(c.label() << " sample " << i);
                CHECK(fv::golden::near((*expected)[i], (*got)[i], c.float_tolerance()));
            }
        });
}
