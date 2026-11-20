#include "diarize/detail/ort_loader.h"

namespace fv::diarize::detail {

std::expected<void*, std::string> load_ort_get_api_base(const std::filesystem::path&) {
    return std::unexpected(
        std::string("Speaker identification is not supported on this platform."));
}

} // namespace fv::diarize::detail
