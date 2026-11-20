#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "base/json.h"
#include "base/log.h"
#include "meeting_agent/interfaces.h"
#include "meeting_agent/scheduler.h"

// What the scheduler's source files share: Python's reads of a transcript row and of a pass's
// result, and the scheduler's logger.
namespace fv::meeting_agent::detail {

inline constexpr log::Logger scheduler_log("meeting.agent.scheduler");

// float(row.get("start_s") or 0.0): a number or a numeric string; anything else falsy is 0.
double row_start_s(const Json& row);
// str(row.get("id") or "").
std::string row_id(const Json& row);
// row.get("text") or "" as text (a non-string value as Python's str() prints it).
std::string row_text(const Json& row);

// How many ops of a pass applied.
std::size_t applied_count(const AgentResult& result);
// Whether a pass rewrote any transcript segment text.
bool revised_transcript(const AgentResult& result);

// The size of the first bounded cleanup window of `segments` (meeting/finalization.py's
// polish_blocks(segments)[0]): at most 80 rows and 8,000 code points of text, though the first
// row always counts whole. 0 for no rows.
std::size_t first_polish_block_size(const std::vector<Json>& segments);

// The terminal outcome of a consolidation result (nullopt: it timed out after `timeout_s`).
ConsolidationOutcome consolidation_outcome(const std::optional<AgentResult>& result,
                                           double timeout_s);

// Python's "%.0f" and "%.3f" of a number of seconds or a ratio, for log lines.
std::string fixed(double value, int digits);

// Names the calling thread for debuggers and crash reports.
void set_thread_name(std::string_view name);

} // namespace fv::meeting_agent::detail
