#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "base/cancel.h"
#include "base/clock.h"
#include "base/json.h"
#include "llm/judge.h"
#include "meeting_intel/history_source.h"

// meeting/recall.py: consent-gated, bounded recall of past meeting transcripts for the meeting
// agents. Policy lives here so no caller can bypass consent or caps, and past-meeting segment ids
// never leave it: they share the sg_ prefix of live evidence ids and would be repaired onto the
// current transcript.
// Affinity: thread-safe; blocks on storage and the judge, never on the GUI thread.
namespace fv::meeting_intel {

inline constexpr std::int64_t kRecallDefaultHitLimit = 10;
inline constexpr std::int64_t kRecallMaxHitLimit = 20;
inline constexpr std::size_t kRecallMaxSnippetChars = 180;
inline constexpr std::size_t kRecallMaxTotalChars = 4000;
inline constexpr std::int64_t kRecallMaxSliceSegments = 40;

struct RecallDeps {
    // The stored meetings; nullptr when recall is not available.
    HistorySource* history = nullptr;
    // The settings mapping, read on every call; a failure leaves recall off.
    std::function<Json()> settings;
    // The judge that ranks the search semantically when that feature is on; nullptr: none.
    std::shared_ptr<llm::JudgeProvider> judges;
    const Clock* clock = nullptr; // system_clock() when null
    // Production can restrict ranking to its currently configured device-local endpoint.
    std::function<std::shared_ptr<llm::Judge>()> local_judge;
};

// {ok, disabled?, text, hits}. `text` is what the model sees; hits are {ref, meeting_id, title,
// started_at, start_s, snippet} with refs "past:<meeting id>:<n>" numbered per meeting. With
// `meeting_id`, a slice of at most 40 segments of that meeting; else a search for `query`
// (ranked semantically when that Fast judgments feature is on). The current meeting is always
// excluded. `limit` is int()'d (10 when that fails) and clamped to 1..20; an infinite float
// throws py::OverflowError, as Python's int() does.
Json search_past_meetings(RecallDeps& deps, std::string_view query,
                          std::string_view current_meeting_id, std::string_view meeting_id,
                          const Json& limit = Json(kRecallDefaultHitLimit),
                          const CancelToken& cancel = {});
// Whether the user opted in; false (logged by type) when the settings cannot be read.
bool past_recall_enabled(const std::function<Json()>& settings);

namespace detail {

// _format_hits: rows of search_history, at most `limit` hits within 4000 characters of text.
Json format_hits(const std::vector<Json>& rows, std::int64_t limit,
                 std::string_view current_meeting_id);
// _slice_meeting.
Json slice_meeting(HistorySource& history, std::string_view meeting_id,
                   std::string_view current_meeting_id, std::int64_t limit);
// _date_label: the first ten characters of an ISO date, the raw text otherwise, or
// "unknown date".
std::string date_label(const Json& started_at);
// _title: sanitized and stripped, or "Untitled meeting".
std::string title_of(const Json& value);
// _start_s: float(value), 0.0 when that fails.
double start_seconds(const Json& value);
// f"{value:g}".
std::string format_g(double value);

} // namespace detail

} // namespace fv::meeting_intel
