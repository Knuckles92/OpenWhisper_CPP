#pragma once

#include <cstddef>
#include <string_view>

#include "base/json.h"
#include "meeting_state/highlights.h"
#include "meeting_state/patches.h"
#include "meeting_state/schema.h"
#include "meeting_state/types.h"

// meeting/fast_state.py: system-owned advisory state ops for fast meeting judgments. Consent is
// not checked here: whether a judgment needed the meeting's AI insights depends on where its
// text went, which only the worker that asked knows; workers recheck it under the state lock.
// Each op belongs to one system actor id.
// Affinity: functions over a document the caller owns.
namespace fv::meeting_state {

inline constexpr std::string_view LIVE_SIGNALS_ACTOR = "live-signals";
inline constexpr std::string_view CITATION_VERIFIER_ACTOR = "citation-verifier";
inline constexpr std::string_view VOICE_COMMAND_ACTOR = "voice_command";

// Pulses kept: four hours at one pulse per kind per minute.
inline constexpr std::size_t MAX_LIVE_HIGHLIGHTS = 240 * PULSE_LABELS.size();
// Code points of a voice feedback message.
inline constexpr std::size_t MAX_VOICE_FEEDBACK_CHARS = 500;

// {pulses: [{id, kind, start_s, segment_id, text, ...}], minute?, final?}: live-signals only,
// while the meeting runs (ending or ended with final true). A minute replaces that minute's
// pulses; the rest merge by id, sorted by start_s.
OpResult publish_highlights(MeetingState& state, Json& op, const OpContext& ctx);
// {id, revision, check}: the verifier's annotation of one live item at its current revision;
// no revision increment (an annotation, not an edit of the claim).
OpResult citation_check(MeetingState& state, Json& op, const OpContext& ctx);
// {segment_ids?}: clears the annotations of live items citing any of them (all without ids).
OpResult invalidate_citations(MeetingState& state, Json& op, const OpContext& ctx);
// {message}: the voice command's spoken answer, while the meeting runs.
OpResult voice_feedback(MeetingState& state, Json& op, const OpContext& ctx);

} // namespace fv::meeting_state
