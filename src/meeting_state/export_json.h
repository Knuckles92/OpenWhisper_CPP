#pragma once

#include <array>
#include <string>
#include <string_view>
#include <vector>

#include "base/json.h"

// meeting/export/json_export.py: the complete machine-readable meeting record. Unlike the
// Markdown export it keeps everything (the full state document with its evidence ids, every
// segment and the meeting row) except the row fields that must never leave the app: the
// capability tokens, the process-liveness bookkeeping and the state_json snapshot the exported
// state duplicates. Every other row field stays, spool_dir included (lead decision 8).
// Affinity: thread-safe (pure functions).
namespace fv::meeting_state {

// The export envelope's version; bumped on breaking shape changes.
inline constexpr int EXPORT_FORMAT_VERSION = 1;

// Row fields stripped from an export: the tokens grant dashboard access and must not leak.
inline constexpr std::array<std::string_view, 5> VOLATILE_MEETING_FIELDS = {
    "host_token", "guest_token", "app_pid", "app_heartbeat_at", "state_json"};

// {"format_version": 1, "meeting": <row less the volatile fields>, "state": ..., "segments":
// [...]}: a null or non-object meeting reads as {}.
Json export_json_payload(const Json& meeting, const Json& state, const std::vector<Json>& segments);
// The payload as json.dumps(indent=2, ensure_ascii=False) writes it.
std::string export_json(const Json& meeting, const Json& state, const std::vector<Json>& segments);

} // namespace fv::meeting_state
