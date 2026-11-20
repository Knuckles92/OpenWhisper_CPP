#pragma once

#include <string>
#include <string_view>

#include "base/json.h"
#include "meeting_state/patches.h"

// The built-in op handlers, split by domain across detail/patches_*.cpp.
namespace fv::meeting_state::detail {

// An applied op's result.
OpResult accepted(const Json& op, std::optional<std::string> target_id, Json effect,
                  Json inverse = Json());

// list(op.get("evidence") or []) as ids; check_evidence has vetted it.
std::vector<std::string> evidence_ids(const Json& op);
Json evidence_json(const std::vector<std::string>& ids);

// detail/patches_items.cpp
OpResult op_add_item(MeetingState& state, Json& op, const OpContext& ctx);
OpResult op_update_item(MeetingState& state, Json& op, const OpContext& ctx);
OpResult op_remove_item(MeetingState& state, Json& op, const OpContext& ctx);
OpResult op_pin_item(MeetingState& state, Json& op, const OpContext& ctx, bool pinned);
OpResult op_confirm_item(MeetingState& state, Json& op, const OpContext& ctx);

// detail/patches_people.cpp
OpResult op_upsert_participant(MeetingState& state, Json& op, const OpContext& ctx);
OpResult op_suggest_participant_name(MeetingState& state, Json& op, const OpContext& ctx);
OpResult op_rename_participant(MeetingState& state, Json& op, const OpContext& ctx);
OpResult op_ask_question(MeetingState& state, Json& op, const OpContext& ctx);
OpResult op_resolve_question(MeetingState& state, Json& op, const OpContext& ctx);
OpResult op_answer_question(MeetingState& state, Json& op, const OpContext& ctx);
OpResult op_dismiss_question(MeetingState& state, Json& op, const OpContext& ctx);
OpResult op_reopen_question(MeetingState& state, Json& op, const OpContext& ctx);

// detail/patches_meta.cpp
OpResult op_set_topic(MeetingState& state, Json& op, const OpContext& ctx);
OpResult op_set_rolling_summary(MeetingState& state, Json& op, const OpContext& ctx);
OpResult op_set_title(MeetingState& state, Json& op, const OpContext& ctx);
OpResult op_set_meeting_intent(MeetingState& state, Json& op, const OpContext& ctx);
OpResult op_set_cloud_enabled(MeetingState& state, Json& op, const OpContext& ctx);
OpResult op_reassign_segment_speaker(MeetingState& state, Json& op, const OpContext& ctx);
OpResult op_revise_segment_text(MeetingState& state, Json& op, const OpContext& ctx);

// Normalized item text for duplicate detection: lower(), then every run outside [a-z0-9]
// becomes one space, stripped (accented letters are separators).
std::string normalize_item_text(std::string_view text);
// Equal normalized texts, token Jaccard at or above the first threshold, or containment at or
// above the second once both sides have four tokens.
bool item_text_too_similar(std::string_view left, std::string_view right,
                           double jaccard_threshold = 0.60, double containment_threshold = 0.75);

} // namespace fv::meeting_state::detail
