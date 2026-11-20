#include "meeting/cloud_audio.h"

#include <algorithm>
#include <cmath>
#include <limits>

#include "base/base64.h"

namespace fv::meeting::cloud_audio {
namespace {

constexpr double kHeaderBytes = 1024.0;
constexpr double kOverhead = 1.05;

std::int64_t bounded_sample(double value, std::int64_t maximum) {
    if (std::isnan(value) || value <= 0) {
        return 0;
    }
    if (value >= static_cast<double>(maximum)) {
        return maximum;
    }
    return static_cast<std::int64_t>(std::nearbyint(value));
}

} // namespace

std::int64_t estimate_upload_bytes(double duration_s, int bitrate) {
    constexpr auto maximum = std::numeric_limits<std::int64_t>::max();
    const auto bytes =
        std::max(0.0, duration_s) * static_cast<double>(std::max(1, bitrate)) / 8.0 * kOverhead +
        kHeaderBytes;
    return bytes >= static_cast<double>(maximum) ? maximum : static_cast<std::int64_t>(bytes);
}

std::vector<Range> plan_windows(std::int64_t sample_count, int sample_rate,
                                std::int64_t byte_budget, int bitrate, double overlap_s) {
    if (sample_count <= 0) {
        return {};
    }
    const auto rate = static_cast<double>(std::max(1, sample_rate));
    if (estimate_upload_bytes(static_cast<double>(sample_count) / rate, bitrate) <= byte_budget) {
        return {{0, sample_count}};
    }
    const auto usable = std::max(1.0, static_cast<double>(byte_budget) - kHeaderBytes) / kOverhead;
    const auto seconds = std::max(10.0, usable * 8.0 / static_cast<double>(std::max(1, bitrate)));
    const auto length = std::max<std::int64_t>(1, bounded_sample(seconds * rate, sample_count));
    auto overlap = bounded_sample(overlap_s * rate, sample_count);
    if (overlap >= length) {
        overlap = length / 8;
    }
    std::vector<Range> windows;
    for (std::int64_t start = 0; start < sample_count;) {
        const auto end = start + std::min(length, sample_count - start);
        windows.emplace_back(start, end);
        if (end == sample_count) {
            break;
        }
        start = end - overlap;
    }
    return windows;
}

Range clip_sample_range(std::int64_t sample_count, int sample_rate, double start_s, double end_s,
                        double origin_s) {
    const auto count = std::max<std::int64_t>(0, sample_count);
    const auto rate = static_cast<std::int64_t>(std::max(1, sample_rate));
    auto start = bounded_sample((start_s - origin_s) * static_cast<double>(rate), count);
    auto end = bounded_sample((end_s - origin_s) * static_cast<double>(rate), count);
    if (end < start) {
        std::swap(start, end);
    }
    const auto minimum = 2 * rate;
    const auto maximum = 10 * rate;
    if (end - start < minimum) {
        const auto deficit = minimum - (end - start);
        start -= std::min(start, deficit / 2);
        end += std::min(count - end, deficit - deficit / 2);
        start -= std::min(start, minimum - (end - start));
        end += std::min(count - end, minimum - (end - start));
    } else if (end - start > maximum) {
        end = start + maximum;
    }
    return {start, end};
}

std::optional<std::string> cut_reference_clip(std::span<const std::int16_t> frames, int sample_rate,
                                              double start_s, double end_s, double origin_s,
                                              const Encoder& encode,
                                              std::string_view content_type) {
    const auto [start, end] = clip_sample_range(static_cast<std::int64_t>(frames.size()),
                                                sample_rate, start_s, end_s, origin_s);
    if (end <= start || !encode) {
        return std::nullopt;
    }
    try {
        const auto audio = encode(
            frames.subspan(static_cast<std::size_t>(start), static_cast<std::size_t>(end - start)),
            sample_rate);
        if (!audio.empty()) {
            return "data:" + std::string(content_type) + ";base64," + b64encode(audio);
        }
    } catch (const std::exception&) {
        // A reference is optional. The whole window is still available to the provider.
    }
    return std::nullopt;
}

} // namespace fv::meeting::cloud_audio
