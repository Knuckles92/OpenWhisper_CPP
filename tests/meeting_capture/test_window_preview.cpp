// The live caption preview (meeting/asr/preview.py WindowSpeechPreview) and the engine's preview
// lane. From tests/test_meeting_voice_preview.py and tests/test_local_asr.py.

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <expected>
#include <mutex>
#include <numeric>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "asr_test_support.h"
#include "base/testing/fake_clock.h"
#include "capture_test_support.h"
#include "meeting_capture/asr_engine.h"
#include "meeting_capture/window_preview.h"

using Catch::Approx;
using fv::Json;
using fv::meeting::CaptureBlock;
using fv::meeting::PreviewWindows;
using fv::meeting::WindowSpeechPreview;
using fv::meeting::test::SpeechRig;
using fv::meeting::test::TempDir;

namespace {

using namespace std::chrono_literals;

// The Python harness: a decoder that records each window and answers a fixed text, a callback
// that keeps the events, and a pinned clock.
struct Harness {
    explicit Harness(std::function<bool()> busy = [] { return false; })
        : windows(
              [this](std::string_view, std::span<const float> samples,
                     const fv::CancelToken&) -> std::optional<std::string> {
                  calls.emplace_back(samples.begin(), samples.end());
                  return "Assistant, note that.";
              },
              [this](const Json& event) { events.push_back(event); }, std::move(busy), clock) {}

    // block(start): one second of 1000 on `channel`, captured at monotonic 100 + start.
    void accept(double start, double start_s, const std::string& channel = "mic") {
        const std::vector<std::int16_t> frames(16000, 1000);
        windows.accept(channel, frames, 16000, 100 + start, start_s);
    }

    fv::FakeClock clock;
    std::vector<std::vector<float>> calls;
    std::vector<Json> events;
    PreviewWindows windows;
};

CaptureBlock block_of(const std::vector<std::int16_t>& frames, double t_mono,
                      std::string_view channel = "mic") {
    return CaptureBlock{.channel = channel,
                        .frames = frames.data(),
                        .count = frames.size(),
                        .rate = 16000,
                        .t_mono = t_mono};
}

} // namespace

TEST_CASE("one second cadence and bounded audio window", "[test_meeting_voice_preview]") {
    // from tests/test_meeting_voice_preview.py::test_one_second_cadence_and_bounded_audio_window
    Harness harness;
    for (int i = 0; i < 20; ++i) {
        harness.accept(i, i);
        harness.windows.allow_decode_now();
        harness.windows.decode();
    }
    REQUIRE(harness.calls.size() == 20);
    std::size_t longest = 0;
    for (const auto& samples : harness.calls) {
        longest = std::max(longest, samples.size());
    }
    CHECK(longest == 8 * 16000);
    REQUIRE_FALSE(harness.events.empty());
    CHECK(harness.events.back().at("start_s") == 12);
    CHECK(harness.events.back().at("end_s") == 20);
    // The event's keys, in Python's order.
    std::vector<std::string> keys;
    for (const auto& [key, value] : harness.events.back().items()) {
        keys.push_back(key);
    }
    CHECK(keys == std::vector<std::string>{"channel", "text", "start_s", "end_s", "final"});
    CHECK(harness.events.back().at("final") == false);
    CHECK(harness.events.back().at("text") == "Assistant, note that.");
}

TEST_CASE("busy durable asr skips inference and retains latest window",
          "[test_meeting_voice_preview]") {
    // from
    // tests/test_meeting_voice_preview.py::test_busy_durable_asr_skips_inference_and_retains_latest_window
    bool busy = true;
    Harness harness([&] { return busy; });
    for (int i = 0; i < 20; ++i) {
        harness.accept(i, i);
        harness.windows.decode();
    }
    CHECK(harness.calls.empty());
    busy = false;
    harness.windows.decode();
    REQUIRE(harness.calls.size() == 1);
    CHECK(harness.events.at(0).at("end_s") == 20);
}

TEST_CASE("audio gap and pause reset window", "[test_meeting_voice_preview]") {
    // from tests/test_meeting_voice_preview.py::test_audio_gap_and_pause_reset_window
    Harness harness;
    harness.accept(0, 0);
    harness.accept(4, 4);
    harness.windows.decode();
    CHECK(harness.events.back().at("start_s") == 4);
    harness.windows.allow_decode_now();
    // The meeting clock is contiguous; the wall clock has a pause.
    harness.accept(20, 5);
    harness.windows.decode();
    CHECK(harness.events.back().at("start_s") == 5);
}

TEST_CASE("the next decode waits three times the last decode's time", "[asr_preview]") {
    Harness harness;
    PreviewWindows slow(
        [&](std::string_view, std::span<const float>,
            const fv::CancelToken&) -> std::optional<std::string> {
            harness.clock.advance(400ms);
            return "slow";
        },
        [&](const Json& event) { harness.events.push_back(event); }, [] { return false; },
        harness.clock);
    const std::vector<std::int16_t> frames(16000, 1000);
    slow.accept("mic", frames, 16000, 100, 0);
    slow.decode();
    REQUIRE(harness.events.size() == 1);
    slow.accept("mic", frames, 16000, 101, 1);
    harness.clock.advance(1100ms);
    slow.decode(); // 1.1 s after a 0.4 s decode: 1.2 s have not passed
    CHECK(harness.events.size() == 1);
    harness.clock.advance(200ms);
    slow.decode();
    CHECK(harness.events.size() == 2);
}

TEST_CASE("the oldest due channel decodes first and silence is never decoded", "[asr_preview]") {
    Harness harness;
    const std::vector<std::int16_t> silent(16000, 0);
    harness.windows.accept("loopback", silent, 16000, 100, 0);
    harness.accept(0, 0, "mic");
    harness.windows.decode();
    // The silent loopback window was due first: it counts as decoded, without a decode.
    REQUIRE(harness.calls.size() == 1);
    CHECK(harness.events.back().at("channel") == "mic");
    harness.windows.allow_decode_now();
    harness.accept(1, 1, "mic");
    const std::vector<std::int16_t> loud(16000, 1000);
    harness.windows.accept("loopback", loud, 16000, 101, 1);
    harness.windows.decode();
    CHECK(harness.events.back().at("channel") == "loopback");
}

TEST_CASE("preview overflow does not modify capture buffers or block",
          "[test_meeting_voice_preview]") {
    // from
    // tests/test_meeting_voice_preview.py::test_preview_overflow_does_not_modify_capture_buffers_or_block
    fv::FakeClock clock;
    fv::meeting::PreviewOptions options;
    options.queue_blocks = 2;
    options.start_thread = false;
    WindowSpeechPreview preview(
        [](std::string_view, std::span<const float>,
           const fv::CancelToken&) -> std::optional<std::string> { return ""; },
        [](const Json&) {}, [] { return false; }, clock, options);
    const std::vector<std::int16_t> original(16000, 1000);
    for (int i = 0; i < 10; ++i) {
        preview.feed(block_of(original, 100), 0);
    }
    CHECK(preview.queued() == 2);
    std::optional<fv::meeting::PreviewBlock> copied = preview.take();
    REQUIRE(copied);
    CHECK(copied->channel == "mic");
    std::fill(copied->frames.begin(), copied->frames.end(), 0);
    CHECK(std::ranges::all_of(original, [](std::int16_t v) { return v == 1000; }));
}

TEST_CASE("a wide block takes several slots stamped where they start", "[asr_preview]") {
    fv::FakeClock clock;
    fv::meeting::PreviewOptions options;
    options.start_thread = false;
    WindowSpeechPreview preview(
        [](std::string_view, std::span<const float>,
           const fv::CancelToken&) -> std::optional<std::string> { return ""; },
        [](const Json&) {}, [] { return false; }, clock, options);
    const std::vector<std::int16_t> wide(10000, 7);
    preview.feed(block_of(wide, 50.0, "loopback"), 2.0);
    CHECK(preview.queued() == 3);
    const auto first = preview.take();
    const auto second = preview.take();
    const auto third = preview.take();
    REQUIRE((first && second && third));
    CHECK(first->frames.size() == 4096);
    CHECK(second->start_s == Approx(2.0 + 4096.0 / 16000));
    CHECK(second->t_mono == Approx(50.0 + 4096.0 / 16000));
    CHECK(third->frames.size() == 10000 - 2 * 4096);
    CHECK(third->channel == "loopback");
    CHECK_FALSE(preview.take());
}

TEST_CASE("meeting preview copies capture and flushes last audio", "[test_local_asr]") {
    // from tests/test_local_asr.py::test_meeting_preview_copies_capture_and_flushes_last_audio
    // The window preview takes the place of the streaming one: the block is copied before feed()
    // returns, and the tail shorter than a second is never decoded (neither preview of Python's
    // window path flushes at stop).
    std::mutex mutex;
    std::vector<std::vector<float>> received;
    std::atomic<bool> got_result{false};
    WindowSpeechPreview preview(
        [&](std::string_view, std::span<const float> samples,
            const fv::CancelToken&) -> std::optional<std::string> {
            const std::lock_guard lock(mutex);
            received.emplace_back(samples.begin(), samples.end());
            return "preview";
        },
        [&](const Json& event) {
            if (event.at("text") == "preview") {
                got_result = true;
            }
        },
        [] { return false; }, fv::system_clock());
    std::vector<std::int16_t> frames(16000, 3000);
    preview.feed(block_of(frames, 100.0), 10.0);
    std::fill(frames.begin(), frames.end(), 0); // the capture buffer is reused at once
    const auto deadline = std::chrono::steady_clock::now() + 3s;
    while (!got_result && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(5ms);
    }
    REQUIRE(got_result);
    const std::vector<std::int16_t> tail(1600, 3000);
    preview.feed(block_of(tail, 101.0), 11.0);
    CHECK(preview.stop());
    const std::lock_guard lock(mutex);
    REQUIRE(received.size() == 1);
    const double mean =
        std::accumulate(received[0].begin(), received[0].end(), 0.0) / received[0].size();
    CHECK(mean == Approx(3000.0 / 32768).margin(1e-4));
}

TEST_CASE("stop cancels the running decode and ends the thread", "[asr_preview]") {
    std::atomic<bool> entered{false};
    std::atomic<bool> saw_cancel{false};
    WindowSpeechPreview preview(
        [&](std::string_view, std::span<const float>,
            const fv::CancelToken& stop) -> std::optional<std::string> {
            entered = true;
            saw_cancel = stop.wait_for(5s);
            return std::nullopt;
        },
        [](const Json&) {}, [] { return false; }, fv::system_clock());
    const std::vector<std::int16_t> frames(16000, 3000);
    preview.feed(block_of(frames, 100.0), 0.0);
    const auto deadline = std::chrono::steady_clock::now() + 3s;
    while (!entered && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(5ms);
    }
    REQUIRE(entered);
    CHECK(preview.stop());
    CHECK(saw_cancel);
    preview.feed(block_of(frames, 101.0), 1.0); // dropped after stop
    CHECK(preview.queued() == 0);
}

TEST_CASE("a decode that throws ends the preview and is logged", "[asr_preview]") {
    const fv::meeting::test::LogCapture logs;
    std::atomic<int> calls{0};
    WindowSpeechPreview preview(
        [&](std::string_view, std::span<const float>,
            const fv::CancelToken&) -> std::optional<std::string> {
            ++calls;
            throw std::runtime_error("worker died");
        },
        [](const Json&) {}, [] { return false; }, fv::system_clock());
    const std::vector<std::int16_t> frames(16000, 3000);
    preview.feed(block_of(frames, 100.0), 0.0);
    const auto deadline = std::chrono::steady_clock::now() + 3s;
    while (logs.levels("Window speech preview stopped; durable recording continues").empty() &&
           std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(5ms);
    }
    CHECK(logs.levels("Window speech preview stopped; durable recording continues") ==
          std::vector{fv::log::Level::Error});
    CHECK(calls.load() == 1);
    CHECK(preview.stop());
}

TEST_CASE("backend preview never waits for model and rechecks durable priority",
          "[test_meeting_voice_preview]") {
    // from
    // tests/test_meeting_voice_preview.py::test_backend_preview_never_waits_for_model_and_rechecks_durable_priority
    // With one speech thread, "never waits for the model" becomes: a preview is skipped while a
    // durable chunk is outstanding, and preview jobs are coalesced per channel on the Preview
    // lane, below the Meeting lane.
    TempDir folder;
    SpeechRig speech;
    speech.select();
    auto repository = std::make_shared<fv::meeting::test::RecordingRepository>();
    fv::meeting::MeetingAsrEngine engine(speech.service, repository, "m_test", "auto",
                                         fv::system_clock());
    REQUIRE(engine.load_backend({}));
    std::mutex mutex;
    std::vector<Json> events;
    engine.start_preview([&](const Json& event) {
        const std::lock_guard lock(mutex);
        events.push_back(event);
    });
    speech.script->set_fallback({{{"spoken", 0.0, 0.5}}, std::nullopt});
    speech.script->hold();
    engine.start([&](const fv::meeting_state::SpooledChunk& chunk,
                     std::vector<fv::meeting_state::TranscriptSegment>) {
        repository->set_chunk_status(chunk.chunk_id, "done", std::nullopt);
    });
    REQUIRE(engine.enqueue(fv::meeting::test::register_chunk(*repository, folder.path(), 1, 0.0)));
    REQUIRE(speech.script->wait_entered());
    const std::vector<std::int16_t> frames(16000, 3000);
    for (int i = 0; i < 3; ++i) {
        engine.feed_preview(block_of(frames, 100.0 + i), i);
    }
    std::this_thread::sleep_for(300ms);
    CHECK(speech.script->calls_on(fv::speech::Lane::Preview).empty());
    speech.script->resume();
    REQUIRE(engine.drain(5s));
    engine.feed_preview(block_of(frames, 103.0), 3.0);
    REQUIRE(speech.script->wait_calls(2));
    const auto previews = speech.script->calls_on(fv::speech::Lane::Preview);
    REQUIRE(previews.size() == 1);
    CHECK(previews[0].language == "auto");
    CHECK(previews[0].slot == fv::speech::Slot::Meeting);
    CHECK(previews[0].samples.size() == 4 * 16000);
    const auto deadline = std::chrono::steady_clock::now() + 3s;
    for (;;) {
        {
            const std::lock_guard lock(mutex);
            if (!events.empty() || std::chrono::steady_clock::now() > deadline) {
                break;
            }
        }
        std::this_thread::sleep_for(5ms);
    }
    engine.stop_preview();
    const std::lock_guard lock(mutex);
    REQUIRE(events.size() == 1);
    CHECK(events[0].at("channel") == "mic");
    CHECK(events[0].at("text") == "spoken");
    CHECK(events[0].at("start_s") == 0);
    CHECK(events[0].at("end_s") == 4);
}

TEST_CASE("a channel's preview jobs replace each other in the Preview lane", "[asr_preview]") {
    // The speech thread is held by another job, so the preview's job and one the test submits
    // with the key "meeting-preview:mic" wait together: the newer replaces the older, whichever
    // came first, and exactly one of them is decoded.
    SpeechRig speech;
    speech.select();
    auto repository = std::make_shared<fv::meeting::test::RecordingRepository>();
    fv::meeting::MeetingAsrEngine engine(speech.service, repository, "m_test", "auto",
                                         fv::system_clock());
    REQUIRE(engine.load_backend({}));
    std::atomic<int> events{0};
    engine.start_preview([&](const Json&) { ++events; });
    speech.script->set_fallback({{{"text", 0.0, 0.5}}, std::nullopt});
    speech.script->hold();
    fv::speech::DecodeJob blocker;
    blocker.input = fv::speech::AudioClip{std::make_shared<const std::vector<float>>(1600, 0.1f), 0,
                                          1600, 16000, 0.0};
    blocker.options.language = "en";
    speech.service.submit(std::move(blocker), speech.notify, [](auto) {});
    REQUIRE(speech.script->wait_entered());
    const std::vector<std::int16_t> frames(16000, 3000);
    engine.feed_preview(block_of(frames, 100.0), 0.0);
    std::this_thread::sleep_for(1s); // the preview thread submits its job meanwhile
    fv::speech::DecodeJob mine;
    mine.input = fv::speech::AudioClip{std::make_shared<const std::vector<float>>(1600, 0.1f), 0,
                                       1600, 16000, 0.0};
    mine.options.language = "auto";
    mine.lane = fv::speech::Lane::Preview;
    mine.slot = fv::speech::Slot::Meeting;
    mine.coalesce_key = "meeting-preview:mic";
    std::optional<bool> mine_decoded;
    speech.service.submit(std::move(mine), speech.notify,
                          [&](std::expected<fv::speech::DecodeResult, fv::EngineError> result) {
                              mine_decoded = result.has_value();
                          });
    speech.script->resume();
    const auto deadline = std::chrono::steady_clock::now() + 5s;
    while (!mine_decoded && std::chrono::steady_clock::now() < deadline) {
        speech.notify.run_all();
        std::this_thread::sleep_for(5ms);
    }
    REQUIRE(mine_decoded.has_value());
    std::this_thread::sleep_for(300ms);
    engine.stop_preview();
    CHECK(speech.script->calls_on(fv::speech::Lane::Preview).size() == 1);
    CHECK(*mine_decoded != (events.load() == 1));
    CHECK(events.load() <= 1);
}

TEST_CASE("the engine has no preview before it is available or for a file backend",
          "[asr_preview]") {
    SpeechRig speech;
    speech.script->set_accepts_files(true);
    speech.select();
    auto repository = std::make_shared<fv::meeting::test::RecordingRepository>();
    fv::meeting::MeetingAsrEngine engine(speech.service, repository, "m_test", "auto",
                                         fv::system_clock());
    std::atomic<int> events{0};
    engine.start_preview([&](const Json&) { ++events; });
    REQUIRE(engine.load_backend({}));
    engine.start_preview([&](const Json&) { ++events; });
    const std::vector<std::int16_t> frames(16000, 3000);
    engine.feed_preview(block_of(frames, 100.0), 0.0); // no preview: nothing happens
    engine.stop_preview();
    CHECK(speech.script->calls().empty());
}

TEST_CASE("the capture's preview feed stamps blocks with meeting time", "[asr_preview]") {
    fv::FakeClock fake;
    fv::meeting::test::set_steady(fake, 50.0);
    fv::meeting::MeetingClock clock(fake);
    clock.start();
    fake.advance(2s);
    TempDir folder;
    SpeechRig speech;
    speech.select();
    auto repository = std::make_shared<fv::meeting::test::RecordingRepository>();
    fv::meeting::MeetingAsrEngine engine(speech.service, repository, "m_test", "auto", fake);
    REQUIRE(engine.load_backend({}));
    std::mutex mutex;
    std::vector<Json> events;
    engine.start_preview([&](const Json& event) {
        const std::lock_guard lock(mutex);
        events.push_back(event);
    });
    speech.script->set_fallback({{{"hi", 0.0, 0.5}}, std::nullopt});
    const auto feed = fv::meeting::make_preview_feed(engine, clock);
    const std::vector<std::int16_t> frames(16000, 3000);
    feed(block_of(frames, 51.0)); // one second after the meeting started
    REQUIRE(speech.script->wait_calls(1));
    const auto deadline = std::chrono::steady_clock::now() + 3s;
    for (;;) {
        {
            const std::lock_guard lock(mutex);
            if (!events.empty() || std::chrono::steady_clock::now() > deadline) {
                break;
            }
        }
        std::this_thread::sleep_for(5ms);
    }
    engine.stop_preview();
    const std::lock_guard lock(mutex);
    REQUIRE(events.size() == 1);
    CHECK(events[0].at("start_s") == 1);
    CHECK(events[0].at("end_s") == 2);
}
