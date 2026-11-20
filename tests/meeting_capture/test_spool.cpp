// Ported from OpenWhisper tests/test_meeting_spool.py: gap fill (over 120 ms), quiet and hard
// cuts, and the SpoolWriter timeline (drift, overlap trimming, pause, registration order, the
// session WAV, rate changes, bounded gap fills).

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <condition_variable>
#include <filesystem>
#include <fstream>
#include <future>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "base/testing/fake_clock.h"
#include "base/utf8.h"
#include "capture_test_support.h"
#include "meeting_capture/detail/wav_file.h"
#include "meeting_capture/meeting_clock.h"
#include "meeting_capture/session_audio.h"
#include "meeting_capture/spool.h"
#include "meeting_state/testing/fake_meeting_repository.h"
#include "testkit/testkit.h"

using Catch::Approx;
using fv::Json;
using fv::meeting::CaptureBlock;
using fv::meeting::DEFAULT_MAX_SEC;
using fv::meeting::DEFAULT_TARGET_SEC;
using fv::meeting::find_cut_point;
using fv::meeting::gap_fill_frames;
using fv::meeting::GAP_TOLERANCE_S;
using fv::meeting::MeetingClock;
using fv::meeting::QUIET_WINDOW_S;
using fv::meeting::SpoolOptions;
using fv::meeting::SpoolWriter;
using fv::meeting::TARGET_RATE;
using fv::meeting::test::TempDir;
using fv::meeting::test::tone;
using fv::meeting_state::SpooledChunk;
using fv::meeting_state::testing::FakeMeetingRepository;

namespace {

constexpr std::size_t BLOCK = 1024;

// TestFindCutPoint._quiet_buffer: loud, then an optional quiet tail.
std::vector<std::int16_t> quiet_buffer(double duration_s, std::optional<double> quiet_from_s,
                                       std::int16_t loud = 5000, int rate = TARGET_RATE) {
    std::vector<std::int16_t> buffer(static_cast<std::size_t>(duration_s * rate), loud);
    if (quiet_from_s) {
        const auto q0 = static_cast<std::size_t>(*quiet_from_s * rate);
        std::fill(buffer.begin() + static_cast<std::ptrdiff_t>(q0), buffer.end(), 0);
    }
    return buffer;
}

// A started meeting clock, a repository and a collector, the way the Python tests build them.
struct Rig {
    Rig() {
        fv::meeting::test::set_steady(fake, 5000.0);
        clock.start();
        t0 = clock.monotonic();
    }

    std::unique_ptr<SpoolWriter> writer(SpoolOptions options = {}) {
        return std::make_unique<SpoolWriter>(
            "m_test", "mic", folder.path(), clock, repository,
            [this](const SpooledChunk& chunk) {
                const std::lock_guard lock(mutex);
                // Registration precedes on_chunk: the row is already there, pending.
                bool registered = false;
                for (const Json& row : repository->get_audio_chunks("m_test")) {
                    if (row.at("id") == chunk.chunk_id && row.at("asr_status") == "pending") {
                        registered = true;
                    }
                }
                events.emplace_back(registered ? "register" : "missing");
                events.emplace_back("on_chunk");
                chunks.push_back(chunk);
            },
            std::move(options));
    }

    std::vector<SpooledChunk> collected() {
        const std::lock_guard lock(mutex);
        return chunks;
    }

    TempDir folder;
    fv::FakeClock fake;
    MeetingClock clock{fake};
    double t0 = 0.0;
    std::shared_ptr<FakeMeetingRepository> repository =
        std::make_shared<FakeMeetingRepository>(fake);
    std::mutex mutex;
    std::vector<SpooledChunk> chunks;
    std::vector<std::string> events;
};

// _feed_stream: 1024-frame blocks stamped in real time; a stall delivers burst_blocks blocks all
// stamped at the stall's end. Returns the sample index past the last block fed.
std::size_t feed_stream(SpoolWriter& writer, const std::vector<std::int16_t>& signal, int rate,
                        double t0, std::size_t start_index = 0, double stall_s = 0.0,
                        int burst_blocks = 0) {
    std::size_t idx = 0;
    std::optional<std::size_t> burst_at;
    if (burst_blocks > 0) {
        burst_at = static_cast<std::size_t>(stall_s * rate);
    }
    while (idx + BLOCK <= signal.size()) {
        const std::size_t abs_index = start_index + idx;
        double t_mono = 0.0;
        if (burst_at && abs_index >= *burst_at) {
            t_mono = t0 + static_cast<double>(*burst_at) / rate;
            if (--burst_blocks <= 0) {
                burst_at.reset();
            }
        } else {
            t_mono = t0 + static_cast<double>(abs_index) / rate;
        }
        fv::meeting::test::feed_waiting(
            writer, fv::meeting::test::block_of(signal, idx, BLOCK, rate, t_mono));
        idx += BLOCK;
    }
    return idx;
}

std::vector<std::int16_t> wav_samples(const std::filesystem::path& path) {
    const auto [samples, rate] = fv::meeting::detail::load_wav_int16(path);
    return samples;
}

bool has_suffix(const std::vector<std::string>& names, std::string_view suffix) {
    return std::any_of(names.begin(), names.end(),
                       [&](const std::string& name) { return name.ends_with(suffix); });
}

} // namespace

// --- TestGapFill -------------------------------------------------------------------------------

// from tests/test_meeting_spool.py::TestGapFill::test_no_fill_when_expected_is_none
TEST_CASE("TestGapFill no fill when expected is none", "[test_meeting_spool]") {
    CHECK(gap_fill_frames(std::nullopt, 1.0).empty());
}

// from tests/test_meeting_spool.py::TestGapFill::test_no_fill_within_tolerance
TEST_CASE("TestGapFill no fill within tolerance", "[test_meeting_spool]") {
    // Gaps at or below 120 ms are absorbed as timing jitter.
    CHECK(gap_fill_frames(0.0, GAP_TOLERANCE_S).empty());
    CHECK(gap_fill_frames(0.0, 0.10, TARGET_RATE).empty());
    CHECK(gap_fill_frames(2.5, 2.5 + 0.05).empty());
}

// from tests/test_meeting_spool.py::TestGapFill::test_fills_gap_above_120ms
TEST_CASE("TestGapFill fills gap above 120ms", "[test_meeting_spool]") {
    const auto fill = gap_fill_frames(5.0, 5.0 + 0.25, TARGET_RATE);
    CHECK(std::all_of(fill.begin(), fill.end(), [](std::int16_t v) { return v == 0; }));
    CHECK(fill.size() == static_cast<std::size_t>(std::lround(0.25 * TARGET_RATE)));
}

// from tests/test_meeting_spool.py::TestGapFill::test_gap_just_over_tolerance
TEST_CASE("TestGapFill gap just over tolerance", "[test_meeting_spool]") {
    const double gap_s = 0.121;
    const auto fill = gap_fill_frames(0.0, gap_s);
    CHECK(fill.size() == static_cast<std::size_t>(std::lround(gap_s * TARGET_RATE)));
    CHECK_FALSE(fill.empty());
}

// --- TestFindCutPoint --------------------------------------------------------------------------

// from tests/test_meeting_spool.py::TestFindCutPoint::test_no_cut_before_target
TEST_CASE("TestFindCutPoint no cut before target", "[test_meeting_spool]") {
    const auto buffer = quiet_buffer(4.9, 3.0);
    CHECK_FALSE(find_cut_point(buffer, TARGET_RATE, DEFAULT_TARGET_SEC, DEFAULT_MAX_SEC));
}

// from tests/test_meeting_spool.py::TestFindCutPoint::test_quiet_cut_after_target_at_400ms
TEST_CASE("TestFindCutPoint quiet cut after target at 400ms", "[test_meeting_spool]") {
    // Loud just past the live target, then silence: the cut is at the quiet window's end.
    const double quiet_from_s = DEFAULT_TARGET_SEC + 0.1;
    const auto buffer = quiet_buffer(7.0, quiet_from_s);
    const auto cut = find_cut_point(buffer, TARGET_RATE, DEFAULT_TARGET_SEC, DEFAULT_MAX_SEC);
    REQUIRE(cut);
    const auto expected = static_cast<std::int64_t>(quiet_from_s * TARGET_RATE) +
                          static_cast<std::int64_t>(QUIET_WINDOW_S * TARGET_RATE);
    CHECK(std::llabs(*cut - expected) <= static_cast<std::int64_t>(0.05 * TARGET_RATE) + 1);
    CHECK(static_cast<double>(*cut) / TARGET_RATE >= DEFAULT_TARGET_SEC + QUIET_WINDOW_S - 0.15);
}

// from tests/test_meeting_spool.py::TestFindCutPoint::test_hard_cut_at_max_without_quiet
TEST_CASE("TestFindCutPoint hard cut at max without quiet", "[test_meeting_spool]") {
    const auto buffer = quiet_buffer(DEFAULT_MAX_SEC + 1.0, std::nullopt);
    CHECK(find_cut_point(buffer, TARGET_RATE, DEFAULT_TARGET_SEC, DEFAULT_MAX_SEC) ==
          static_cast<std::int64_t>(DEFAULT_MAX_SEC * TARGET_RATE));
}

// from tests/test_meeting_spool.py::TestFindCutPoint::test_no_hard_cut_until_max
TEST_CASE("TestFindCutPoint no hard cut until max", "[test_meeting_spool]") {
    // Between the target and the maximum with no quiet: wait for more audio.
    const auto buffer = quiet_buffer((DEFAULT_TARGET_SEC + DEFAULT_MAX_SEC) / 2.0, std::nullopt);
    CHECK_FALSE(find_cut_point(buffer, TARGET_RATE, DEFAULT_TARGET_SEC, DEFAULT_MAX_SEC));
}

// from tests/test_meeting_spool.py::TestFindCutPoint::test_native_rate_cut_matches_16k_cut
TEST_CASE("TestFindCutPoint native rate cut matches 16k cut", "[test_meeting_spool]") {
    // The writer scans at the device's native rate; the policy must not shift at 44.1 kHz.
    const double quiet_from_s = DEFAULT_TARGET_SEC + 0.1;
    const auto buffer = quiet_buffer(7.0, quiet_from_s, 5000, 44100);
    const auto cut = find_cut_point(buffer, 44100, DEFAULT_TARGET_SEC, DEFAULT_MAX_SEC);
    REQUIRE(cut);
    CHECK(std::abs(static_cast<double>(*cut) / 44100 - (quiet_from_s + QUIET_WINDOW_S)) <= 0.06);
}

// --- SpoolWriter -------------------------------------------------------------------------------

// from
// tests/test_meeting_spool.py::test_queue_overflow_marks_audio_incomplete_and_keeps_accepted_blocks
TEST_CASE("queue overflow marks audio incomplete and keeps accepted blocks",
          "[test_meeting_spool]") {
    Rig rig;
    std::promise<void> entered;
    std::shared_future<void> release_signal;
    std::promise<void> release;
    release_signal = release.get_future().share();
    std::once_flag entered_once;
    SpoolOptions options;
    options.queue_size = 1;
    options.before_block = [&] {
        std::call_once(entered_once, [&] { entered.set_value(); });
        REQUIRE(release_signal.wait_for(std::chrono::seconds(3)) == std::future_status::ready);
    };
    auto writer = rig.writer(options);
    const std::vector<std::int16_t> samples(1600, 2000);
    const CaptureBlock block =
        fv::meeting::test::block_of(samples, 0, samples.size(), 16000, rig.t0);
    writer->feed(block);
    REQUIRE(entered.get_future().wait_for(std::chrono::seconds(3)) == std::future_status::ready);
    writer->feed(block);
    writer->feed(block);
    CHECK(writer->dropped_blocks() == 1);
    REQUIRE(writer->health_error());
    CHECK(writer->health_error()->find("dropped") != std::string::npos);
    release.set_value();
    writer->flush();
    CHECK(std::filesystem::exists(rig.folder.path() / "mic_session.wav"));
}

// from tests/test_meeting_spool.py::test_chunk_write_failure_marks_audio_incomplete
TEST_CASE("chunk write failure marks audio incomplete", "[test_meeting_spool]") {
    Rig rig;
    // A folder where the first chunk's WAV goes: replacing it fails as a full disk would.
    std::filesystem::create_directories(rig.folder.path() / "mic_00000.wav");
    auto writer = rig.writer();
    feed_stream(*writer, tone(1.0, 16000), 16000, rig.t0);
    writer->flush();
    REQUIRE(writer->health_error());
    CHECK(writer->health_error()->find("could not be written") != std::string::npos);
    CHECK(rig.collected().empty());
    CHECK_FALSE(has_suffix(rig.folder.names(), ".tmp"));
}

// from tests/test_meeting_spool.py::TestSpoolWriterTimeline::test_no_drift_over_60s_at_44100
TEST_CASE("TestSpoolWriterTimeline no drift over 60s at 44100", "[test_meeting_spool]") {
    // Per-block round(n * 16000 / 44100) gives 372 samples where 371.5193 is exact: 0.9 ms/s
    // fast, 54 ms after a minute, 4.7 s after an hour.
    const int rate = 44100;
    Rig rig;
    auto writer = rig.writer();
    const auto signal = tone(60.0, rate, 7.0);
    const std::size_t fed = feed_stream(*writer, signal, rate, rig.t0);
    writer->flush();
    const auto chunks = rig.collected();
    REQUIRE(chunks.size() >= 3);
    CHECK(chunks.front().start_s == Approx(0.0).margin(0.02));
    for (std::size_t i = 1; i < chunks.size(); ++i) {
        CHECK(chunks[i].start_s ==
              Approx(chunks[i - 1].start_s + chunks[i - 1].duration_s).margin(0.005));
    }
    const double fed_s = static_cast<double>(fed) / rate;
    CHECK(chunks.back().start_s + chunks.back().duration_s == Approx(fed_s).margin(0.02));
}

// from
// tests/test_meeting_spool.py::TestSpoolWriterTimeline::test_negative_gap_does_not_shift_the_channel
TEST_CASE("TestSpoolWriterTimeline negative gap does not shift the channel",
          "[test_meeting_spool]") {
    const int rate = 48000;
    Rig rig;
    auto writer = rig.writer();
    // 5 s clean, a 0.4 s stall whose buffered audio arrives as 20 same-stamped blocks, then
    // clean again.
    const auto signal = tone(30.0, rate, 7.0);
    feed_stream(*writer, signal, rate, rig.t0, 0, 5.0, 20);
    writer->flush();
    const auto chunks = rig.collected();
    REQUIRE_FALSE(chunks.empty());
    const double end_s = chunks.back().start_s + chunks.back().duration_s;
    const double fed_s = static_cast<double>((signal.size() / BLOCK) * BLOCK) / rate;
    // Without overlap trimming the burst adds about 0.43 s that never comes back.
    CHECK(end_s == Approx(fed_s).margin(GAP_TOLERANCE_S + 0.03));
}

// from tests/test_meeting_spool.py::TestSpoolWriterTimeline::test_positive_gap_is_zero_filled
TEST_CASE("TestSpoolWriterTimeline positive gap is zero filled", "[test_meeting_spool]") {
    const int rate = 16000;
    Rig rig;
    auto writer = rig.writer();
    feed_stream(*writer, tone(1.0, rate), rate, rig.t0);
    // Resumes 0.5 s after the stream ended: a real dropout.
    feed_stream(*writer, tone(1.0, rate), rate, rig.t0, static_cast<std::size_t>(1.5 * rate));
    writer->flush();
    const auto chunks = rig.collected();
    REQUIRE(chunks.size() == 1);
    const SpooledChunk& chunk = chunks.front();
    CHECK(chunk.start_s == Approx(0.0).margin(0.02));
    const auto data = wav_samples(fv::path_from_utf8(chunk.file_path));
    // 1 s of audio, 0.5 s of fill, 1 s of audio, block-quantized.
    CHECK(chunk.duration_s == Approx(2.5).margin(0.08));
    CHECK(data.size() == static_cast<std::size_t>(std::lround(chunk.duration_s * TARGET_RATE)));
    const auto begin = static_cast<std::size_t>(1.05 * rate);
    const auto end = static_cast<std::size_t>(1.45 * rate);
    CHECK(std::all_of(data.begin() + static_cast<std::ptrdiff_t>(begin),
                      data.begin() + static_cast<std::ptrdiff_t>(end),
                      [](std::int16_t v) { return v == 0; }));
    int loudest = 0;
    for (std::size_t i = static_cast<std::size_t>(1.6 * rate);
         i < static_cast<std::size_t>(2.4 * rate); ++i) {
        loudest = std::max(loudest, std::abs(static_cast<int>(data[i])));
    }
    CHECK(loudest > 1000);
}

// from
// tests/test_meeting_spool.py::TestSpoolWriterTimeline::test_blocks_fed_while_paused_are_dropped
TEST_CASE("TestSpoolWriterTimeline blocks fed while paused are dropped", "[test_meeting_spool]") {
    const int rate = 16000;
    Rig rig;
    auto writer = rig.writer();
    rig.clock.pause();
    feed_stream(*writer, tone(3.0, rate), rate, rig.t0);
    writer->flush();
    CHECK(rig.collected().empty());
    CHECK(rig.repository->get_audio_chunks("m_test").empty());
    CHECK(rig.folder.names().empty());
}

// --- TestSpoolWriterContract -------------------------------------------------------------------

// from
// tests/test_meeting_spool.py::TestSpoolWriterContract::test_registered_pending_before_on_chunk
TEST_CASE("TestSpoolWriterContract registered pending before on chunk", "[test_meeting_spool]") {
    const int rate = 16000;
    Rig rig;
    auto writer = rig.writer();
    feed_stream(*writer, tone(2.0, rate), rate, rig.t0);
    writer->flush();
    const auto chunks = rig.collected();
    REQUIRE(chunks.size() == 1);
    CHECK(rig.events == std::vector<std::string>{"register", "on_chunk"});
    const auto rows = rig.repository->get_audio_chunks("m_test");
    REQUIRE(rows.size() == 1);
    CHECK(rows[0].at("asr_status") == "pending");
    CHECK(rows[0].at("sample_rate") == TARGET_RATE);
    CHECK(rows[0].at("seq") == 0);
    // Atomic write: neither the temporary file nor the registration marker survives.
    const auto names = rig.folder.names();
    CHECK_FALSE(has_suffix(names, ".tmp"));
    CHECK_FALSE(has_suffix(names, ".recovery.json"));
    const auto path = fv::path_from_utf8(chunks[0].file_path);
    CHECK(path.filename() == "mic_00000.wav");
    const auto wav = fv::meeting::detail::read_wav(path);
    CHECK(wav.rate == TARGET_RATE);
    CHECK(wav.channels == 1);
    CHECK(wav.sample_width == 2);
}

// from tests/test_meeting_spool.py::TestSpoolWriterContract::test_short_remainder_is_kept
TEST_CASE("TestSpoolWriterContract short remainder is kept", "[test_meeting_spool]") {
    // 0.5 s of final speech survives the flush (MIN_FLUSH_S = 0.25).
    const int rate = 16000;
    Rig rig;
    auto writer = rig.writer();
    feed_stream(*writer, tone(0.5, rate), rate, rig.t0);
    const auto result = writer->flush();
    CHECK(result);
    const auto chunks = rig.collected();
    REQUIRE(chunks.size() == 1);
    CHECK(chunks[0].duration_s == Approx(0.46).margin(0.05));
}

// from tests/test_meeting_spool.py::TestSpoolWriterContract::test_flush_is_idempotent
TEST_CASE("TestSpoolWriterContract flush is idempotent", "[test_meeting_spool]") {
    const int rate = 16000;
    Rig rig;
    auto writer = rig.writer();
    feed_stream(*writer, tone(1.0, rate), rate, rig.t0);
    const auto first = writer->flush();
    const auto second = writer->flush();
    CHECK(first == second);
    CHECK(rig.collected().size() == 1);
    // Feeding after flush is harmless.
    const auto late = tone(0.1, rate);
    writer->feed(fv::meeting::test::block_of(late, 0, late.size(), rate, rig.t0 + 5.0));
    CHECK(rig.collected().size() == 1);
}

// from
// tests/test_meeting_spool.py::TestSpoolWriterContract::test_registration_failure_orphans_the_wav
TEST_CASE("TestSpoolWriterContract registration failure orphans the wav", "[test_meeting_spool]") {
    const int rate = 16000;
    Rig rig;
    rig.repository->fail_register_chunk(2); // both the attempt and its retry
    auto writer = rig.writer();
    feed_stream(*writer, tone(1.0, rate), rate, rig.t0);
    writer->flush();
    CHECK(rig.collected().empty());
    const auto names = rig.folder.names();
    CHECK(std::find(names.begin(), names.end(), "mic_00000.wav.orphan") != names.end());
    const auto orphan = rig.folder.path() / "mic_00000.wav.orphan";
    CHECK(std::filesystem::is_regular_file(fv::meeting::chunk_recovery_meta_path(orphan)));
    const auto meta = fv::meeting::load_chunk_recovery_meta(orphan);
    REQUIRE(meta);
    CHECK(meta->meeting_id == "m_test");
    CHECK(meta->channel == "mic");
    CHECK(meta->seq == 0);
    CHECK(std::any_of(names.begin(), names.end(), [](const std::string& name) {
        return name.starts_with("mic_session") || name.ends_with(".16k.pcm");
    }));
}

// --- TestSessionWav ----------------------------------------------------------------------------

// from tests/test_meeting_spool.py::TestSessionWav::test_flush_writes_16k_session_wav
TEST_CASE("TestSessionWav flush writes 16k session wav", "[test_meeting_spool]") {
    const int rate = 16000;
    Rig rig;
    auto writer = rig.writer();
    feed_stream(*writer, tone(2.0, rate), rate, rig.t0);
    writer->flush();
    const auto wav_path = rig.folder.path() / "mic_session.wav";
    CHECK(std::filesystem::is_regular_file(wav_path));
    CHECK(std::filesystem::is_regular_file(rig.folder.path() / "mic_session.json"));
    const auto wav = fv::meeting::detail::read_wav(wav_path);
    CHECK(wav.rate == TARGET_RATE);
    CHECK(wav.channels == 1);
    const auto frames = static_cast<double>(wav.frames.size() / 2);
    CHECK(frames == Approx(2.0 * TARGET_RATE).margin(static_cast<double>(BLOCK)));
}

// from tests/test_meeting_spool.py::TestSessionWav::test_pause_does_not_create_session_files
TEST_CASE("TestSessionWav pause does not create session files", "[test_meeting_spool]") {
    const int rate = 16000;
    Rig rig;
    auto writer = rig.writer();
    rig.clock.pause();
    feed_stream(*writer, tone(3.0, rate), rate, rig.t0);
    writer->flush();
    CHECK(rig.folder.names().empty());
}

// from tests/test_meeting_spool.py::TestSessionWav::test_native_rate_session_matches_timeline
TEST_CASE("TestSessionWav native rate session matches timeline", "[test_meeting_spool]") {
    const int rate = 44100;
    Rig rig;
    auto writer = rig.writer();
    feed_stream(*writer, tone(3.0, rate), rate, rig.t0);
    writer->flush();
    const auto wav = fv::meeting::detail::read_wav(rig.folder.path() / "mic_session.wav");
    const double duration = static_cast<double>(wav.frames.size() / 2) / wav.rate;
    const double fed_s =
        static_cast<double>((static_cast<std::size_t>(3.0 * rate) / BLOCK) * BLOCK) / rate;
    CHECK(duration == Approx(fed_s).margin(0.05));
}

// from tests/test_meeting_spool.py::TestSessionWav::test_chunk_concat_fallback
TEST_CASE("TestSessionWav chunk concat fallback", "[test_meeting_spool]") {
    TempDir folder;
    const auto write = [&](const std::string& name, const std::vector<std::int16_t>& samples) {
        const auto path = folder.path() / name;
        fv::meeting::detail::WavOut wav(path, TARGET_RATE);
        wav.write(std::span<const std::int16_t>(samples));
        wav.close();
        return fv::path_to_utf8(path);
    };
    const auto a = write("loopback_00000.wav", tone(0.5, TARGET_RATE));
    const auto b = write("loopback_00001.wav", tone(0.5, TARGET_RATE));
    const auto out = folder.path() / "loopback_session.wav";
    const std::vector<Json> chunks = {
        Json{{"channel", "loopback"},
             {"seq", 0},
             {"file_path", a},
             {"start_s", 0.0},
             {"duration_s", 0.5}},
        Json{{"channel", "loopback"},
             {"seq", 1},
             {"file_path", b},
             {"start_s", 0.5},
             {"duration_s", 0.5}},
    };
    const auto origin = fv::meeting::concat_channel_chunks_to_wav(chunks, "loopback", out);
    REQUIRE(origin);
    CHECK(*origin == Approx(0.0));
    const auto wav = fv::meeting::detail::read_wav(out);
    CHECK(static_cast<double>(wav.frames.size() / 2) == Approx(TARGET_RATE).margin(2));
}

// --- TestRateChangeAndLargeGaps ----------------------------------------------------------------

// from
// tests/test_meeting_spool.py::TestRateChangeAndLargeGaps::test_rate_change_preserves_session_origin_and_gap
TEST_CASE("TestRateChangeAndLargeGaps rate change preserves session origin and gap",
          "[test_meeting_spool]") {
    Rig rig;
    auto writer = rig.writer();
    const auto first = tone(0.5, 44100);
    feed_stream(*writer, first, 44100, rig.t0);
    const double first_dur = static_cast<double>((first.size() / BLOCK) * BLOCK) / 44100.0;
    const auto second = tone(0.5, 48000);
    const double resume_t0 = rig.t0 + first_dur + 1.0;
    for (std::size_t idx = 0; idx + BLOCK <= second.size(); idx += BLOCK) {
        fv::meeting::test::feed_waiting(
            *writer, fv::meeting::test::block_of(second, idx, BLOCK, 48000,
                                                 resume_t0 + static_cast<double>(idx) / 48000.0));
    }
    writer->flush();
    const auto meta = fv::meeting::load_session_meta(rig.folder.path() / "mic_session.json");
    REQUIRE(meta);
    CHECK(meta->origin_s == Approx(0.0).margin(0.05));
    const auto wav = fv::meeting::detail::read_wav(rig.folder.path() / "mic_session.wav");
    CHECK(static_cast<double>(wav.frames.size() / 2) / wav.rate == Approx(2.0).margin(0.25));
    // Live chunks may span the silence inside one remainder; the session timeline is the
    // alignment contract for the offline pass.
    const auto chunks = rig.collected();
    REQUIRE_FALSE(chunks.empty());
    double end = 0.0;
    for (const auto& chunk : chunks) {
        end = std::max(end, chunk.start_s + chunk.duration_s);
    }
    CHECK(end == Approx(first_dur + 1.0 + 0.5).margin(0.35));
}

// from
// tests/test_meeting_spool.py::TestRateChangeAndLargeGaps::test_large_gap_fill_uses_bounded_allocations
TEST_CASE("TestRateChangeAndLargeGaps large gap fill uses bounded allocations",
          "[test_meeting_spool]") {
    // OpenWhisper counts numpy's zeros() allocations; here the writer's silence pieces are the
    // same numbers, and the run shows the gap filled.
    const int rate = 16000;
    const double gap_s = 30.0;
    const auto pieces = fv::meeting::gap_fill_pieces(static_cast<std::int64_t>(gap_s * rate), rate);
    REQUIRE_FALSE(pieces.empty());
    const std::int64_t largest = *std::max_element(pieces.begin(), pieces.end());
    CHECK(largest <= static_cast<std::int64_t>(rate * fv::meeting::GAP_FILL_CHUNK_S) + 1);
    CHECK(largest < static_cast<std::int64_t>(gap_s * rate) / 2);

    Rig rig;
    SpoolOptions options;
    options.target_sec = 1.0;
    options.max_sec = 2.0;
    auto writer = rig.writer(options);
    const auto first = tone(0.25, rate);
    feed_stream(*writer, first, rate, rig.t0);
    const double first_dur = static_cast<double>((first.size() / BLOCK) * BLOCK) / rate;
    const auto second = tone(0.25, rate);
    const double resume_t0 = rig.t0 + first_dur + gap_s;
    for (std::size_t idx = 0; idx + BLOCK <= second.size(); idx += BLOCK) {
        fv::meeting::test::feed_waiting(
            *writer, fv::meeting::test::block_of(second, idx, BLOCK, rate,
                                                 resume_t0 + static_cast<double>(idx) / rate));
    }
    writer->flush();
    const auto wav = fv::meeting::detail::read_wav(rig.folder.path() / "mic_session.wav");
    CHECK(static_cast<double>(wav.frames.size() / 2) / rate ==
          Approx(first_dur + gap_s + first_dur).margin(0.01));
}

// --- beyond the Python tests -------------------------------------------------------------------

TEST_CASE("feeding a block allocates nothing", "[test_meeting_spool]") {
    Rig rig;
    auto writer = rig.writer();
    const auto signal = tone(0.5, 48000);
    const CaptureBlock block = fv::meeting::test::block_of(signal, 0, BLOCK, 48000, rig.t0);
    {
        const fv::testkit::RealtimeGuard guard;
        writer->feed(block);
        writer->feed(block);
    }
    writer->flush();
}

TEST_CASE("a block wider than a queue slot is kept whole", "[test_meeting_spool]") {
    const int rate = 16000;
    Rig rig;
    auto writer = rig.writer();
    const auto signal = tone(1.0, rate);
    // 16000 samples in one block: four slots, stamped where each piece starts.
    fv::meeting::test::feed_waiting(
        *writer, fv::meeting::test::block_of(signal, 0, signal.size(), rate, rig.t0));
    writer->flush();
    CHECK(writer->timeline_fixes() == 0);
    const auto session = wav_samples(rig.folder.path() / "mic_session.wav");
    CHECK(session == signal);
}

TEST_CASE("session helpers read back what they write", "[test_meeting_spool]") {
    TempDir folder;
    const auto meta_path = folder.path() / "mic_session.json";
    fv::meeting::write_session_meta(meta_path, 48000, 96000, 1.5, 2.25, 32000, false);
    std::ifstream in(meta_path, std::ios::binary);
    const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    CHECK(
        text ==
        R"({"sample_rate":48000,"sample_count":96000,"origin_s":1.5,"pcm_origin_s":2.25,"prefix_16k_samples":32000,"pcm_active":false})");
    const auto meta = fv::meeting::load_session_meta(meta_path);
    REQUIRE(meta);
    CHECK(meta->sample_rate == 48000);
    CHECK(meta->pcm_origin_s == 2.25);
    CHECK(meta->pcm_active == false);
    // Old files without the newer fields, and files Python would refuse.
    std::ofstream(meta_path, std::ios::binary) << R"({"sample_rate":"16000","origin_s":null})";
    const auto old = fv::meeting::load_session_meta(meta_path);
    REQUIRE(old);
    CHECK(old->sample_rate == 16000);
    CHECK(old->origin_s == 0.0);
    CHECK_FALSE(old->pcm_origin_s);
    std::ofstream(meta_path, std::ios::binary) << R"({"sample_rate":"fast"})";
    CHECK_FALSE(fv::meeting::load_session_meta(meta_path));
    std::ofstream(meta_path, std::ios::binary) << "[1, 2]";
    CHECK_FALSE(fv::meeting::load_session_meta(meta_path));
}

TEST_CASE("a native PCM slice becomes a 16 kHz WAV and the session is found again",
          "[test_meeting_spool]") {
    TempDir folder;
    const auto signal = tone(2.0, 48000);
    const auto pcm = folder.path() / "mic_session.pcm";
    {
        std::ofstream out(pcm, std::ios::binary);
        out.write(reinterpret_cast<const char*>(signal.data()),
                  static_cast<std::streamsize>(signal.size() * 2));
    }
    const auto wav = folder.path() / "slice.wav";
    const auto written = fv::meeting::pcm_slice_to_wav(pcm, 48000, 48000, 96000, wav);
    CHECK(written == 16000);
    const std::vector<std::int16_t> second_half(signal.begin() + 48000, signal.end());
    CHECK(wav_samples(wav) == fv::meeting::resample_to_16k(second_half, 48000));
    CHECK_FALSE(std::filesystem::exists(folder.path() / "slice.wav.tmp"));
    CHECK(fv::meeting::pcm_slice_to_wav(pcm, 48000, 10, 5, wav) == 0);

    // load_channel_session: the session WAV, its rate, and the origin from the session meta.
    fv::meeting::pcm16k_to_wav(pcm, fv::meeting::session_wav_path(folder.path(), "mic"));
    fv::meeting::write_session_meta(fv::meeting::session_meta_path(folder.path(), "mic"), 16000,
                                    96000, 3.5);
    const auto [frames, rate, origin] = fv::meeting::load_channel_session(folder.path(), "mic");
    REQUIRE(frames);
    CHECK(frames->size() == signal.size());
    CHECK(rate == 16000);
    CHECK(origin == 3.5);
    // A channel with no audio and no chunks.
    const auto [none, none_rate, none_origin] =
        fv::meeting::load_channel_session(folder.path(), "loopback");
    CHECK_FALSE(none);
    CHECK(none_rate == 16000);
    CHECK(none_origin == 0.0);
}
