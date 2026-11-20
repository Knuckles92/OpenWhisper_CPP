#pragma once

#include <cstdint>
#include <optional>
#include <string>

namespace fv::speech::detail {

// _final_windows' last check: the swr flush lands within a sample of the ideal count for any
// input over 1000 frames, so a resampled total `total` (16 kHz samples) of a WAV holding
// `frames` at `rate` that is a sample or more away from frames * 16000 / rate is not the file's
// stream. The mismatch sentence, or nullopt when they agree.
std::optional<std::string> resampled_length_mismatch(std::int64_t total, std::int64_t frames,
                                                     int rate);

} // namespace fv::speech::detail
