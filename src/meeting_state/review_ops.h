#pragma once

#include <cstddef>
#include <string_view>

#include "base/json.h"
#include "meeting_state/patches.h"
#include "meeting_state/schema.h"
#include "meeting_state/types.h"

// meeting/state/review.py: the atomic insight-review actions. Only the review worker (a
// "system" actor) may publish model judgments; only the host answers, skips or reopens a
// question. The questions live in MeetingState::insight_review["questions"] as objects:
// {id, item_id, revision, field, text, choices: {answer: label}, status: open | skipped |
// answered, evidence, superseded?, correction?, note_id?, history?, ...}.
// item_effect (patches.h) is review.py's too: a later edit of a reviewed item supersedes its
// questions. Handlers follow apply_ops's contract (patches.h); a malformed question or op that
// Python would index into raises, which apply_ops turns into "internal_error".
// Affinity: functions over a document the caller owns.
namespace fv::meeting_state {

// The consent a meeting must record before a review may run (services.settings saves the same
// value).
inline constexpr std::string_view REVIEW_CONSENT = "typesafe-text-v1";

// Text bounds of an answer (code points) and of its owner and deadline fields; the user notes
// card bound an answer's note respects; the live_notes block a clarification may not outgrow.
inline constexpr std::size_t MAX_REVIEW_TEXT = 3500;
inline constexpr std::size_t MAX_REVIEW_FIELD_CHARS = 120;
inline constexpr std::size_t MAX_REVIEW_USER_NOTES = 500;
inline constexpr std::size_t MAX_NOTE_BLOCK_CHARS = 4000;

// review_begin {run_id}: system only, with AI insights, review enabled and REVIEW_CONSENT;
// the meeting ended and finalization settled ("meeting_not_ready"); not already running.
OpResult review_begin(MeetingState& state, Json& op, const OpContext& ctx);
// review_finish {run_id, status, message?, assessments?, questions?}: the worker's verdicts.
// A stale run is "stale_review"; consent revoked meanwhile stops the review as unavailable.
OpResult review_finish(MeetingState& state, Json& op, const OpContext& ctx);
// review_answer {question_id, answer, text?, owner_participant_id?, owner_name?, deadline?}.
OpResult review_answer(MeetingState& state, Json& op, const OpContext& ctx);
// review_skip / review_reopen {question_id}.
OpResult review_skip(MeetingState& state, Json& op, const OpContext& ctx);
OpResult review_reopen(MeetingState& state, Json& op, const OpContext& ctx);
// review_unavailable: system only.
OpResult review_unavailable(MeetingState& state, Json& op, const OpContext& ctx);

// A new finalization run: every citation check is cleared and, with the review enabled, the
// review waits for the new insights, unanswered questions are superseded and every assessment
// a human did not give is dropped. The store calls it when finalization enters "running".
void invalidate_checks(MeetingState& state);

} // namespace fv::meeting_state
