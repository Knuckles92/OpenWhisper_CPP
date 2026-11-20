#pragma once

#include <cstddef>
#include <string>
#include <vector>

#include "base/json.h"

namespace fv::text::detail {

// config.MAX_TRANSCRIPT_CLEANUP_RULES.
inline constexpr std::size_t kMaxCleanupRules = 50;

// services/settings.py resolve_transcript_cleanup_rules: the stored list's strings, stripped,
// blank ones dropped, capped at kMaxCleanupRules.
std::vector<std::string> cleanup_rules(const Json& settings);

} // namespace fv::text::detail
