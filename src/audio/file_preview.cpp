#include "audio/file_preview.h"

#include <cerrno>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <format>
#include <memory>
#include <system_error>
#include <utility>

#include "audio/detail/ffmpeg.h"
#include "audio/file_decoder.h"
#include "audio/testing/decode_fallback.h"
#include "base/identity.h"
#include "base/log.h"
#include "base/utf8.h"
#include "pyport/errors.h"
#include "pyport/number.h"

namespace fv::audio {

namespace {

constexpr log::Logger kLog("services.audio_processor");

constexpr double kBytesPerMb = 1024.0 * 1024.0;

[[noreturn]] void throw_not_found(const std::filesystem::path& path) {
    throw AudioFileNotFound(std::format("Audio file not found: {}", path_to_utf8(path)));
}

double size_mb(const std::filesystem::path& path) {
    // os.path.getsize of a folder is 0 on Windows, so a dropped folder reads as an empty file.
    if (std::filesystem::is_directory(path)) {
        return 0.0;
    }
    std::error_code error;
    const std::uintmax_t bytes = std::filesystem::file_size(path, error);
    if (error) {
        // A file deleted after the exists check: os.path.getsize raises FileNotFoundError, which
        // the Upload tab shows as "This file no longer exists.".
        if (error == std::errc::no_such_file_or_directory) {
            throw_not_found(path);
        }
        throw std::filesystem::filesystem_error("file_size", path, error);
    }
    return static_cast<double>(bytes) / kBytesPerMb;
}

void require_exists(const std::filesystem::path& path) {
    std::error_code error;
    // os.path.exists: false for a path that cannot be checked, never an error.
    if (!std::filesystem::exists(path, error)) {
        throw_not_found(path);
    }
}

std::string not_allowed_message() {
    return std::format("{} isn't allowed to read this file.", identity().product);
}

struct FormatCloser {
    void operator()(AVFormatContext* format) const { avformat_close_input(&format); }
};

// float(stream.duration * stream.time_base): the exact fraction rounded once, as Python's
// Fraction-to-float conversion does, where the product is exactly representable.
double stream_seconds(std::int64_t duration, AVRational time_base) {
    constexpr std::int64_t kExact = std::int64_t{1} << 53;
    const std::int64_t num = time_base.num;
    if (duration >= -kExact && duration <= kExact && num > -kExact && num < kExact &&
        (num == 0 || std::llabs(duration) <= kExact / std::llabs(num))) {
        return static_cast<double>(duration * num) / static_cast<double>(time_base.den);
    }
    return static_cast<double>(duration) * av_q2d(time_base);
}

// The samples of the first audio stream counted by decoding it (_iter_audio_blocks). Decoding at
// the stream's own rate holds nothing back, so the frames counted are the mono samples Python's
// converter yields.
testing::DecoderOpener& decoder_opener() {
    static testing::DecoderOpener opener;
    return opener;
}

AudioHeader count_decoded(const std::filesystem::path& path) {
    const testing::DecoderOpener& open = decoder_opener();
    AudioFileDecoder decoder = open ? open(path) : AudioFileDecoder(path);
    const int rate = decoder.rate();
    if (rate <= 0) {
        throw std::runtime_error("Invalid audio sample rate");
    }
    std::int64_t total = 0;
    DecodedFrame frame;
    while (decoder.next(frame)) {
        total += static_cast<std::int64_t>(frame.frames());
    }
    if (total == 0) {
        throw UnreadableAudio("This file has no audio in it.");
    }
    return {static_cast<double>(total) / rate, rate, decoder.channels()};
}

} // namespace

void testing::set_decode_fallback_opener(DecoderOpener opener) {
    decoder_opener() = std::move(opener);
}

bool AudioFilePreview::over_upload_limit() const {
    return file_size_mb > kMaxFileSizeMb;
}

std::string AudioFilePreview::duration_formatted() const {
    // services/format_utils.py format_audio_duration; fv_audio cannot reach fv_history's copy.
    double value = duration_seconds;
    if (value < 0) {
        value = 0.0;
    }
    if (value < 60) {
        return py::format_fixed(value, 1) + "s";
    }
    if (std::isnan(value)) {
        throw py::ValueError("cannot convert float NaN to integer");
    }
    if (std::isinf(value)) {
        throw py::OverflowError("cannot convert float infinity to integer");
    }
    const double rounded = py::round_half_even(value);
    if (rounded >= 9.223372036854775808e18) {
        // Past int64 Python's int keeps every digit: divide the exact decimal digits of the
        // whole double by 3600, as divmod does.
        std::string hours;
        int rem = 0;
        for (const char digit : py::format_fixed(rounded, 0)) {
            rem = rem * 10 + (digit - '0');
            if (!hours.empty() || rem / 3600 != 0) {
                hours += static_cast<char>('0' + rem / 3600);
            }
            rem %= 3600;
        }
        const int minutes = rem / 60;
        return minutes != 0 ? std::format("{}h {}m", hours, minutes) : hours + "h";
    }
    const auto total_seconds = static_cast<std::int64_t>(rounded);
    const std::int64_t hours = total_seconds / 3600;
    const std::int64_t rem = total_seconds % 3600;
    const std::int64_t minutes = rem / 60;
    const std::int64_t secs = rem % 60;
    if (hours > 0) {
        if (minutes != 0) {
            return std::format("{}h {}m", hours, minutes);
        }
        return std::format("{}h", hours);
    }
    return std::format("{}m {}s", minutes, secs);
}

std::string AudioFilePreview::file_size_formatted() const {
    if (file_size_mb >= 1) {
        return py::format_fixed(file_size_mb, 1) + " MB";
    }
    return py::format_fixed(file_size_mb * 1024, 0) + " KB";
}

FileSize check_file_size(const std::filesystem::path& path) {
    require_exists(path);
    const double mb = size_mb(path);
    const bool needs_splitting = mb > kMaxFileSizeMb;
    kLog.info("Audio file size: {:.2f} MB (limit: {} MB)", mb, kMaxFileSizeMb);
    if (needs_splitting) {
        kLog.info("File exceeds size limit, splitting will be required");
    }
    return {needs_splitting, mb};
}

AudioHeader probe_audio_header(const std::filesystem::path& path) {
    AudioHeader header;
    {
        AVFormatContext* raw = nullptr;
        const std::string utf8 = path_to_utf8(path);
        int result = avformat_open_input(&raw, utf8.c_str(), nullptr, nullptr);
        if (result < 0) {
            // PyAV raises its PermissionError for both errno values.
            if (result == AVERROR(EACCES) || result == AVERROR(EPERM)) {
                throw UnreadableAudio(not_allowed_message());
            }
            throw std::runtime_error(detail::av_error_text(result));
        }
        const std::unique_ptr<AVFormatContext, FormatCloser> format(raw);
        result = avformat_find_stream_info(format.get(), nullptr);
        if (result < 0) {
            throw std::runtime_error(detail::av_error_text(result));
        }
        const AVStream* stream = nullptr;
        for (unsigned i = 0; i < format->nb_streams; ++i) {
            if (format->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_AUDIO) {
                stream = format->streams[i];
                break;
            }
        }
        if (stream == nullptr) {
            throw UnreadableAudio("This file has no audio track.");
        }
        header.sample_rate = stream->codecpar->sample_rate;
        header.channels = stream->codecpar->ch_layout.nb_channels;
        // PyAV reads an unset duration as None and a zero time base as falsy.
        if (stream->duration != AV_NOPTS_VALUE && stream->time_base.num != 0 &&
            stream->time_base.den != 0) {
            header.duration_s = stream_seconds(stream->duration, stream->time_base);
        } else if (format->duration != AV_NOPTS_VALUE) {
            header.duration_s = static_cast<double>(format->duration) / AV_TIME_BASE;
        }
    }
    if (header.duration_s > 0 && header.sample_rate != 0) {
        return header;
    }
    kLog.info("No duration in the header of {}; counting decoded samples",
              path_to_utf8(path.filename()));
    return count_decoded(path);
}

AudioFilePreview preview_file(const std::filesystem::path& path, bool engine_splits,
                              const ChunkEstimator* estimator) {
    require_exists(path);
    AudioFilePreview preview;
    preview.file_path = path_to_utf8(path);
    preview.file_name = path_to_utf8(path.filename());
    preview.file_size_mb = size_mb(path);
    preview.needs_splitting = engine_splits && preview.file_size_mb > kMaxFileSizeMb;
    if (preview.file_size_mb == 0.0) {
        throw UnreadableAudio("This file is empty.");
    }

    AudioHeader header;
    try {
        header = probe_audio_header(path);
    } catch (const UnreadableAudio&) {
        throw;
    } catch (const AudioFileDenied&) {
        // The decoding fallback's PermissionError: the file was locked after its header was
        // read (another program opening it without sharing, on Windows).
        throw UnreadableAudio(not_allowed_message());
    } catch (const std::exception& error) {
        kLog.info("Could not read {} as audio: {}", preview.file_name, error.what());
        throw UnreadableAudio(std::string(NOT_AUDIO_MESSAGE));
    }
    preview.duration_seconds = header.duration_s;
    preview.sample_rate = header.sample_rate;
    preview.channels = header.channels;

    if (preview.needs_splitting && estimator != nullptr && *estimator) {
        const auto total_samples =
            static_cast<std::int64_t>(py::round_half_even(header.duration_s * header.sample_rate));
        std::vector<std::int64_t> ends = (*estimator)(total_samples, header.sample_rate);
        ends.push_back(total_samples);
        std::int64_t start = 0;
        for (const std::int64_t end : ends) {
            preview.chunk_durations.push_back(static_cast<double>(end - start) /
                                              header.sample_rate);
            start = end;
        }
        preview.estimated_chunks = static_cast<int>(preview.chunk_durations.size());
    } else {
        preview.estimated_chunks = 1;
        preview.chunk_durations = {header.duration_s};
    }

    kLog.info("Audio preview: {}, {:.2f} MB, {:.1f}s, {} chunk(s)", preview.file_name,
              preview.file_size_mb, preview.duration_seconds, preview.estimated_chunks);
    return preview;
}

} // namespace fv::audio
