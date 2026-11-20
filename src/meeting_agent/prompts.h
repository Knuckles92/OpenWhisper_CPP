#pragma once

#include <string>
#include <vector>

#include "base/json.h"
#include "meeting_agent/guidance.h"
#include "meeting_agent/spotlight.h"

// meeting/agent/prompts.py: the copilot's and the note taker's standing charters and the user
// prompt of every pass (cards checkpoint, notes, polish, consolidation), rendered from a state
// snapshot and transcript segment rows. The wording is benchmarked behaviour and is kept byte
// for byte; the policy numbers come from meeting_state/patches.h, so the prompt can never
// disagree with validation. The product's own name is identity().product.
// Affinity: thread-safe (pure functions).
namespace fv::meeting_agent {

using Json = fv::Json;

// The copilot's system prompt (A26-01).
std::string build_system_prompt();
// The note taker's system prompt (A26-04).
std::string build_note_taker_system_prompt();

// The user prompt of a cards checkpoint, a polish pass or the consolidation pass: the brief, the
// human guidance, the dashboard, recent context (cards passes only), the segments, then the
// pass's instructions (A26-02). `state` is a MeetingState::to_json() snapshot, optionally with
// recent_transcript_context, report_views and the other keys the scheduler adds.
std::string build_checkpoint_user_prompt(const Json& state, const std::vector<Json>& new_segments,
                                         bool is_consolidation = false, bool is_polish = false);
// The final-pass instructions: the two "ribbon" steps are left out when the enabled views
// (an iterable, Python's report_views) exclude it; null or empty keeps every step. Steps are
// renumbered contiguously (A26-03).
std::string build_consolidation_instructions(const Json& report_views);
// The note taker's user prompt (A26-04): note_adjustment_request or notes_review_requested in
// the state replace the default instructions.
std::string build_notes_user_prompt(const Json& state, const std::vector<Json>& new_segments);

// The host's brief with its guardrails, or "" when no brief was written (A26-05).
std::string intent_prompt(const Json& state);
// The dashboard as the agent reads it: ids, revisions, statuses and protection flags (A26-06).
std::string render_state_compact(const Json& state);
// The notes page as the note taker reads it.
std::string render_notes_page(const Json& state);
// "[sg_id] [t=123.4s] [Speaker]: text" (A26-08). `participants` is the snapshot's
// participants object.
std::string format_segment_line(const Json& segment, const Json& participants);

} // namespace fv::meeting_agent
