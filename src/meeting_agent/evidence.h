#pragma once

#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "base/json.h"

// meeting/agent/evidence.py: repairs evidence ids a model truncated or mistyped before the
// exact-match validation of the state-patch layer, so a grounded op is not lost to one wrong
// character. An id is replaced only when it does not exist and exactly one citable id is a
// near-certain match; ambiguous or unmatched ids pass through and keep their honest rejection.
// Affinity: thread-safe (pure functions; `exists` is called on the caller's thread).
namespace fv::meeting_agent {

using Json = fv::Json;

// A random pair of 23-character hex ids scores far below these; a model-rebuilt id with 5 or 6
// wrong characters still scores about 0.74 against its target. The margin is the guard against
// guessing between two legitimately similar ids.
inline constexpr double FUZZY_MIN_RATIO = 0.60;
inline constexpr double FUZZY_MARGIN = 0.10;

// The one citable id `bad_id` stands for: only "sg_" ids, by unique prefix, then unique
// containment, then difflib's best close match (cutoff 0.5) when its ratio is at least
// FUZZY_MIN_RATIO and FUZZY_MARGIN above the runner-up.
std::optional<std::string> repair_one(std::string_view bad_id,
                                      const std::vector<std::string>& citable);

// The ops (a JSON list, never changed) with unknown evidence ids repaired, and the number of
// ids repaired. An op without a non-empty evidence list is kept as it is; within a list an
// existing id stays, a non-text entry is dropped, and an unknown id is repaired or kept.
// `citable` is deduplicated in order; each distinct bad id is resolved once per call.
std::pair<Json, int> repair_evidence_ids(const Json& ops, const std::vector<std::string>& citable,
                                         const std::function<bool(std::string_view)>& exists);

} // namespace fv::meeting_agent
