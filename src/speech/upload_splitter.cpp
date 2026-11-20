#include "speech/upload_splitter.h"

#include <algorithm>
#include <cmath>
#include <format>
#include <limits>
#include <random>
#include <stdexcept>
#include <system_error>

#include "audio/file_decoder.h"
#include "audio/wav_writer.h"
#include "base/log.h"
#include "base/utf8.h"
#include "engine/error.h"
#include "pyport/number.h"
#include "pyport/text.h"

namespace fv::speech {

namespace {

constexpr log::Logger kLog{"services.audio_processor"};

// Samples per cumulative-sum block: bounds the float64 working set to about 32 MB however long
// the recording is (a whole-signal cumsum needs 8 bytes a sample, 1.3 GB for an hour).
constexpr std::size_t kSmoothBlockSamples = std::size_t{1} << 22;
// numpy's PW_BLOCKSIZE.
constexpr std::size_t kPairwiseBlock = 128;
// A mono PCM WAV written by Python's wave module has a 44-byte header.
constexpr std::int64_t kWavHeaderBytes = 44;

void raise_if_canceled(const CancelToken& cancel) {
    if (cancel.cancelled()) {
        throw EngineError("Transcription canceled");
    }
}

// numpy's float32 pairwise summation (the add.reduce inner loop), so a mean rounds as
// np.mean's does.
float pairwise_sum(const float* values, std::size_t n) {
    if (n < 8) {
        float result = 0.0F;
        for (std::size_t i = 0; i < n; ++i) {
            result += values[i];
        }
        return result;
    }
    if (n <= kPairwiseBlock) {
        float r[8];
        for (std::size_t j = 0; j < 8; ++j) {
            r[j] = values[j];
        }
        std::size_t i = 8;
        for (; i < n - (n % 8); i += 8) {
            for (std::size_t j = 0; j < 8; ++j) {
                r[j] += values[i + j];
            }
        }
        float result = ((r[0] + r[1]) + (r[2] + r[3])) + ((r[4] + r[5]) + (r[6] + r[7]));
        for (; i < n; ++i) {
            result += values[i];
        }
        return result;
    }
    std::size_t half = n / 2;
    half -= half % 8;
    return pairwise_sum(values, half) + pairwise_sum(values + half, n - half);
}

// int() of a non-negative float, as Python truncates.
std::int64_t truncated(double value) {
    return static_cast<std::int64_t>(value);
}

// config.MAX_FILE_SIZE_MB as Python prints it: "23", "0.05".
std::string limit_text(double megabytes) {
    if (megabytes == std::floor(megabytes) && std::abs(megabytes) < 1e15) {
        return std::to_string(static_cast<std::int64_t>(megabytes));
    }
    return py::float_repr(megabytes);
}

// What PyAV's AudioResampler(format="s16", layout="mono", rate=<native>) makes of interleaved
// int16: two channels mixed by swresample's integer matrix (16384 each, rounded with +16384 and
// an arithmetic shift of 15); more channels averaged with the same rounding.
void downmix(std::vector<std::int16_t>& interleaved, int channels) {
    if (channels <= 1) {
        return;
    }
    const std::size_t frames = interleaved.size() / static_cast<std::size_t>(channels);
    for (std::size_t frame = 0; frame < frames; ++frame) {
        std::int64_t sum = 0;
        for (int channel = 0; channel < channels; ++channel) {
            sum += interleaved[frame * static_cast<std::size_t>(channels) +
                               static_cast<std::size_t>(channel)];
        }
        std::int64_t mixed = 0;
        if (channels == 2) {
            mixed = (sum * 16384 + 16384) >> 15;
        } else {
            const auto coefficient = static_cast<std::int64_t>(std::lrint(32768.0 / channels));
            mixed = (sum * coefficient + 16384) >> 15;
        }
        interleaved[frame] = static_cast<std::int16_t>(
            std::clamp<std::int64_t>(mixed, std::numeric_limits<std::int16_t>::min(),
                                     std::numeric_limits<std::int16_t>::max()));
    }
    interleaved.resize(frames);
}

class DecoderBlocks final : public UploadSplitter::BlockSource {
public:
    DecoderBlocks(const std::filesystem::path& audio, CancelToken cancel)
        : decoder_(audio), cancel_(std::move(cancel)) {
        if (decoder_.rate() <= 0) {
            throw std::invalid_argument("Invalid audio sample rate");
        }
    }

    bool next(std::vector<std::int16_t>& samples, int& rate, int& channels) override {
        while (true) {
            raise_if_canceled(cancel_);
            samples.clear();
            if (!decoder_.read_s16(samples)) {
                if (!found_audio_) {
                    throw EngineError("This file has no audio in it.");
                }
                return false;
            }
            downmix(samples, decoder_.channels());
            if (!samples.empty()) {
                found_audio_ = true;
                rate = decoder_.rate();
                channels = decoder_.channels();
                return true;
            }
        }
    }

private:
    audio::AudioFileDecoder decoder_;
    CancelToken cancel_;
    bool found_audio_ = false;
};

} // namespace

std::vector<float> moving_average(std::span<const float> samples, std::size_t window) {
    const std::size_t n = samples.size();
    if (window <= 1 || n == 0) {
        return std::vector<float>(samples.begin(), samples.end());
    }
    const std::size_t left = window / 2;
    const std::size_t right = (window - 1) / 2;
    const std::size_t span = left + right + 1;
    const std::size_t step = std::max(kSmoothBlockSamples, window);
    std::vector<float> out(n);
    std::vector<double> block;
    for (std::size_t start = 0; start < n; start += step) {
        const std::size_t stop = std::min(start + step, n);
        const std::size_t lo = start > left ? start - left : 0;
        const std::size_t hi = std::min(n, stop + right);
        const std::size_t pad_lo = left - (start - lo);
        const std::size_t pad_hi = right - (hi - stop);
        // Where the halo was clipped by an end of the signal, the sum extends flat: zeros
        // before the start, the final total after it.
        block.assign(pad_lo + (hi - lo + 1) + pad_hi, 0.0);
        double total = 0.0;
        for (std::size_t k = 0; k < hi - lo; ++k) {
            total += static_cast<double>(samples[lo + k]);
            block[pad_lo + k + 1] = total;
        }
        std::fill(block.begin() + static_cast<std::ptrdiff_t>(pad_lo + (hi - lo + 1)), block.end(),
                  total);
        const std::size_t width = stop - start;
        for (std::size_t j = 0; j < width; ++j) {
            out[start + j] =
                static_cast<float>((block[span + j] - block[j]) / static_cast<double>(window));
        }
    }
    return out;
}

std::pair<std::size_t, std::size_t> chunk_limits(int rate, const SplitLimits& limits) {
    const std::int64_t max_samples =
        (truncated(limits.max_file_size_mb * 1024 * 1024) - kWavHeaderBytes) / 2;
    if (max_samples < 4) {
        throw std::invalid_argument("Upload size limit is too small for a WAV file");
    }
    const std::int64_t overlap =
        std::min(std::max<std::int64_t>(0, truncated(limits.overlap_s * rate)), max_samples / 4);
    return {static_cast<std::size_t>(max_samples), static_cast<std::size_t>(overlap)};
}

std::optional<std::size_t> find_best_silence(std::span<const float> smooth, std::size_t start,
                                             std::size_t end, std::size_t silence_samples, int rate,
                                             const SplitLimits& limits, const CancelToken& cancel) {
    const auto step = std::max<std::int64_t>(1, truncated(0.1 * rate));
    std::optional<std::size_t> best_start;
    float best_quality = std::numeric_limits<float>::infinity();
    const auto threshold = static_cast<float>(limits.silence_threshold);
    for (std::int64_t i =
             static_cast<std::int64_t>(end) - static_cast<std::int64_t>(silence_samples);
         i > static_cast<std::int64_t>(start); i -= step) {
        raise_if_canceled(cancel);
        const auto at = static_cast<std::size_t>(i);
        if (at + silence_samples >= smooth.size()) {
            continue;
        }
        const float* region = smooth.data() + at;
        const float max_level = *std::max_element(region, region + silence_samples);
        if (max_level < threshold) {
            // np.mean of float32: 0 plus the pairwise sum, divided in float32.
            const float average = (0.0F + pairwise_sum(region, silence_samples)) /
                                  static_cast<float>(silence_samples);
            const float quality = average + max_level * 0.1F;
            if (quality < best_quality) {
                best_quality = quality;
                best_start = at + silence_samples / 2;
            }
        }
    }
    return best_start;
}

std::size_t chunk_boundary(std::span<const std::int16_t> window, int rate, std::size_t prefix,
                           std::size_t overlap, const SplitLimits& limits,
                           const CancelToken& cancel) {
    const auto end = static_cast<std::int64_t>(window.size()) - static_cast<std::int64_t>(overlap);
    // Leaves room to advance even if the minimum duration is longer than a permitted upload
    // (very high sample rates).
    const std::int64_t minimum = std::max<std::int64_t>(1, truncated(limits.min_chunk_s * rate));
    const auto prefix_at = static_cast<std::int64_t>(prefix);
    const std::int64_t start = std::max({static_cast<std::int64_t>(overlap) + 1, prefix_at + 1,
                                         std::min(prefix_at + minimum, end)});
    const std::int64_t silence = std::max<std::int64_t>(1, truncated(limits.silence_s * rate));
    if (end - start >= silence) {
        std::vector<float> absolute(window.size());
        for (std::size_t i = 0; i < window.size(); ++i) {
            absolute[i] = std::abs(static_cast<float>(window[i])) / 32768.0F;
        }
        const auto smooth = moving_average(
            absolute, static_cast<std::size_t>(std::max<std::int64_t>(1, truncated(0.1 * rate))));
        if (const auto found = find_best_silence(
                smooth, static_cast<std::size_t>(start), static_cast<std::size_t>(end),
                static_cast<std::size_t>(silence), rate, limits, cancel)) {
            return *found;
        }
    }
    return static_cast<std::size_t>(std::max<std::int64_t>(0, end));
}

std::vector<std::size_t> find_split_points(std::span<const std::int16_t> audio, int rate,
                                           const SplitLimits& limits) {
    const auto [max_samples, overlap] = chunk_limits(rate, limits);
    std::vector<std::size_t> points;
    std::size_t start = 0;
    std::size_t prefix = 0;
    while (audio.size() - start > max_samples) {
        const std::size_t cut = start + chunk_boundary(audio.subspan(start, max_samples), rate,
                                                       prefix, overlap, limits);
        points.push_back(cut);
        start = cut - overlap;
        prefix = overlap;
    }
    return points;
}

std::vector<std::size_t> time_based_splits(std::size_t total_samples, int rate,
                                           const SplitLimits& limits) {
    const auto [max_samples, overlap] = chunk_limits(rate, limits);
    std::vector<std::size_t> points;
    std::size_t position = 0;
    while (total_samples - position > max_samples) {
        const std::size_t cut = position + max_samples - overlap;
        points.push_back(cut);
        position = cut - overlap;
    }
    return points;
}

std::string combine_transcriptions(std::span<const std::string> transcriptions) {
    std::string combined;
    bool first = true;
    for (const std::string& transcription : transcriptions) {
        std::string text = py::strip(transcription);
        if (text.empty()) {
            continue;
        }
        if (!first && !combined.ends_with(' ') && !text.starts_with(' ')) {
            combined += ' ';
        }
        combined += text;
        first = false;
    }
    std::string collapsed;
    for (const char c : combined) {
        if (!(c == ' ' && collapsed.ends_with(' '))) {
            collapsed += c;
        }
    }
    return py::strip(collapsed);
}

UploadSplitter::UploadSplitter(SplitLimits limits) : limits_(limits) {}

UploadSplitter::~UploadSplitter() = default;

std::pair<bool, double> UploadSplitter::check_file_size(const std::filesystem::path& audio) const {
    std::error_code error;
    if (!std::filesystem::exists(audio, error)) {
        throw EngineError("Audio file not found: " + path_to_utf8(audio));
    }
    const auto bytes = std::filesystem::file_size(audio, error);
    if (error) {
        throw EngineError("Audio file not found: " + path_to_utf8(audio));
    }
    const double megabytes = static_cast<double>(bytes) / (1024.0 * 1024.0);
    const bool needs_splitting = megabytes > limits_.max_file_size_mb;
    kLog.info("Audio file size: {} MB (limit: {} MB)", py::format_fixed(megabytes, 2),
              limit_text(limits_.max_file_size_mb));
    if (needs_splitting) {
        kLog.info("File exceeds size limit, splitting will be required");
    }
    return {needs_splitting, megabytes};
}

std::vector<std::filesystem::path>
UploadSplitter::split_audio_file(const std::filesystem::path& audio, const Progress& progress,
                                 const CancelToken& cancel) {
    std::vector<std::filesystem::path> chunks;
    std::optional<std::filesystem::path> temp_dir;
    try {
        raise_if_canceled(cancel);
        if (progress) {
            progress("Decoding and splitting audio...");
        }
        temp_dir = make_temp_dir();
        // Registered at once: a failed write can leave a partial file.
        temp_files_.push_back(*temp_dir);
        const auto save = [&](std::span<const std::int16_t> samples, int rate) {
            raise_if_canceled(cancel);
            const auto file = *temp_dir / std::format("chunk_{:03}.wav", chunks.size());
            save_chunk(samples, rate, file);
            chunks.push_back(file);
            if (progress) {
                progress(std::format("Created audio chunk {}...", chunks.size()));
            }
        };
        {
            const auto blocks = open_blocks(audio, cancel);
            split_blocks(*blocks, save, cancel);
        }
        raise_if_canceled(cancel);
        kLog.info("Successfully split audio into {} chunks", chunks.size());
        return chunks;
    } catch (const std::exception& error) {
        kLog.error("Failed to split audio file: {}", error.what());
        // Only this operation's files are owned here.
        if (temp_dir) {
            discard_temp_dir(*temp_dir);
        }
        throw;
    }
}

void UploadSplitter::split_blocks(
    BlockSource& blocks, const std::function<void(std::span<const std::int16_t>, int)>& save,
    const CancelToken& cancel) {
    std::vector<std::int16_t> pending;
    std::vector<std::int16_t> samples;
    std::size_t max_samples = 0;
    std::size_t overlap = 0;
    std::size_t count = 0;
    std::size_t prefix = 0;
    int rate = 0;
    int channels = 0;
    bool started = false;
    while (blocks.next(samples, rate, channels)) {
        raise_if_canceled(cancel);
        if (!started) {
            std::tie(max_samples, overlap) = chunk_limits(rate, limits_);
            // One window plus one sample of look-ahead, so an exact-sized final chunk does not
            // leave a chunk of overlap alone.
            pending.resize(max_samples + 1);
            started = true;
        }
        std::size_t cursor = 0;
        while (cursor < samples.size()) {
            raise_if_canceled(cancel);
            const std::size_t take = std::min(pending.size() - count, samples.size() - cursor);
            std::copy_n(samples.begin() + static_cast<std::ptrdiff_t>(cursor), take,
                        pending.begin() + static_cast<std::ptrdiff_t>(count));
            count += take;
            cursor += take;
            if (count <= max_samples) {
                continue;
            }
            const std::size_t cut =
                boundary(std::span<const std::int16_t>(pending.data(), max_samples), rate, prefix,
                         overlap, cancel);
            save(std::span<const std::int16_t>(pending.data(), cut + overlap), rate);
            const std::size_t retain = cut - overlap;
            const std::size_t remaining = count - retain;
            std::copy(pending.begin() + static_cast<std::ptrdiff_t>(retain),
                      pending.begin() + static_cast<std::ptrdiff_t>(count), pending.begin());
            count = remaining;
            prefix = overlap;
        }
    }
    if (count != 0) {
        save(std::span<const std::int16_t>(pending.data(), count), rate);
    }
}

void UploadSplitter::discard_temp_dir(const std::filesystem::path& dir) {
    std::error_code ignored;
    std::filesystem::remove_all(dir, ignored);
    std::erase(temp_files_, dir);
}

void UploadSplitter::cleanup_temp_files() {
    for (const auto& path : temp_files_) {
        try {
            remove_path(path);
        } catch (const std::filesystem::filesystem_error& error) {
            kLog.warning("Failed to cleanup temp file {}: {}", path_to_utf8(path), error.what());
        }
    }
    temp_files_.clear();
    kLog.info("Temporary files cleaned up");
}

std::unique_ptr<UploadSplitter::BlockSource>
UploadSplitter::open_blocks(const std::filesystem::path& audio, const CancelToken& cancel) {
    return std::make_unique<DecoderBlocks>(audio, cancel);
}

void UploadSplitter::save_chunk(std::span<const std::int16_t> samples, int rate,
                                const std::filesystem::path& file) {
    audio::WavWriter writer(file, audio::WavWriter::Format{rate, 1, 2});
    writer.write(samples);
    writer.commit(std::nullopt);
}

std::filesystem::path UploadSplitter::make_temp_dir() {
    // tempfile's _RandomNameSequence alphabet and length.
    static constexpr std::string_view kCharacters = "abcdefghijklmnopqrstuvwxyz0123456789_";
    thread_local std::mt19937 random{std::random_device{}()};
    std::uniform_int_distribution<std::size_t> pick(0, kCharacters.size() - 1);
    const auto root = std::filesystem::temp_directory_path();
    for (int attempt = 0; attempt < 100; ++attempt) {
        std::string name = "audio_chunks_";
        for (int i = 0; i < 8; ++i) {
            name += kCharacters[pick(random)];
        }
        const auto dir = root / name;
        std::error_code error;
        if (std::filesystem::create_directory(dir, error)) {
            return dir;
        }
        if (error) {
            throw std::filesystem::filesystem_error("Could not create a temporary folder", dir,
                                                    error);
        }
    }
    throw std::filesystem::filesystem_error("No usable temporary folder name found", root,
                                            std::make_error_code(std::errc::file_exists));
}

void UploadSplitter::remove_path(const std::filesystem::path& path) {
    std::error_code error;
    if (std::filesystem::is_regular_file(path, error)) {
        std::filesystem::remove(path, error);
    } else if (std::filesystem::is_directory(path, error)) {
        std::filesystem::remove_all(path, error);
    } else {
        return;
    }
    if (error) {
        throw std::filesystem::filesystem_error("Could not remove", path, error);
    }
}

std::size_t UploadSplitter::boundary(std::span<const std::int16_t> window, int rate,
                                     std::size_t prefix, std::size_t overlap,
                                     const CancelToken& cancel) {
    return chunk_boundary(window, rate, prefix, overlap, limits_, cancel);
}

} // namespace fv::speech
