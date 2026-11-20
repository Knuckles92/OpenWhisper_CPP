#pragma once

#include <cstdint>
#include <span>
#include <string>

namespace fv::audio {

bool has_upload_flac();
// Complete in-memory mono FLAC file. The samples remain local until the caller sends it.
std::string encode_upload_flac(std::span<const std::int16_t> samples, int sample_rate);

} // namespace fv::audio
