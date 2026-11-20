#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include "base/json.h"

// The decisions of Settings → Recording → retention (settings_dialog.py _commit_retention and
// _confirm_recording_removal): which limits an edit sets, whether committing it must ask History
// which recordings it deletes, and the confirmation's words. The page (M2.19) passes the limits
// to HistoryManager::recordings_over_limit and asks only when that list is not empty.
// Affinity: thread-safe (pure functions).
namespace fv::appcore {

// (resolve_max_saved_recordings, resolve_max_saved_recordings_bytes) of one settings state.
struct RetentionLimits {
    std::optional<std::int64_t> max_recordings;
    std::optional<std::int64_t> max_bytes;
    bool operator==(const RetentionLimits&) const = default;
};

// The limits of a saved settings mapping.
RetentionLimits retention_limits(const Json& settings);
// The limits the page's three controls would save ({**saved, **edits} reads only these three
// keys): each value goes through its typed key, so an unknown mode or a count below 1 reads as
// the key does.
RetentionLimits retention_limits(const Json& mode, const Json& count, const Json& mb);
// The settings patch that saves those three values together, in Python's key order.
Json retention_edits(const Json& mode, const Json& count, const Json& mb);

// Whether committing `pending` over `saved` must ask History what it would delete: the limits
// changed and at least one applies. Unchanged limits save nothing; changed limits without any
// cap save without asking.
bool shrinks(const RetentionLimits& saved, const RetentionLimits& pending);
// Whether a commit saves anything at all (the limits changed).
bool retention_changed(const RetentionLimits& saved, const RetentionLimits& pending);

// The confirmation for `count` recordings (at least 1) totalling `bytes`:
// "Delete N saved recording(s)?" and its body with format_file_size(bytes).
std::string delete_confirm_title(std::int64_t count);
std::string delete_confirm_text(std::int64_t count, std::int64_t bytes);
// "Currently N recording(s) using X." under the retention controls.
std::string recordings_usage_text(std::int64_t count, std::int64_t bytes);
// The message line when History cannot preview the deletion.
std::string retention_check_failed(std::string_view error);

} // namespace fv::appcore
