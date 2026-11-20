#pragma once

#include <array>
#include <string>
#include <string_view>

#include "base/env.h"
#include "settings/store.h"

// Whether model downloads may contact Hugging Face (services/settings.py:1341-1395 and
// HuggingFaceAccessPolicy). Cached models always load; the policy only governs downloads. The
// consent flow that reads it is not here.
namespace fv::models {

namespace hf_access_policy {
inline constexpr std::string_view kAsk = "ask";       // prompt before downloading a missing model
inline constexpr std::string_view kAlways = "always"; // download missing models without asking
inline constexpr std::string_view kNever = "never";   // stay offline unless overridden once
inline constexpr std::array<std::string_view, 3> kAll{kAsk, kAlways, kNever};
} // namespace hf_access_policy

// The saved policy. A legacy hf_hub_offline (true: "never"; false or absent: "ask") or an
// invalid saved policy is migrated: the result is saved and hf_hub_offline removed (a failed save
// is logged, not thrown).
std::string load_hf_access_policy(SettingsStore& store);

// Saves `policy` and removes hf_hub_offline. Throws std::invalid_argument "Invalid HF access
// policy '<x>'. Valid values: ['ask', 'always', 'never']" for anything else.
void save_hf_access_policy(SettingsStore& store, std::string_view policy);

// Whether HF_HUB_OFFLINE is "1", "on", "true" or "yes" (stripped, case-insensitive): a hard
// override that disables every model download, whatever the saved policy.
bool is_hf_hub_offline_env_set(const EnvReader& env);

} // namespace fv::models
