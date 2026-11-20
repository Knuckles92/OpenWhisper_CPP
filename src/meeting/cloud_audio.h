#pragma once

#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace fv::meeting::cloud_audio {

inline constexpr int kDefaultBitrate = 32'000;
inline constexpr std::int64_t kDefaultByteBudget = 20 * 1024 * 1024;
inline constexpr double kDefaultWindowOverlapS = 2.0;

using Range = std::pair<std::int64_t, std::int64_t>;
using Encoder = std::function<std::string(std::span<const std::int16_t>, int)>;

std::int64_t estimate_upload_bytes(double duration_s, int bitrate = kDefaultBitrate);
std::vector<Range> plan_windows(std::int64_t sample_count, int sample_rate,
                                std::int64_t byte_budget = kDefaultByteBudget,
                                int bitrate = kDefaultBitrate,
                                double overlap_s = kDefaultWindowOverlapS);
Range clip_sample_range(std::int64_t sample_count, int sample_rate, double start_s, double end_s,
                        double origin_s = 0);
std::optional<std::string> cut_reference_clip(std::span<const std::int16_t> frames, int sample_rate,
                                              double start_s, double end_s, double origin_s,
                                              const Encoder& encode, std::string_view content_type);

} // namespace fv::meeting::cloud_audio
