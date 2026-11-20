#pragma once

#include <expected>
#include <filesystem>
#include <string>

namespace fv::diarize::detail {

// Loads the library at `absolute` (searching its dependencies in its own folder and System32
// only) and returns its exported OrtGetApiBase. The library is never unloaded. The error is a
// sentence. Implemented in win/ and, as "not supported", in posix/.
std::expected<void*, std::string> load_ort_get_api_base(const std::filesystem::path& absolute);

} // namespace fv::diarize::detail
