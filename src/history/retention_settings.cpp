#include "history/retention_settings.h"

#include <limits>

#include "settings/keys.h"

namespace fv {

std::optional<std::int64_t> resolve_max_saved_recordings(const Json& settings) {
    const std::string mode = keys::recording_retention_mode.read(settings);
    if (mode == "keep_all" || mode == "size_limit") {
        return std::nullopt;
    }
    return keys::max_saved_recordings.read(settings);
}

std::optional<std::int64_t> resolve_max_saved_recordings_bytes(const Json& settings) {
    if (keys::recording_retention_mode.read(settings) != "size_limit") {
        return std::nullopt;
    }
    constexpr std::int64_t mib = 1024 * 1024;
    const std::int64_t megabytes = keys::max_saved_recordings_mb.read(settings);
    // Python's int has no ceiling; past int64 the cap is beyond any folder anyway.
    if (megabytes > std::numeric_limits<std::int64_t>::max() / mib) {
        return std::numeric_limits<std::int64_t>::max();
    }
    return megabytes * mib;
}

} // namespace fv
