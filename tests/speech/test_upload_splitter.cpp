// The silence-aware upload splitter (tests/test_audio_processor.py splitting, moving-average and
// cleanup tests; tests/test_chunking.py) and its golden vectors.

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <mutex>
#include <numbers>
#include <random>
#include <stdexcept>
#include <string>
#include <vector>

#include "audio/wav_writer.h"
#include "base/hash.h"
#include "base/log.h"
#include "engine/error.h"
#include "golden/golden.h"
#include "speech/upload_splitter.h"
#include "testkit/testkit.h"

using namespace fv;
using namespace fv::speech;

namespace {

// ---- signals (tools/parity/cases/services.audio_processor.py) ----

std::int64_t floor_div(std::int64_t a, std::int64_t b) {
    const std::int64_t q = a / b;
    return (a % b != 0 && ((a < 0) != (b < 0))) ? q - 1 : q;
}

std::vector<std::int16_t> pcm(const std::string& kind, std::size_t length, int rate,
                              std::uint32_t seed) {
    std::vector<std::int16_t> out(length);
    const std::size_t tenth = std::max(1, rate / 10);
    std::uint32_t state = seed;
    for (std::size_t i = 0; i < length; ++i) {
        state = state * 1664525U + 1013904223U;
        const std::int64_t v = static_cast<std::int64_t>(state >> 16) - 32768;
        std::int64_t sample = 0;
        if (kind == "noise" || (kind == "gaps" && (i / tenth) % 37 < 29)) {
            sample = floor_div(v * 6, 32);
        } else if (kind == "quiet") {
            sample = floor_div(v, 512);
        } else if (kind == "loud") {
            sample = v;
        }
        out[i] = static_cast<std::int16_t>(sample);
    }
    return out;
}

std::vector<float> floats(const std::string& kind, std::size_t length, std::uint32_t seed) {
    std::vector<float> out(length);
    if (kind == "uniform") {
        std::uint32_t state = seed;
        for (auto& value : out) {
            state = state * 1664525U + 1013904223U;
            value = static_cast<float>(state >> 8) / 16777216.0F;
        }
    } else {
        const auto noise = pcm("noise", length, 16000, seed);
        for (std::size_t i = 0; i < length; ++i) {
            out[i] = std::abs(static_cast<float>(noise[i])) / 32768.0F;
        }
    }
    return out;
}

SplitLimits limits_of(const Json& given) {
    SplitLimits limits;
    if (given.contains("max_file_size_mb")) {
        limits.max_file_size_mb = given["max_file_size_mb"].get<double>();
    }
    if (given.contains("overlap_s")) {
        limits.overlap_s = given["overlap_s"].get<double>();
    }
    if (given.contains("min_chunk_s")) {
        limits.min_chunk_s = given["min_chunk_s"].get<double>();
    }
    if (given.contains("silence_s")) {
        limits.silence_s = given["silence_s"].get<double>();
    }
    return limits;
}

// A 16-bit PCM WAV as Python's wave module writes it.
void write_wav(const std::filesystem::path& path, const std::vector<std::int16_t>& interleaved,
               int rate, int channels = 1) {
    audio::WavWriter writer(path, audio::WavWriter::Format{rate, channels, 2});
    writer.write(interleaved);
    writer.commit(std::nullopt);
}

// test_audio_processor.py's write_wav: a 440 Hz tone at 8000 amplitude.
std::vector<std::int16_t> tone(double seconds, int rate, int channels = 1) {
    const auto frames = static_cast<std::size_t>(seconds * rate);
    std::vector<std::int16_t> out;
    for (std::size_t i = 0; i < frames; ++i) {
        const double t = static_cast<double>(i) * (seconds / static_cast<double>(frames));
        const auto value =
            static_cast<std::int16_t>(std::sin(2.0 * std::numbers::pi * 440.0 * t) * 8000.0);
        for (int channel = 0; channel < channels; ++channel) {
            out.push_back(value);
        }
    }
    return out;
}

// A WAV of any sample format: 1 PCM (8, 16, 32 bit) or 3 IEEE float (32 bit).
void write_raw_wav(const std::filesystem::path& path, int format, int bits, int channels, int rate,
                   const std::string& data) {
    std::string out;
    const auto u32 = [&out](std::uint32_t v) {
        for (int i = 0; i < 4; ++i) {
            out += static_cast<char>((v >> (8 * i)) & 0xFF);
        }
    };
    const auto u16 = [&out](std::uint16_t v) {
        out += static_cast<char>(v & 0xFF);
        out += static_cast<char>(v >> 8);
    };
    const int block = channels * bits / 8;
    out += "RIFF";
    u32(static_cast<std::uint32_t>(36 + data.size()));
    out += "WAVEfmt ";
    u32(16);
    u16(static_cast<std::uint16_t>(format));
    u16(static_cast<std::uint16_t>(channels));
    u32(static_cast<std::uint32_t>(rate));
    u32(static_cast<std::uint32_t>(rate * block));
    u16(static_cast<std::uint16_t>(block));
    u16(static_cast<std::uint16_t>(bits));
    out += "data";
    u32(static_cast<std::uint32_t>(data.size()));
    out += data;
    std::ofstream(path, std::ios::binary) << out;
}

// The mono 16-bit samples of a chunk WAV.
std::vector<std::int16_t> read_chunk(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    std::string bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    REQUIRE(bytes.size() >= 44);
    std::vector<std::int16_t> samples((bytes.size() - 44) / 2);
    std::memcpy(samples.data(), bytes.data() + 44, samples.size() * 2);
    return samples;
}

// A splitter whose temporary folders live in the test's data root.
class TestSplitter : public UploadSplitter {
public:
    TestSplitter(SplitLimits limits, std::filesystem::path root)
        : UploadSplitter(limits), root_(std::move(root)) {}

    // _load_audio_metadata: every block of a file, concatenated.
    std::vector<std::int16_t> decode_all(const std::filesystem::path& file, int& rate,
                                         int& channels) {
        auto blocks = open_blocks(file, CancelToken());
        std::vector<std::int16_t> all;
        std::vector<std::int16_t> block;
        while (blocks->next(block, rate, channels)) {
            all.insert(all.end(), block.begin(), block.end());
        }
        return all;
    }

protected:
    std::filesystem::path make_temp_dir() override {
        const auto dir = root_ / ("audio_chunks_" + std::to_string(++made_));
        std::filesystem::create_directories(dir);
        return dir;
    }

private:
    std::filesystem::path root_;
    int made_ = 0;
};

// Blocks from a list, as the tests replace _iter_audio_blocks.
class ListBlocks final : public UploadSplitter::BlockSource {
public:
    ListBlocks(std::vector<std::vector<std::int16_t>> blocks, int rate, bool* closed = nullptr)
        : blocks_(std::move(blocks)), rate_(rate), closed_(closed) {}
    ~ListBlocks() override {
        if (closed_ != nullptr) {
            *closed_ = true;
        }
    }
    bool next(std::vector<std::int16_t>& samples, int& rate, int& channels) override {
        if (index_ >= blocks_.size()) {
            return false;
        }
        samples = blocks_[index_++];
        rate = rate_;
        channels = 1;
        return true;
    }

private:
    std::vector<std::vector<std::int16_t>> blocks_;
    std::size_t index_ = 0;
    int rate_;
    bool* closed_;
};

} // namespace

// ---- TestSplitting ----

// from
// tests/test_audio_processor.py::TestSplitting::test_split_produces_files_and_cleanup_removes_them
TEST_CASE("TestSplitting::test_split_produces_files_and_cleanup_removes_them",
          "[test_audio_processor]") {
    const testkit::TempDataRoot root;
    const auto wav = root.path() / "big.wav";
    write_wav(wav, tone(3.0, 44100), 44100);
    UploadSplitter splitter(SplitLimits{.max_file_size_mb = 0.05});
    const auto chunks = splitter.split_audio_file(wav, {}, {});
    REQUIRE_FALSE(chunks.empty());
    for (const auto& chunk : chunks) {
        CHECK(std::filesystem::exists(chunk));
    }
    splitter.cleanup_temp_files();
    for (const auto& chunk : chunks) {
        CHECK_FALSE(std::filesystem::exists(chunk));
    }
    CHECK(splitter.temp_files().empty());
}

// from tests/test_audio_processor.py::TestSplitting::test_pcm_waveform_survives_splitting_exactly
TEST_CASE("TestSplitting::test_pcm_waveform_survives_splitting_exactly", "[test_audio_processor]") {
    const testkit::TempDataRoot root;
    const std::vector<std::int16_t> pattern{-32768, -20000, -1, 0, 1, 20000, 32767};
    std::vector<std::int16_t> samples;
    for (int i = 0; i < 4000; ++i) {
        samples.insert(samples.end(), pattern.begin(), pattern.end());
    }
    const auto wav = root.path() / "pcm.wav";
    write_wav(wav, samples, 8000);
    TestSplitter splitter(SplitLimits{.max_file_size_mb = 0.01, .overlap_s = 0}, root.path());
    const auto chunks = splitter.split_audio_file(wav, {}, {});
    REQUIRE(chunks.size() > 1);
    std::vector<std::int16_t> decoded;
    for (const auto& chunk : chunks) {
        const auto part = read_chunk(chunk);
        decoded.insert(decoded.end(), part.begin(), part.end());
    }
    CHECK(decoded == samples);
    splitter.cleanup_temp_files();
}

// from
// tests/test_audio_processor.py::TestSplitting::test_overlap_and_header_never_exceed_upload_limit
TEST_CASE("TestSplitting::test_overlap_and_header_never_exceed_upload_limit",
          "[test_audio_processor]") {
    const testkit::TempDataRoot root;
    const auto wav = root.path() / "overlap.wav";
    write_wav(wav, tone(5, 8000), 8000);
    const SplitLimits limits{.max_file_size_mb = 0.02, .overlap_s = 2};
    TestSplitter splitter(limits, root.path());
    const auto chunks = splitter.split_audio_file(wav, {}, {});
    REQUIRE(chunks.size() > 1);
    std::vector<std::vector<std::int16_t>> arrays;
    for (const auto& chunk : chunks) {
        CHECK(std::filesystem::file_size(chunk) <= static_cast<std::uintmax_t>(0.02 * 1024 * 1024));
        arrays.push_back(read_chunk(chunk));
    }
    const auto overlap = chunk_limits(8000, limits).second;
    for (std::size_t i = 1; i < arrays.size(); ++i) {
        const auto& previous = arrays[i - 1];
        const auto& current = arrays[i];
        CHECK(std::vector<std::int16_t>(previous.end() - static_cast<std::ptrdiff_t>(2 * overlap),
                                        previous.end()) ==
              std::vector<std::int16_t>(
                  current.begin(), current.begin() + static_cast<std::ptrdiff_t>(2 * overlap)));
    }
    splitter.cleanup_temp_files();
}

// from
// tests/test_audio_processor.py::TestSplitting::test_streaming_writes_before_reading_entire_input
TEST_CASE("TestSplitting::test_streaming_writes_before_reading_entire_input",
          "[test_audio_processor]") {
    // File length cannot increase the analysis and decode working buffer.
    const SplitLimits limits{.max_file_size_mb = 0.02, .overlap_s = 0.05, .min_chunk_s = 0.1};
    const auto max_samples = chunk_limits(8000, limits).first;

    class Streaming final : public UploadSplitter {
    public:
        using UploadSplitter::UploadSplitter;
        std::vector<std::size_t> saved;
        std::vector<std::size_t> examined;

    protected:
        std::unique_ptr<BlockSource> open_blocks(const std::filesystem::path&,
                                                 const CancelToken&) override {
            class Source final : public BlockSource {
            public:
                explicit Source(Streaming& owner) : owner_(owner) {}
                bool next(std::vector<std::int16_t>& samples, int& rate, int& channels) override {
                    if (index_ == 500) {
                        return false;
                    }
                    if (index_ == 20) {
                        CHECK_FALSE(owner_.saved.empty());
                    }
                    ++index_;
                    samples.assign(1000, 8000);
                    rate = 8000;
                    channels = 1;
                    return true;
                }

            private:
                Streaming& owner_;
                int index_ = 0;
            };
            return std::make_unique<Source>(*this);
        }
        void save_chunk(std::span<const std::int16_t> samples, int,
                        const std::filesystem::path&) override {
            saved.push_back(samples.size());
        }
        std::size_t boundary(std::span<const std::int16_t> window, int rate, std::size_t prefix,
                             std::size_t overlap, const CancelToken& cancel) override {
            examined.push_back(window.size());
            return UploadSplitter::boundary(window, rate, prefix, overlap, cancel);
        }
    } splitter(limits);

    splitter.split_audio_file("stream", {}, {});
    CHECK(splitter.saved.size() > 40);
    CHECK(*std::max_element(splitter.saved.begin(), splitter.saved.end()) <= max_samples);
    REQUIRE_FALSE(splitter.examined.empty());
    CHECK(*std::max_element(splitter.examined.begin(), splitter.examined.end()) <= max_samples);
    splitter.cleanup_temp_files();
}

namespace {

// A splitter fed from a list of blocks, saving real chunk files.
class ListSplitter final : public UploadSplitter {
public:
    ListSplitter(SplitLimits limits, std::vector<std::int16_t> samples, int rate)
        : UploadSplitter(limits), samples_(std::move(samples)), rate_(rate) {}

protected:
    std::unique_ptr<BlockSource> open_blocks(const std::filesystem::path&,
                                             const CancelToken&) override {
        return std::make_unique<ListBlocks>(std::vector<std::vector<std::int16_t>>{samples_},
                                            rate_);
    }

private:
    std::vector<std::int16_t> samples_;
    int rate_;
};

} // namespace

// from
// tests/test_audio_processor.py::TestSplitting::test_exact_chunk_size_does_not_emit_overlap_only_tail
TEST_CASE("TestSplitting::test_exact_chunk_size_does_not_emit_overlap_only_tail",
          "[test_audio_processor]") {
    const SplitLimits limits{.max_file_size_mb = 0.02};
    const auto max_samples = chunk_limits(8000, limits).first;
    ListSplitter splitter(limits, std::vector<std::int16_t>(max_samples, 0), 8000);
    const auto chunks = splitter.split_audio_file("stream", {}, {});
    CHECK(chunks.size() == 1);
    splitter.cleanup_temp_files();
}

// from
// tests/test_audio_processor.py::TestSplitting::test_silence_boundary_keeps_every_sample_with_overlap
TEST_CASE("TestSplitting::test_silence_boundary_keeps_every_sample_with_overlap",
          "[test_audio_processor]") {
    const testkit::TempDataRoot root;
    std::vector<std::int16_t> samples(9000, 12000);
    std::fill(samples.begin() + 3800, samples.begin() + 4600, std::int16_t{0});
    const auto wav = root.path() / "silence.wav";
    write_wav(wav, samples, 1000);
    TestSplitter splitter(
        SplitLimits{.max_file_size_mb = 0.01, .overlap_s = 0.1, .min_chunk_s = 1, .silence_s = 0.3},
        root.path());
    const auto chunks = splitter.split_audio_file(wav, {}, {});
    REQUIRE(chunks.size() == 2);
    const auto first = read_chunk(chunks[0]);
    const auto second = read_chunk(chunks[1]);
    const std::size_t overlap = 100;
    CHECK(first.size() - overlap > 3800);
    CHECK(first.size() - overlap < 4600);
    std::vector<std::int16_t> joined(first.begin(), first.end() - overlap);
    joined.insert(joined.end(), second.begin() + overlap, second.end());
    CHECK(joined == samples);
    splitter.cleanup_temp_files();
}

// from
// tests/test_audio_processor.py::TestSplitting::test_small_minimum_duration_still_advances_past_overlap
TEST_CASE("TestSplitting::test_small_minimum_duration_still_advances_past_overlap",
          "[test_audio_processor]") {
    ListSplitter splitter(
        SplitLimits{.max_file_size_mb = 0.01, .overlap_s = 2, .min_chunk_s = 0, .silence_s = 0.1},
        std::vector<std::int16_t>(20000, 0), 1000);
    const auto chunks = splitter.split_audio_file("stream", {}, {});
    CHECK(chunks.size() > 1);
    CHECK(chunks.size() < 20);
    for (const auto& chunk : chunks) {
        CHECK(std::filesystem::file_size(chunk) <= static_cast<std::uintmax_t>(0.01 * 1024 * 1024));
    }
    splitter.cleanup_temp_files();
}

// from
// tests/test_audio_processor.py::TestSplitting::test_failure_or_cancel_removes_partial_files_and_closes_decoder
TEST_CASE("TestSplitting::test_failure_or_cancel_removes_partial_files_and_closes_decoder",
          "[test_audio_processor]") {
    const bool cancel = GENERATE(false, true);
    CAPTURE(cancel);
    const testkit::TempDataRoot data;
    const auto root = data.path() / "chunks";

    class Failing final : public UploadSplitter {
    public:
        Failing(std::filesystem::path root, bool cancel)
            : UploadSplitter(SplitLimits{.max_file_size_mb = 0.02}), root_(std::move(root)),
              cancel_(cancel) {}
        bool closed = false;
        CancelSource stop;

    protected:
        std::filesystem::path make_temp_dir() override {
            std::filesystem::create_directories(root_);
            return root_;
        }
        std::unique_ptr<BlockSource> open_blocks(const std::filesystem::path&,
                                                 const CancelToken&) override {
            class Endless final : public BlockSource {
            public:
                explicit Endless(bool& closed) : closed_(closed) {}
                ~Endless() override { closed_ = true; }
                bool next(std::vector<std::int16_t>& samples, int& rate, int& channels) override {
                    samples.assign(1000, 0);
                    rate = 8000;
                    channels = 1;
                    return true;
                }

            private:
                bool& closed_;
            };
            return std::make_unique<Endless>(closed);
        }
        void save_chunk(std::span<const std::int16_t>, int,
                        const std::filesystem::path& file) override {
            std::ofstream(file, std::ios::binary) << "partial";
            if (cancel_) {
                stop.cancel();
            } else {
                throw std::runtime_error("disk full");
            }
        }

    private:
        std::filesystem::path root_;
        bool cancel_;
    } splitter(root, cancel);

    std::string error;
    try {
        splitter.split_audio_file("stream", {}, splitter.stop.token());
    } catch (const std::exception& failure) {
        error = failure.what();
    }
    CHECK(error == (cancel ? "Transcription canceled" : "disk full"));
    CHECK(splitter.closed);
    CHECK_FALSE(std::filesystem::exists(root));
    CHECK(splitter.temp_files().empty());
}

// ---- TestPcmConversion ----

// from
// tests/test_audio_processor.py::TestPcmConversion::test_real_stereo_keeps_duration_and_source_channels
TEST_CASE("TestPcmConversion::test_real_stereo_keeps_duration_and_source_channels",
          "[test_audio_processor]") {
    const testkit::TempDataRoot root;
    const auto wav = root.path() / "stereo.wav";
    write_wav(wav, tone(1, 8000, 2), 8000, 2);
    TestSplitter splitter({}, root.path());
    int rate = 0;
    int channels = 0;
    const auto samples = splitter.decode_all(wav, rate, channels);
    CHECK(samples.size() == 8000);
    CHECK(rate == 8000);
    CHECK(channels == 2);
    // Equal stereo channels keep their waveform after the downmix.
    const auto expected = tone(1, 8000);
    REQUIRE(samples.size() == expected.size());
    for (std::size_t i = 0; i < samples.size(); ++i) {
        CHECK(std::abs(samples[i] - expected[i]) <= 2);
    }
}

// from
// tests/test_audio_processor.py::TestPcmConversion::test_integer_float_and_packed_planar_formats
TEST_CASE("TestPcmConversion::test_integer_float_and_packed_planar_formats",
          "[test_audio_processor]") {
    // Adapted: Python feeds PyAV frames of each format; here WAV files carry the packed ones
    // (s16, s32, flt, u8). The planar layouts only come out of compressed decoders, which the
    // decoder converts the same way.
    const std::string format = GENERATE("s16", "s32", "flt", "u8");
    const int channels = GENERATE(1, 2);
    CAPTURE(format, channels);
    const std::vector<std::int16_t> values{-16384, -8192, 0, 8192, 16384};
    std::string data;
    const auto append = [&data](const void* bytes, std::size_t size) {
        data.append(static_cast<const char*>(bytes), size);
    };
    for (std::size_t i = 0; i < values.size(); ++i) {
        for (int channel = 0; channel < channels; ++channel) {
            const std::int16_t value = channel == 0 ? values[i] : values[values.size() - 1 - i];
            if (format == "s16") {
                append(&value, 2);
            } else if (format == "s32") {
                const std::int32_t wide = static_cast<std::int32_t>(value) * 65536;
                append(&wide, 4);
            } else if (format == "flt") {
                const float real = static_cast<float>(value) / 32768.0F;
                append(&real, 4);
            } else {
                const auto narrow = static_cast<std::uint8_t>(floor_div(value, 256) + 128);
                append(&narrow, 1);
            }
        }
    }
    const testkit::TempDataRoot root;
    const auto wav = root.path() / "format.wav";
    const int bits = format == "s16" ? 16 : format == "u8" ? 8 : 32;
    write_raw_wav(wav, format == "flt" ? 3 : 1, bits, channels, 8000, data);
    TestSplitter splitter({}, root.path());
    int rate = 0;
    int source_channels = 0;
    const auto samples = splitter.decode_all(wav, rate, source_channels);
    CHECK(rate == 8000);
    CHECK(source_channels == channels);
    CHECK(samples == (channels == 1 ? values : std::vector<std::int16_t>(values.size(), 0)));
}

// ---- TestMovingAverage ----

// from tests/test_audio_processor.py::TestMovingAverage::test_matches_numpy_convolve
TEST_CASE("TestMovingAverage::test_matches_numpy_convolve", "[test_audio_processor][golden]") {
    // np.convolve's values were recorded by OpenWhisper's Python (services.audio_processor's
    // convolve vectors); the boxcar must meet them within 3e-8.
    golden::for_each("services.audio_processor", "convolve", [](const golden::Case& c) {
        const auto samples =
            floats(c.arg<std::string>(0), c.arg<std::size_t>(1), c.arg<std::uint32_t>(2));
        const auto actual = moving_average(samples, c.arg<std::size_t>(3));
        const auto expected = c.result<std::vector<double>>();
        REQUIRE(actual.size() == expected.size());
        for (std::size_t i = 0; i < actual.size(); ++i) {
            CHECK(golden::near(expected[i], actual[i], 3e-8));
        }
    });
}

// from tests/test_audio_processor.py::TestMovingAverage::test_matches_across_block_boundaries
TEST_CASE("TestMovingAverage::test_matches_across_block_boundaries", "[test_audio_processor]") {
    // The blockwise halo must not leave a seam every 4M samples. Samples are k / 2^24, so the
    // reference window sums are exact integers.
    const std::size_t window = 4410;
    const std::size_t n = (std::size_t{1} << 22) * 2 + 12345;
    std::vector<std::uint32_t> units(n);
    std::vector<float> samples(n);
    std::uint32_t state = 7;
    for (std::size_t i = 0; i < n; ++i) {
        state = state * 1664525U + 1013904223U;
        units[i] = state >> 8;
        samples[i] = static_cast<float>(units[i]) / 16777216.0F;
    }
    const auto actual = moving_average(samples, window);
    const std::size_t left = window / 2;
    const std::size_t right = (window - 1) / 2;
    std::vector<std::uint64_t> prefix(n + 1, 0);
    for (std::size_t i = 0; i < n; ++i) {
        prefix[i + 1] = prefix[i] + units[i];
    }
    double worst = 0;
    for (std::size_t i = 0; i < n; ++i) {
        const std::size_t lo = i > left ? i - left : 0;
        const std::size_t hi = std::min(n, i + right + 1);
        const double expected =
            static_cast<double>(prefix[hi] - prefix[lo]) / 16777216.0 / static_cast<double>(window);
        worst = std::max(worst, std::abs(expected - static_cast<double>(actual[i])));
    }
    CHECK(worst < 1e-6);
}

// from tests/test_audio_processor.py::TestMovingAverage::test_stays_far_below_the_silence_threshold
TEST_CASE("TestMovingAverage::test_stays_far_below_the_silence_threshold",
          "[test_audio_processor]") {
    const std::size_t window = 4410;
    std::mt19937 random(11);
    std::normal_distribution<double> normal;
    std::vector<float> samples(200000);
    for (auto& sample : samples) {
        sample = static_cast<float>(std::abs(normal(random)) / 4.0);
    }
    const auto actual = moving_average(samples, window);
    const std::size_t left = window / 2;
    const std::size_t right = (window - 1) / 2;
    double drift = 0;
    for (std::size_t i = 0; i < samples.size(); ++i) {
        // Direct sum: what np.convolve does.
        const std::size_t lo = i > left ? i - left : 0;
        const std::size_t hi = std::min(samples.size(), i + right + 1);
        double sum = 0;
        for (std::size_t k = lo; k < hi; k += 1) {
            sum += samples[k];
        }
        drift = std::max(drift, std::abs(sum / window - actual[i]));
        if (i % 97 != 0) {
            // Every 97th point is checked in full; the rest skip ahead.
            i += 96;
        }
    }
    CHECK(drift < SplitLimits{}.silence_threshold / 1000);
}

// from tests/test_audio_processor.py::TestMovingAverage::test_degenerate_inputs_are_passed_through
TEST_CASE("TestMovingAverage::test_degenerate_inputs_are_passed_through",
          "[test_audio_processor]") {
    CHECK(moving_average(std::vector<float>{}, 4410).empty());
    CHECK(moving_average(std::vector<float>{0.25F}, 1) == std::vector<float>{0.25F});
}

// from
// tests/test_audio_processor.py::TestMovingAverage::test_split_points_are_unchanged_by_the_faster_smoothing
TEST_CASE("TestMovingAverage::test_split_points_are_unchanged_by_the_faster_smoothing",
          "[test_audio_processor][golden]") {
    // Adapted: the split points Python computed (with its own smoothing) are replayed, so the
    // same audio must split in the same places here.
    golden::for_each("services.audio_processor", "_find_split_points", [](const golden::Case& c) {
        const int rate = c.arg<int>(2);
        const auto audio =
            pcm(c.arg<std::string>(0), static_cast<std::size_t>(c.arg<double>(1) * rate), rate,
                c.arg<std::uint32_t>(3));
        CHECK(find_split_points(audio, rate, limits_of(c.arg<Json>(4))) ==
              c.result<std::vector<std::size_t>>());
    });
}

// from tests/test_audio_processor.py::test_cleanup_survives_a_temp_file_it_cannot_remove
TEST_CASE("test_cleanup_survives_a_temp_file_it_cannot_remove", "[test_audio_processor]") {
    class Stuck final : public UploadSplitter {
    public:
        using UploadSplitter::UploadSplitter;

    protected:
        void remove_path(const std::filesystem::path& path) override {
            throw std::filesystem::filesystem_error(
                "in use", path, std::make_error_code(std::errc::permission_denied));
        }
    };
    struct Capture final : log::Sink {
        std::mutex mutex;
        std::string text;
        void write(const log::Record& record) override {
            std::lock_guard lock(mutex);
            text += std::string(record.message) + "\n";
        }
    };
    const testkit::TempDataRoot root;
    const auto wav = root.path() / "clip.wav";
    write_wav(wav, tone(3, 8000), 8000);
    Stuck splitter(SplitLimits{.max_file_size_mb = 0.02});
    const auto chunks = splitter.split_audio_file(wav, {}, {});
    REQUIRE_FALSE(splitter.temp_files().empty());
    auto capture = std::make_shared<Capture>();
    log::add_sink(capture);
    splitter.cleanup_temp_files();
    log::flush(std::chrono::milliseconds(2'000));
    log::remove_sink(capture);
    CHECK(splitter.temp_files().empty());
    {
        std::lock_guard lock(capture->mutex);
        CHECK(capture->text.find("Failed to cleanup temp file") != std::string::npos);
    }
    std::filesystem::remove_all(chunks.front().parent_path());
}

// from tests/test_chunking.py::test_chunking
TEST_CASE("test_chunking", "[test_chunking]") {
    // Adapted: Python runs this only when a manual test_chunking_audio.wav is present; here the
    // same steps run on a generated file over a shrunken limit.
    const testkit::TempDataRoot root;
    const auto wav = root.path() / "test_chunking_audio.wav";
    write_wav(wav, pcm("gaps", 8000 * 40, 8000, 3), 8000);
    TestSplitter splitter(SplitLimits{.max_file_size_mb = 0.2, .overlap_s = 1.0, .min_chunk_s = 5},
                          root.path());
    const auto [needs_splitting, megabytes] = splitter.check_file_size(wav);
    CHECK(needs_splitting);
    CHECK(megabytes > 0.2);
    std::vector<std::string> progress;
    const auto chunks = splitter.split_audio_file(
        wav, [&progress](const std::string& message) { progress.push_back(message); }, {});
    REQUIRE(chunks.size() > 1);
    CHECK(progress.front() == "Decoding and splitting audio...");
    CHECK(progress.back() == "Created audio chunk " + std::to_string(chunks.size()) + "...");
    std::vector<std::string> texts;
    for (std::size_t i = 0; i < chunks.size(); ++i) {
        texts.push_back("Mock transcription for chunk " + std::to_string(i + 1));
    }
    CHECK(combine_transcriptions(texts).starts_with("Mock transcription for chunk 1 Mock"));
    splitter.cleanup_temp_files();
    CHECK_FALSE(std::filesystem::exists(chunks.front()));
    CHECK_THROWS_AS(splitter.check_file_size(root.path() / "missing.wav"), EngineError);
}

// ---- golden vectors ----

TEST_CASE("the splitter's functions match Python", "[golden]") {
    const std::string module = "services.audio_processor";
    golden::for_each(module, "_moving_average", [](const golden::Case& c) {
        const auto samples =
            floats(c.arg<std::string>(0), c.arg<std::size_t>(1), c.arg<std::uint32_t>(2));
        const auto actual = moving_average(samples, c.arg<std::size_t>(3));
        const auto expected = c.result<std::vector<double>>();
        REQUIRE(actual.size() == expected.size());
        for (std::size_t i = 0; i < actual.size(); ++i) {
            // The same float32 values: the same blockwise float64 sums, rounded once.
            CHECK(static_cast<double>(actual[i]) == expected[i]);
        }
    });
    golden::for_each(module, "_chunk_limits", [](const golden::Case& c) {
        const auto limits = limits_of(c.arg<Json>(1));
        if (c.raises()) {
            CHECK(c.raises_type() == "ValueError");
            CHECK_THROWS_AS(chunk_limits(c.arg<int>(0), limits), std::invalid_argument);
            return;
        }
        const auto [max_samples, overlap] = chunk_limits(c.arg<int>(0), limits);
        CHECK(std::vector<std::size_t>{max_samples, overlap} ==
              c.result<std::vector<std::size_t>>());
    });
    golden::for_each(module, "_generate_time_based_splits", [](const golden::Case& c) {
        CHECK(time_based_splits(c.arg<std::size_t>(0), c.arg<int>(1), limits_of(c.arg<Json>(2))) ==
              c.result<std::vector<std::size_t>>());
    });
    golden::for_each(module, "_chunk_boundary", [](const golden::Case& c) {
        const int rate = c.arg<int>(2);
        const auto samples =
            pcm(c.arg<std::string>(0), c.arg<std::size_t>(1), rate, c.arg<std::uint32_t>(3));
        CHECK(chunk_boundary(samples, rate, c.arg<std::size_t>(4), c.arg<std::size_t>(5),
                             limits_of(c.arg<Json>(6))) == c.result<std::size_t>());
    });
    golden::for_each(module, "_find_best_silence", [](const golden::Case& c) {
        const int rate = c.arg<int>(2);
        const auto samples =
            pcm(c.arg<std::string>(0), c.arg<std::size_t>(1), rate, c.arg<std::uint32_t>(3));
        std::vector<float> absolute(samples.size());
        for (std::size_t i = 0; i < samples.size(); ++i) {
            absolute[i] = std::abs(static_cast<float>(samples[i])) / 32768.0F;
        }
        const auto smooth = moving_average(absolute, static_cast<std::size_t>(0.1 * rate));
        CHECK(find_best_silence(smooth, c.arg<std::size_t>(4), c.arg<std::size_t>(5),
                                c.arg<std::size_t>(6), rate, limits_of(c.arg<Json>(7))) ==
              c.result<std::optional<std::size_t>>());
    });
    golden::for_each(module, "combine_transcriptions", [](const golden::Case& c) {
        CHECK(combine_transcriptions(c.arg<std::vector<std::string>>(0)) ==
              c.result<std::string>());
    });
}

TEST_CASE("split_audio_file cuts fixture PCM where Python cuts it", "[golden]") {
    golden::for_each("services.audio_processor", "split_audio_file", [](const golden::Case& c) {
        const testkit::TempDataRoot root;
        const std::string kind = c.arg<std::string>(0);
        const int rate = c.arg<int>(2);
        const auto seed = c.arg<std::uint32_t>(3);
        const int channels = c.arg<int>(4);
        const auto frames = static_cast<std::size_t>(c.arg<double>(1) * rate);
        std::vector<std::int16_t> data;
        if (channels == 1) {
            data = pcm(kind, frames, rate, seed);
        } else {
            const auto left = pcm(kind, frames, rate, seed);
            const auto right = pcm(kind, frames, rate, seed + 1);
            for (std::size_t i = 0; i < frames; ++i) {
                data.push_back(left[i]);
                data.push_back(right[i]);
            }
        }
        const auto wav = root.path() / "signal.wav";
        write_wav(wav, data, rate, channels);
        TestSplitter splitter(limits_of(c.arg<Json>(5)), root.path());
        const auto chunks = splitter.split_audio_file(wav, {}, {});
        Json actual = Json::array();
        for (const auto& chunk : chunks) {
            const auto samples = read_chunk(chunk);
            actual.push_back(Json::array(
                {samples.size(),
                 sha256_hex(std::string_view(reinterpret_cast<const char*>(samples.data()),
                                             samples.size() * 2))}));
        }
        CHECK(golden::equal(c.result<Json>(), actual));
        splitter.cleanup_temp_files();
    });
}
