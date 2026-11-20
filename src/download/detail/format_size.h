#pragma once

#include <cstdint>
#include <string>

namespace fv::download::detail {

// services.format_utils.format_size_bytes: decimal units, GB to two places, MB and KB to none
// ("1.73 GB", "736 MB", "2 KB", "999 B"). history/format_utils.h owns the shared copy; fv_download
// may not depend on fv_history.
std::string format_size_bytes(std::int64_t size_bytes);

} // namespace fv::download::detail
