#include "bench/detail/peak_memory.h"

#include <sys/resource.h>

namespace fv::bench::detail {

std::optional<std::uint64_t> peak_resident_bytes() {
    rusage usage{};
    if (::getrusage(RUSAGE_SELF, &usage) != 0) {
        return std::nullopt;
    }
#ifdef __APPLE__
    return static_cast<std::uint64_t>(usage.ru_maxrss); // bytes on macOS
#else
    return static_cast<std::uint64_t>(usage.ru_maxrss) * 1024; // kilobytes on Linux
#endif
}

} // namespace fv::bench::detail
