#pragma once

#include <cstddef>
#include <vector>

#include "base/json.h"
#include "meeting_state/store.h"

// meeting/state/repair.py: deterministic post-pass repairs of a meeting's dashboard. Structural
// gaps are filled from existing synthesized key points and note blocks only: raw transcript
// snippets, named entities and timeline labels are not evidence of importance, so only the
// agent or a human selects new key points, and empty cards are an honest result.
// The builders read a MeetingState::to_json() snapshot and segment rows (repository shape:
// id, start_s, text, ...) and return op lists; they never change anything. Not here: the
// evidence-id fuzzy repair of the agent's tool policy.
// Affinity: functions; repair_meeting_state goes through the store (thread-safe).
namespace fv::meeting_state {

// Cap on derived navigation beats, and on a beat's text (code points, "..." when clipped).
inline constexpr std::size_t MAX_TIMELINE_BEATS = 8;
inline constexpr std::size_t MAX_BEAT_TEXT = 180;

// add_item ops that populate an empty timeline: synthesized key points with timed evidence
// (start_s from the earliest evidence segment), else note blocks whose evidence is in the
// transcript; sorted by start_s, at most MAX_TIMELINE_BEATS. A populated timeline gets none.
// Throws py::ValueError or py::TypeError for a note block's start_s float() refuses.
Json build_timeline_backfill_ops(const Json& state, const std::vector<Json>& segments);
// Timeline beats for key points (three or more four-letter tokens) no live beat covers (token
// Jaccard below 0.45), until the timeline holds MAX_TIMELINE_BEATS.
Json build_timeline_coverage_from_segments(const Json& state, const std::vector<Json>& segments);
// One set_topic op when the topic is blank: the first synthesized key point, else the first
// note block's heading or text.
Json build_topic_backfill_ops(const Json& state, const std::vector<Json>& segments);
// One set_rolling_summary op when the summary is blank: the first six synthesized key points
// (else note blocks) joined as sentences, citing their evidence.
Json build_summary_backfill_ops(const Json& state, const std::vector<Json>& segments);

// Applies the repairs through the store as system/state_repair: the timeline (only when the
// report views include "ribbon"), then summary and topic, then, once the meeting is ended,
// needs_recovery or failed, the topic into a blank title. A failed snapshot or batch is logged
// and ends that part. Returns how many ops were applied.
int repair_meeting_state(MeetingStateStore& store, const std::vector<Json>& segments);

} // namespace fv::meeting_state
