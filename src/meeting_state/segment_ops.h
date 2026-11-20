#pragma once

#include <functional>
#include <memory>
#include <string>
#include <string_view>

#include "base/json.h"
#include "meeting_state/repository.h"
#include "meeting_state/types.h"

// meeting/state/segment_ops.py: the inverse of a segment-log op, for the store's segment
// handler. Persistence itself is the repository's write-through (on_ops_applied); this only
// reads the prior row. Shared by the live engine and the headless post-meeting passes.
// Affinity: as the repository (thread-safe).
namespace fv::meeting_state {

using Json = fv::Json;

// Applies a validated segment op (its effect describes the intent) and returns the inverse op,
// or null when there is none; throws on failure, which rejects the op ("segment_error").
using SegmentHandler = std::function<Json(OpResult& result)>;

// The inverse restoring the prior row, or null when the segment has no prior row (a failed
// read is logged and counts as none). revise_segment_text also replaces result.effect with
// {entity: "segment_text", segment_id, text, segment} (the post-apply row) and undoes to the
// stored raw text, never the corrected view; reassign_segment_speaker undoes to the prior
// speaker, source and pin with "force".
Json handle_segment_op(MeetingRepository& repository, std::string_view meeting_id,
                       OpResult& result);

// A store segment handler bound to one meeting; it keeps `repository` alive.
SegmentHandler make_segment_handler(std::shared_ptr<MeetingRepository> repository,
                                    std::string meeting_id);

} // namespace fv::meeting_state
