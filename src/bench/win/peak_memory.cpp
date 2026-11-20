#include "bench/detail/peak_memory.h"

#include <windows.h>

#include <psapi.h>

namespace fv::bench::detail {

std::optional<std::uint64_t> peak_resident_bytes() {
    PROCESS_MEMORY_COUNTERS counters{};
    counters.cb = sizeof(counters);
    if (!::GetProcessMemoryInfo(::GetCurrentProcess(), &counters, sizeof(counters))) {
        return std::nullopt;
    }
    return static_cast<std::uint64_t>(counters.PeakWorkingSetSize);
}

} // namespace fv::bench::detail
