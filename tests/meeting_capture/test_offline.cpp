// The offline re-decode (meeting/asr/offline.py): cut ranges, the overlap drop, stable ids, the
// failed window, mic echo removal and the meeting pass over session WAVs. From
// tests/test_meeting_asr.py and tests/test_meeting_hallucination.py.

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "asr_test_support.h"
#include "capture_test_support.h"
#include "meeting_capture/offline.h"
#include "meeting_capture/segment_ids.h"
#include "meeting_capture/session_audio.h"
#include "meeting_capture/spool.h"

using Catch::Approx;
using fv::Json;
using fv::meeting::OfflineWindowError;
using fv::meeting::TARGET_RATE;
using fv::meeting::test::TempDir;
using fv::meeting::test::write_wav_samples;
using fv::meeting_state::TranscriptSegment;

namespace {

TranscriptSegment row(std::string id, std::string channel, double start, double end,
                      std::string text) {
    TranscriptSegment made;
    made.segment_id = std::move(id);
    made.meeting_id = "m";
    made.channel = std::move(channel);
    made.start_s = start;
    made.end_s = end;
    made.text = std::move(text);
    return made;
}

std::vector<std::string> ids(const std::vector<TranscriptSegment>& segments) {
    std::vector<std::string> out;
    for (const auto& segment : segments) {
        out.push_back(segment.segment_id);
    }
    return out;
}

std::vector<std::string> texts(const std::vector<TranscriptSegment>& segments) {
    std::vector<std::string> out;
    for (const auto& segment : segments) {
        out.push_back(segment.text);
    }
    return out;
}

} // namespace

TEST_CASE("hard cut without audio", "[test_meeting_asr]") {
    // from tests/test_meeting_asr.py::TestOfflineCutRanges::test_hard_cut_without_audio
    const std::int64_t total = 90 * TARGET_RATE;
    fv::meeting::OfflineCutOptions options;
    options.overlap_s = 1.0;
    const auto ranges = fv::meeting::offline_cut_ranges(total, TARGET_RATE, std::nullopt, options);
    REQUIRE_FALSE(ranges.empty());
    CHECK(ranges.front().first == 0);
    CHECK(ranges.back().second == total);
    for (std::size_t i = 1; i < ranges.size(); ++i) {
        const std::int64_t overlap = ranges[i - 1].second - ranges[i].first;
        CHECK(overlap == Approx(TARGET_RATE).margin(2));
    }
}

TEST_CASE("quiet gap cuts before max", "[test_meeting_asr]") {
    // from tests/test_meeting_asr.py::TestOfflineCutRanges::test_quiet_gap_cuts_before_max
    const std::size_t n = 40 * TARGET_RATE;
    std::vector<std::int16_t> audio(n, 5000);
    const std::size_t quiet_from = 16 * TARGET_RATE;
    std::fill_n(audio.begin() + static_cast<std::ptrdiff_t>(quiet_from), TARGET_RATE, 0);
    fv::meeting::OfflineCutOptions options;
    options.target_sec = 15.0;
    options.max_sec = 25.0;
    options.quiet_window_s = 0.7;
    options.overlap_s = 1.0;
    const auto ranges = fv::meeting::offline_cut_ranges(
        static_cast<std::int64_t>(n), TARGET_RATE, std::span<const std::int16_t>(audio), options);
    const double first_end = static_cast<double>(ranges.at(0).second) / TARGET_RATE;
    CHECK(first_end < 25.0);
    CHECK(first_end == Approx(16.7).margin(0.15));
}

TEST_CASE("a long session is cut as the whole audio would be cut", "[asr_offline]") {
    // Each scan sees only max(max_sec, target_sec) of audio: the cuts must not change.
    std::vector<std::int16_t> audio(200 * TARGET_RATE, 4000);
    for (std::size_t at = 17 * TARGET_RATE; at < audio.size(); at += 19 * TARGET_RATE) {
        std::fill_n(audio.begin() + static_cast<std::ptrdiff_t>(at),
                    std::min<std::size_t>(TARGET_RATE, audio.size() - at), 0);
    }
    const auto ranges = fv::meeting::offline_cut_ranges(
        static_cast<std::int64_t>(audio.size()), TARGET_RATE, std::span<const std::int16_t>(audio));
    std::int64_t position = 0;
    for (const auto& [start, end] : ranges) {
        CHECK(start == position);
        const auto rest =
            std::span<const std::int16_t>(audio).subspan(static_cast<std::size_t>(start));
        const auto cut =
            fv::meeting::find_cut_point(rest, TARGET_RATE, 15.0, 25.0, fv::meeting::QUIET_RMS, 0.7);
        const std::int64_t expected = std::min<std::int64_t>(
            cut.value_or(25 * TARGET_RATE), static_cast<std::int64_t>(rest.size()));
        CHECK(end - start == std::max<std::int64_t>(1, expected));
        position = end;
    }
    CHECK(position == static_cast<std::int64_t>(audio.size()));
}

TEST_CASE("keeps later window tail", "[test_meeting_asr]") {
    // from tests/test_meeting_asr.py::TestOverlapDrop::test_keeps_later_window_tail
    const std::vector<TranscriptSegment> segs = {row("a", "mic", 9.2, 9.8, "only overlap"),
                                                 row("b", "mic", 10.5, 12.0, "new")};
    CHECK(texts(fv::meeting::drop_overlapped_prefix(segs, 10.0)) ==
          std::vector<std::string>{"new"});
}

TEST_CASE("keeps segment that starts in overlap but extends past", "[test_meeting_asr]") {
    // from
    // tests/test_meeting_asr.py::TestOverlapDrop::test_keeps_segment_that_starts_in_overlap_but_extends_past
    const std::vector<TranscriptSegment> segs = {row("a", "mic", 9.2, 9.8, "only overlap"),
                                                 row("b", "mic", 9.5, 12.0, "spans boundary")};
    CHECK(texts(fv::meeting::drop_overlapped_prefix(segs, 10.0)) ==
          std::vector<std::string>{"spans boundary"});
}

TEST_CASE("offline ids are stable", "[test_meeting_asr]") {
    // from tests/test_meeting_asr.py::TestOverlapDrop::test_offline_ids_are_stable
    const std::string first = fv::meeting::offline_segment_id("m1", "mic", 1.25, 0);
    const std::string second = fv::meeting::offline_segment_id("m1", "mic", 1.25, 0);
    const std::string other = fv::meeting::offline_segment_id("m1", "mic", 1.25, 1);
    CHECK(first == second);
    CHECK(first.starts_with("sg_"));
    CHECK(first.size() == 23);
    CHECK(first != other);
}

TEST_CASE("failed window aborts instead of returning partial transcript", "[test_meeting_asr]") {
    // from
    // tests/test_meeting_asr.py::TestOverlapDrop::test_failed_window_aborts_instead_of_returning_partial_transcript
    int calls = 0;
    const fv::meeting::WindowDecoder decode = [&](std::vector<float>) {
        if (++calls == 2) {
            throw std::runtime_error("decoder failed");
        }
        return std::vector<fv::Segment>{{"first window", 0.0, 0.5}};
    };
    const std::vector<std::int16_t> frames(2 * TARGET_RATE, 1000);
    fv::meeting::SessionAudioOptions options;
    options.meeting_id = "m1";
    options.channel = "mic";
    options.target_sec = 1.0;
    options.max_sec = 1.0;
    try {
        fv::meeting::transcribe_session_audio(decode, frames, TARGET_RATE, options);
        FAIL("the pass should have failed");
    } catch (const OfflineWindowError& error) {
        CHECK(std::string(error.what()) == "Offline ASR failed for mic window 2/2 at 1.0s");
        try {
            std::rethrow_if_nested(error);
            FAIL("the decoder's error should be nested");
        } catch (const std::runtime_error& nested) {
            CHECK(std::string(nested.what()) == "decoder failed");
        }
    }
}

TEST_CASE("offline decoder internal typeerror is not retried", "[test_meeting_asr]") {
    // from tests/test_meeting_asr.py::test_offline_decoder_internal_typeerror_is_not_retried
    TempDir folder;
    const std::vector<std::int16_t> ones(16000, 1);
    write_wav_samples(folder.path() / "mic_session.wav", ones);
    int calls = 0;
    bool had_progress = false;
    fv::meeting::MeetingSessionsOptions options;
    options.channels = {"mic"};
    options.progress = [](const std::string&, int, int) {
    };
    options.transcribe =
        [&](std::span<const std::int16_t>, int, const std::string&, double,
            const fv::meeting::OfflineProgress& progress) -> std::vector<TranscriptSegment> {
        ++calls;
        had_progress = static_cast<bool>(progress);
        throw std::logic_error("decoder internal failure");
    };
    CHECK_THROWS_AS(
        fv::meeting::transcribe_meeting_sessions({}, folder.path(), "m_test", {}, options),
        std::logic_error);
    CHECK(calls == 1);
    CHECK(had_progress);
}

TEST_CASE("mic copy of system audio is dropped", "[test_meeting_hallucination]") {
    // from tests/test_meeting_hallucination.py::test_mic_copy_of_system_audio_is_dropped
    const std::vector<TranscriptSegment> rows = {
        row("lb1", "loopback", 10.0, 14.0, "The vendor ships the parts on Monday."),
        row("mic_echo", "mic", 10.4, 14.3, "vendor ships the parts on Monday"),
        row("mic_real", "mic", 15.0, 17.0, "Great, I will tell the warehouse."),
        row("mic_short", "mic", 10.5, 11.0, "Monday."),
    };
    CHECK(ids(fv::meeting::drop_mic_echo(rows)) ==
          std::vector<std::string>{"lb1", "mic_real", "mic_short"});
}

TEST_CASE("same words at a different time are not echo", "[test_meeting_hallucination]") {
    // from tests/test_meeting_hallucination.py::test_same_words_at_a_different_time_are_not_echo
    const std::vector<TranscriptSegment> rows = {
        row("lb1", "loopback", 10.0, 12.0, "ship the parts on Monday"),
        row("mic1", "mic", 40.0, 42.0, "ship the parts on Monday"),
    };
    CHECK(ids(fv::meeting::drop_mic_echo(rows)) == std::vector<std::string>{"lb1", "mic1"});
}

TEST_CASE("backends without scores are never filtered", "[test_meeting_hallucination]") {
    // from tests/test_meeting_hallucination.py::test_backends_without_scores_are_never_filtered
    // Parakeet's segments carry no scores: only empty text is dropped, stock phrases stay.
    const fv::meeting::WindowDecoder decode = [](std::vector<float>) {
        return std::vector<fv::Segment>{
            {"Thanks for watching", 0.0, 0.4}, {"you", 0.4, 0.6}, {"  ", 0.6, 0.7}};
    };
    const std::vector<std::int16_t> frames(TARGET_RATE, 1000);
    fv::meeting::SessionAudioOptions options;
    options.meeting_id = "m";
    options.channel = "mic";
    const auto kept = fv::meeting::transcribe_session_audio(decode, frames, TARGET_RATE, options);
    CHECK(texts(kept) == std::vector<std::string>{"Thanks for watching", "you"});
}

TEST_CASE("windows carry meeting time, ordinals and progress", "[asr_offline]") {
    std::vector<std::pair<std::string, std::pair<int, int>>> progress;
    std::vector<std::size_t> window_sizes;
    const fv::meeting::WindowDecoder decode = [&](std::vector<float> audio) {
        window_sizes.push_back(audio.size());
        return std::vector<fv::Segment>{{"", 0.0, 0.1}, {"said", 0.25, 0.75}};
    };
    const std::vector<std::int16_t> frames(3 * TARGET_RATE, 1000);
    fv::meeting::SessionAudioOptions options;
    options.meeting_id = "m1";
    options.channel = "loopback";
    options.origin_s = 100.0;
    options.target_sec = 1.0;
    options.max_sec = 1.0;
    options.progress = [&](const std::string& detail, int current, int total) {
        progress.push_back({detail, {current, total}});
    };
    const auto segments =
        fv::meeting::transcribe_session_audio(decode, frames, TARGET_RATE, options);
    REQUIRE(segments.size() == 3);
    CHECK(segments[1].start_s == Approx(101.25));
    CHECK(segments[1].end_s == Approx(101.75));
    CHECK(segments[1].segment_id ==
          fv::meeting::offline_segment_id("m1", "loopback", segments[1].start_s, 1));
    CHECK_FALSE(segments[1].chunk_id.has_value());
    CHECK(window_sizes == std::vector<std::size_t>{16000, 16000, 16000});
    REQUIRE(progress.size() == 3);
    CHECK(progress[0].first == "System Audio: decoding window 1/3 (100.0s - 101.0s)");
    CHECK(progress[2].first == "System Audio: decoding window 3/3 (102.0s - 103.0s)");
    CHECK(progress[2].second == std::pair<int, int>{3, 3});
}

TEST_CASE("a progress callback that throws does not stop the pass", "[asr_offline]") {
    const fv::meeting::WindowDecoder decode = [](std::vector<float>) {
        return std::vector<fv::Segment>{{"ok", 0.0, 0.5}};
    };
    const std::vector<std::int16_t> frames(TARGET_RATE, 1000);
    fv::meeting::SessionAudioOptions options;
    options.meeting_id = "m";
    options.channel = "custom";
    std::string seen;
    options.progress = [&](const std::string& detail, int, int) {
        seen = detail;
        throw std::runtime_error("ui gone");
    };
    const fv::meeting::test::LogCapture logs;
    CHECK(fv::meeting::transcribe_session_audio(decode, frames, TARGET_RATE, options).size() == 1);
    CHECK(seen == "Custom: decoding window 1/1 (0.0s - 1.0s)");
    CHECK(logs.levels("Offline ASR progress callback failed") ==
          std::vector{fv::log::Level::Error});
}

TEST_CASE("the meeting pass merges channels by time and drops mic echo", "[asr_offline]") {
    TempDir folder;
    const std::vector<std::int16_t> mic(2 * TARGET_RATE, 1500);
    const std::vector<std::int16_t> loopback(2 * TARGET_RATE, 2500);
    write_wav_samples(folder.path() / "mic_session.wav", mic);
    write_wav_samples(folder.path() / "loopback_session.wav", loopback);
    fv::meeting::write_session_meta(fv::meeting::session_meta_path(folder.path(), "loopback"),
                                    16000, 2 * TARGET_RATE, 0.5);
    const fv::meeting::WindowDecoder decode = [](std::vector<float> audio) {
        // The level tells the channels apart.
        if (audio.front() > 2000.0f / 32768.0f) {
            return std::vector<fv::Segment>{{"the vendor ships the parts", 0.0, 1.5}};
        }
        return std::vector<fv::Segment>{{"vendor ships the parts", 0.1, 1.4},
                                        {"thanks everyone", 1.6, 1.9}};
    };
    const auto merged = fv::meeting::transcribe_meeting_sessions(decode, folder.path(), "m1", {});
    CHECK(texts(merged) ==
          std::vector<std::string>{"the vendor ships the parts", "thanks everyone"});
    REQUIRE(merged.size() == 2);
    CHECK(merged[0].channel == "loopback");
    CHECK(merged[0].start_s == Approx(0.5));
    CHECK(merged[1].channel == "mic");
}

TEST_CASE("the meeting pass skips channels without audio", "[asr_offline]") {
    TempDir folder;
    int calls = 0;
    const fv::meeting::WindowDecoder decode = [&](std::vector<float>) {
        ++calls;
        return std::vector<fv::Segment>{};
    };
    const fv::meeting::test::LogCapture logs;
    CHECK(fv::meeting::transcribe_meeting_sessions(decode, folder.path() / "missing", "m1", {})
              .empty());
    CHECK(calls == 0);
    CHECK(fv::meeting::transcribe_meeting_sessions(decode, folder.path(), "m1", {}).empty());
    CHECK(logs.levels("No session audio for offline ASR on channel mic") ==
          std::vector{fv::log::Level::Info});
    CHECK(calls == 0);
}

TEST_CASE("the meeting pass stitches chunks when the session WAV is missing", "[asr_offline]") {
    TempDir folder;
    const std::vector<std::int16_t> first(TARGET_RATE, 1200);
    const std::vector<std::int16_t> second(TARGET_RATE, 1200);
    write_wav_samples(folder.path() / "mic_00000.wav", first);
    write_wav_samples(folder.path() / "mic_00001.wav", second);
    const auto chunk = [&](int id, int seq, double start) {
        Json made = Json::object();
        made["id"] = id;
        made["meeting_id"] = "m1";
        made["channel"] = "mic";
        made["seq"] = seq;
        made["file_path"] =
            fv::path_to_utf8(folder.path() / ("mic_0000" + std::to_string(seq) + ".wav"));
        made["start_s"] = start;
        made["duration_s"] = 1.0;
        made["sample_rate"] = 16000;
        return made;
    };
    std::vector<std::size_t> sizes;
    const fv::meeting::WindowDecoder decode = [&](std::vector<float> audio) {
        sizes.push_back(audio.size());
        return std::vector<fv::Segment>{{"stitched", 0.0, 0.5}};
    };
    const auto merged = fv::meeting::transcribe_meeting_sessions(
        decode, folder.path(), "m1", {chunk(1, 0, 4.0), chunk(2, 1, 5.0)});
    REQUIRE(merged.size() == 1);
    // The earliest chunk is the session's origin when no meta says otherwise.
    CHECK(merged[0].start_s == Approx(4.0));
    CHECK(sizes == std::vector<std::size_t>{2 * TARGET_RATE});
}
