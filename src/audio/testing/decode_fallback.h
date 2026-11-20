#pragma once

#include <filesystem>
#include <functional>

#include "audio/file_decoder.h"

namespace fv::audio::testing {

// The decoder probe_audio_header's decoding fallback opens (count_decoded), so a test can fail
// that open as a file locked between the header read and the decode does on Windows. Empty (the
// default) opens an AudioFileDecoder. Set it only while no preview runs.
using DecoderOpener = std::function<AudioFileDecoder(const std::filesystem::path&)>;
void set_decode_fallback_opener(DecoderOpener opener);

} // namespace fv::audio::testing
