#include "cli/detail/text_mode.h"
#include "cli/workflow_smoke.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <psapi.h>

namespace fv::cli {

std::optional<double> peak_rss_mb() {
    PROCESS_MEMORY_COUNTERS counters{};
    counters.cb = sizeof(counters);
    if (!GetProcessMemoryInfo(GetCurrentProcess(), &counters, sizeof(counters))) {
        return std::nullopt;
    }
    return static_cast<double>(counters.PeakWorkingSetSize) / 1'000'000;
}

std::string_view detail::text_mode_newline() {
    return "\r\n";
}

} // namespace fv::cli
