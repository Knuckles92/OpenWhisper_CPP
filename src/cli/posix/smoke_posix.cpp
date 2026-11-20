#include "cli/detail/text_mode.h"
#include "cli/workflow_smoke.h"

#include <sys/resource.h>

namespace fv::cli {

std::optional<double> peak_rss_mb() {
    rusage usage{};
    if (getrusage(RUSAGE_SELF, &usage) != 0) {
        return std::nullopt;
    }
    // ru_maxrss is in bytes on macOS and in kilobytes on Linux.
#if defined(__APPLE__)
    return static_cast<double>(usage.ru_maxrss) / 1'000'000;
#else
    return static_cast<double>(usage.ru_maxrss) / 1000;
#endif
}

std::string_view detail::text_mode_newline() {
    return "\n";
}

} // namespace fv::cli
