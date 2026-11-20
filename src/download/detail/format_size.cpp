#include "download/detail/format_size.h"

#include <format>

namespace fv::download::detail {

std::string format_size_bytes(std::int64_t size_bytes) {
    // std::format's fixed precision rounds the exact binary value, as Python's format() does.
    if (size_bytes >= 1'000'000'000) {
        return std::format("{:.2f} GB", static_cast<double>(size_bytes) / 1'000'000'000);
    }
    if (size_bytes >= 1'000'000) {
        return std::format("{:.0f} MB", static_cast<double>(size_bytes) / 1'000'000);
    }
    if (size_bytes >= 1'000) {
        return std::format("{:.0f} KB", static_cast<double>(size_bytes) / 1'000);
    }
    return std::format("{} B", size_bytes);
}

} // namespace fv::download::detail
