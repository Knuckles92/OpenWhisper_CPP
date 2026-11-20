#pragma once

#include <cstdint>
#include <optional>

#include "base/json.h"

// services/settings.py resolve_max_saved_recordings and resolve_max_saved_recordings_bytes:
// how many saved recordings, and how many bytes of them, History keeps. Both read through the
// typed keys, so unset or invalid values fall back to the defaults (20 recordings, 1024 MB).
// Affinity: thread-safe (pure functions over a settings snapshot).
namespace fv {

// The count limit (at least 1), or nullopt in the keep_all and size_limit modes.
std::optional<std::int64_t> resolve_max_saved_recordings(const Json& settings);
// max_saved_recordings_mb * 1024 * 1024 (at most INT64_MAX) in the size_limit mode, else nullopt.
std::optional<std::int64_t> resolve_max_saved_recordings_bytes(const Json& settings);

} // namespace fv
