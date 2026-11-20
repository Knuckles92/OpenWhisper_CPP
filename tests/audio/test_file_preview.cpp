// preview_file: what an uploaded file is, read from its header (services/audio_processor.py).
// preview_file runs on every dropped file, so what it costs matters: it is the difference
// between a card that appears at once and one that waits out a full decode of a long recording.
// Whether a preview decoded the file is read from the log line the decoding fallback writes.
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <format>
#include <fstream>
#include <memory>
#include <mutex>
#include <numbers>
#include <string>
#include <vector>

#include "audio/file_preview.h"
#include "audio/wav_writer.h"
#include "base/identity.h"
#include "base/log.h"
#include "base/utf8.h"
#include "golden/golden.h"
#include "pyport/errors.h"
#include "testkit/testkit.h"

// After golden.h: windows.h defines near and far as macros.
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace {

using Catch::Approx;
using fv::audio::AudioFileNotFound;
using fv::audio::AudioFilePreview;
using fv::audio::preview_file;
using fv::audio::probe_audio_header;
using fv::audio::UnreadableAudio;

std::filesystem::path upload_fixture(const std::string& name) {
    return fv::testkit::source_root() / "tests" / "fixtures" / "upload" / name;
}

// write_wav: a 440 Hz tone as a real 16-bit WAV, so FFmpeg parses a genuine container header.
std::filesystem::path write_wav(const std::filesystem::path& path, double seconds = 2.0,
                                int rate = 44100, int channels = 1) {
    fv::audio::WavWriter writer(path, {.rate = rate, .channels = channels, .sample_width = 2});
    const auto frames = static_cast<std::int64_t>(seconds * rate);
    // One second at a time keeps the 25 MB file's memory to one second of samples.
    std::vector<std::int16_t> block;
    for (std::int64_t start = 0; start < frames; start += rate) {
        const std::int64_t count = std::min<std::int64_t>(rate, frames - start);
        block.clear();
        for (std::int64_t i = start; i < start + count; ++i) {
            const auto tone = static_cast<std::int16_t>(
                std::sin(2 * std::numbers::pi * 440.0 * static_cast<double>(i) / rate) * 8000);
            for (int c = 0; c < channels; ++c) {
                block.push_back(tone);
            }
        }
        writer.write(std::span<const std::int16_t>(block));
    }
    writer.commit(std::nullopt);
    return path;
}

void write_bytes(const std::filesystem::path& path, const std::string& bytes) {
    std::ofstream out(path, std::ios::binary);
    out << bytes;
}

std::string repeat(const std::string& text, int times) {
    std::string out;
    for (int i = 0; i < times; ++i) {
        out += text;
    }
    return out;
}

// Records the messages logged under services.audio_processor while it is alive.
class PreviewLog : public fv::log::Sink {
public:
    void write(const fv::log::Record& record) override {
        if (record.logger == "services.audio_processor") {
            const std::scoped_lock lock(mutex_);
            messages_.emplace_back(record.message);
        }
    }
    bool decoded() const {
        const std::scoped_lock lock(mutex_);
        for (const std::string& message : messages_) {
            if (message.find("counting decoded samples") != std::string::npos) {
                return true;
            }
        }
        return false;
    }

private:
    mutable std::mutex mutex_;
    std::vector<std::string> messages_;
};

class ScopedPreviewLog {
public:
    ScopedPreviewLog() : sink_(std::make_shared<PreviewLog>()) { fv::log::add_sink(sink_); }
    ~ScopedPreviewLog() { fv::log::remove_sink(sink_); }
    ScopedPreviewLog(const ScopedPreviewLog&) = delete;
    ScopedPreviewLog& operator=(const ScopedPreviewLog&) = delete;
    bool decoded() const { return sink_->decoded(); }

private:
    std::shared_ptr<PreviewLog> sink_;
};

// _generate_time_based_splits with config's limits, standing in for the splitting engine's own.
std::vector<std::int64_t> time_based_splits(std::int64_t total_samples, int sample_rate) {
    const std::int64_t max_samples =
        (std::int64_t{fv::audio::kMaxFileSizeMb} * 1024 * 1024 - 44) / 2;
    const std::int64_t overlap = std::min(
        std::max<std::int64_t>(0, static_cast<std::int64_t>(2.0 * sample_rate)), max_samples / 4);
    std::vector<std::int64_t> split_points;
    std::int64_t current = 0;
    while (total_samples - current > max_samples) {
        const std::int64_t boundary = current + max_samples - overlap;
        split_points.push_back(boundary);
        current = boundary - overlap;
    }
    return split_points;
}

// Over the 23 MiB upload limit: 300 s of 44.1 kHz mono is 25.2 MiB.
constexpr double kLargeSeconds = 300.0;

} // namespace

TEST_CASE("small file is previewed from the header only", "[test_audio_processor]") {
    // from
    // tests/test_audio_processor.py::TestPreviewCost::test_small_file_is_previewed_from_the_header_only
    const fv::testkit::TempDataRoot root;
    const auto path = write_wav(root.path() / "clip.wav", 2.0, 44100);
    const ScopedPreviewLog log;

    const AudioFilePreview preview = preview_file(path, true);

    CHECK_FALSE(log.decoded());
    CHECK_FALSE(preview.needs_splitting);
    CHECK_FALSE(preview.over_upload_limit());
    CHECK(preview.sample_rate == 44100);
    CHECK(preview.channels == 1);
    CHECK(preview.duration_seconds == Approx(2.0).margin(0.05));
    CHECK(preview.estimated_chunks == 1);
    CHECK(preview.chunk_durations == std::vector<double>{preview.duration_seconds});
    CHECK(preview.file_name == "clip.wav");
    CHECK(preview.file_path == fv::path_to_utf8(path));
}

TEST_CASE("stereo channel count comes from the header", "[test_audio_processor]") {
    // from
    // tests/test_audio_processor.py::TestPreviewCost::test_stereo_channel_count_comes_from_the_header
    const fv::testkit::TempDataRoot root;
    const auto path = write_wav(root.path() / "stereo.wav", 1.0, 44100, 2);

    const AudioFilePreview preview = preview_file(path, false);

    CHECK(preview.channels == 2);
    CHECK(preview.duration_seconds == Approx(1.0).margin(0.05));
}

TEST_CASE("large file is estimated from header and reports chunks", "[test_audio_processor]") {
    // from
    // tests/test_audio_processor.py::TestPreviewCost::test_large_file_is_estimated_from_header_and_reports_chunks
    // Python shrinks MAX_FILE_SIZE_MB to 0.05 for a 3 s file; the limit is a constant here, so
    // the file is over the real 23 MiB.
    const fv::testkit::TempDataRoot root;
    const auto path = write_wav(root.path() / "big.wav", kLargeSeconds, 44100);
    const fv::audio::ChunkEstimator estimator = time_based_splits;
    const ScopedPreviewLog log;

    const AudioFilePreview preview = preview_file(path, true, &estimator);

    CHECK_FALSE(log.decoded());
    CHECK(preview.over_upload_limit());
    CHECK(preview.needs_splitting);
    CHECK(preview.estimated_chunks >= 1);
    CHECK(preview.estimated_chunks == static_cast<int>(preview.chunk_durations.size()));
    // 300 s is over one upload window (12,058,602 samples, 273.4 s at 44.1 kHz): two chunks, the
    // first ending one overlap before the window's end.
    CHECK(preview.estimated_chunks == 2);
    REQUIRE_FALSE(preview.chunk_durations.empty());
    CHECK(preview.chunk_durations.front() == Approx((12058602.0 - 88200.0) / 44100.0));
    double total = 0.0;
    for (const double seconds : preview.chunk_durations) {
        total += seconds;
    }
    CHECK(total == Approx(kLargeSeconds).margin(0.05));
}

TEST_CASE("large file for a one pass engine is read from the header", "[test_audio_processor]") {
    // from
    // tests/test_audio_processor.py::TestPreviewCost::test_large_file_for_a_one_pass_engine_is_read_from_the_header
    const fv::testkit::TempDataRoot root;
    const auto path = write_wav(root.path() / "big.wav", kLargeSeconds, 44100);
    const fv::audio::ChunkEstimator estimator = time_based_splits;
    const ScopedPreviewLog log;

    const AudioFilePreview preview = preview_file(path, false, &estimator);

    CHECK_FALSE(log.decoded());
    CHECK(preview.over_upload_limit());
    CHECK_FALSE(preview.needs_splitting);
    CHECK(preview.estimated_chunks == 1);
    CHECK(preview.duration_seconds == Approx(kLargeSeconds).margin(0.05));
}

TEST_CASE("header without duration falls back to decoding", "[test_audio_processor]") {
    // from
    // tests/test_audio_processor.py::TestPreviewCost::test_header_without_duration_falls_back_to_decoding
    // Python fakes av.open with a stream that reports no duration; here a FLAC whose STREAMINFO
    // leaves the sample count unknown is the real thing.
    const ScopedPreviewLog log;

    const auto header = probe_audio_header(upload_fixture("no-duration.flac"));

    CHECK(log.decoded());
    CHECK(header.duration_s == Approx(0.8).margin(0.05));
    CHECK(header.sample_rate == 48000);
    CHECK(header.channels == 1);
}

TEST_CASE("missing file raises file not found", "[test_audio_processor]") {
    // from
    // tests/test_audio_processor.py::TestPreviewErrors::test_missing_file_raises_file_not_found
    const fv::testkit::TempDataRoot root;
    const auto path = root.path() / "nope.wav";
    try {
        preview_file(path, false);
        FAIL("a missing file was previewed");
    } catch (const AudioFileNotFound& error) {
        CHECK(std::string(error.what()) == "Audio file not found: " + fv::path_to_utf8(path));
    }
}

TEST_CASE("unreadable file raises value error", "[test_audio_processor]") {
    // from
    // tests/test_audio_processor.py::TestPreviewErrors::test_unreadable_file_raises_value_error The
    // Upload tab turns this into an inline notice, not a crash.
    const fv::testkit::TempDataRoot root;
    const auto path = root.path() / "broken.wav";
    write_bytes(path, "not audio at all");

    CHECK_THROWS_AS(preview_file(path, false), UnreadableAudio);
}

TEST_CASE("unreadable file says why in plain words", "[test_audio_processor]") {
    // from
    // tests/test_audio_processor.py::TestPreviewErrors::test_unreadable_file_says_why_in_plain_words
    // FFmpeg's "Invalid data found when processing input" must not reach the drop zone.
    const auto [content, expected] = GENERATE(table<std::string, std::string>({
        {"", "This file is empty."},
        {repeat("not audio at all", 64), "This doesn't look like an audio file."},
    }));
    const fv::testkit::TempDataRoot root;
    const auto path = root.path() / "broken.wav";
    write_bytes(path, content);

    try {
        preview_file(path, false);
        FAIL("an unreadable file was previewed");
    } catch (const UnreadableAudio& error) {
        CHECK(std::string(error.what()) == expected);
    }
}

TEST_CASE("preview keeps its documented constructor", "[test_audio_processor]") {
    // from
    // tests/test_audio_processor.py::TestPreviewShape::test_preview_keeps_its_documented_constructor
    // The Upload tab's tests build one from exactly these fields.
    const AudioFilePreview preview{
        .file_path = "/tmp/a.wav",
        .file_name = "a.wav",
        .file_size_mb = 1.0,
        .duration_seconds = 60.0,
        .sample_rate = 44100,
        .channels = 2,
        .needs_splitting = false,
        .estimated_chunks = 1,
    };

    CHECK(preview.chunk_durations.empty());
    CHECK_FALSE(preview.duration_formatted().empty());
    CHECK_FALSE(preview.file_size_formatted().empty());
}

TEST_CASE("each unreadable kind of file has its own sentence") {
    const auto [name, expected] = GENERATE(table<std::string, std::string>({
        {"empty.wav", "This file is empty."},
        {"no-audio.srt", "This file has no audio track."},
        {"header-only.wav", "This file has no audio in it."},
    }));
    try {
        preview_file(upload_fixture(name), false);
        FAIL("an unreadable file was previewed");
    } catch (const UnreadableAudio& error) {
        CHECK(std::string(error.what()) == expected);
    }
}

TEST_CASE("a header without duration is previewed by decoding it") {
    const ScopedPreviewLog log;

    const AudioFilePreview preview = preview_file(upload_fixture("no-duration.flac"), true);

    CHECK(log.decoded());
    CHECK(preview.duration_seconds == Approx(0.8).margin(0.05));
    CHECK(preview.chunk_durations == std::vector<double>{preview.duration_seconds});
}

TEST_CASE("a file over the limit without an estimator is one pass") {
    const fv::testkit::TempDataRoot root;
    const auto path = write_wav(root.path() / "big.wav", kLargeSeconds, 44100);

    const AudioFilePreview preview = preview_file(path, true);

    CHECK(preview.needs_splitting);
    CHECK(preview.estimated_chunks == 1);
    CHECK(preview.chunk_durations == std::vector<double>{preview.duration_seconds});
}

TEST_CASE("check_file_size reports the size and whether it is over the limit") {
    const fv::testkit::TempDataRoot root;
    const auto small = write_wav(root.path() / "clip.wav", 1.0, 44100);
    const auto size = fv::audio::check_file_size(small);
    CHECK_FALSE(size.needs_splitting);
    CHECK(size.size_mb == Approx(88244.0 / (1024 * 1024)));
    CHECK_THROWS_AS(fv::audio::check_file_size(root.path() / "nope.wav"), AudioFileNotFound);

    const auto big = write_wav(root.path() / "big.wav", kLargeSeconds, 44100);
    CHECK(fv::audio::check_file_size(big).needs_splitting);
}

TEST_CASE("the size reads in MB from one MiB and in KB below") {
    const auto [size_mb, expected] = GENERATE(table<double, std::string>({
        {1.0, "1.0 MB"},
        {23.04, "23.0 MB"},
        {1.25, "1.2 MB"},
        {0.5, "512 KB"},
        {0.99999, "1024 KB"},
        {88244.0 / (1024 * 1024), "86 KB"},
        {0.0, "0 KB"},
    }));
    AudioFilePreview preview;
    preview.file_size_mb = size_mb;
    CHECK(preview.file_size_formatted() == expected);
}

TEST_CASE("over the upload limit means strictly more than 23 MiB") {
    AudioFilePreview preview;
    preview.file_size_mb = 23.0;
    CHECK_FALSE(preview.over_upload_limit());
    preview.file_size_mb = std::nextafter(23.0, 24.0);
    CHECK(preview.over_upload_limit());
}

TEST_CASE("the duration reads as format_audio_duration does", "[golden]") {
    fv::golden::for_each("services.format_utils", "format_audio_duration",
                         [](const fv::golden::Case& c) {
                             const fv::Json& seconds = c.args()[0];
                             // None and text are format_utils' own inputs; a preview always
                             // holds a number.
                             if (!seconds.is_number() && !seconds.is_object()) {
                                 return;
                             }
                             AudioFilePreview preview;
                             preview.duration_seconds = c.arg<double>(0);
                             CHECK(preview.duration_formatted() == c.result<std::string>());
                         });
}

#ifdef _WIN32
TEST_CASE("a file another program holds open is not allowed to be read", "[platform]") {
    const fv::testkit::TempDataRoot root;
    const auto path = write_wav(root.path() / "locked.wav", 1.0, 44100);
    // No sharing: every other open fails with a sharing violation, which the C runtime reports
    // as EACCES, as Python's open does.
    const HANDLE handle = CreateFileW(path.c_str(), GENERIC_READ, 0, nullptr, OPEN_EXISTING,
                                      FILE_ATTRIBUTE_NORMAL, nullptr);
    REQUIRE(handle != INVALID_HANDLE_VALUE);
    const std::unique_ptr<void, decltype(&CloseHandle)> closer(handle, &CloseHandle);
    try {
        preview_file(path, false);
        FAIL("a locked file was previewed");
    } catch (const UnreadableAudio& error) {
        CHECK(std::string(error.what()) ==
              std::format("{} isn't allowed to read this file.", fv::identity().product));
    }
}
#endif
