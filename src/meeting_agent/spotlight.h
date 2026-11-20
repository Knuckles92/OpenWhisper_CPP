#pragma once

#include <string>
#include <string_view>
#include <vector>

#include "base/json.h"

// meeting/agent/prompts.py's Top Insights selection, which matches the dashboard's spotlight
// ranking: pinned first, then human-touched (edited or confirmed), then the most recently
// updated; one item per card before a second from any card; near-duplicate texts skipped.
// Live notes and user notes are never featured, nor removed items, nor timeline beats or
// unreviewed state_repair samples unless a human pinned or touched them.
// Affinity: thread-safe (pure functions).
namespace fv::meeting_agent {

using Json = fv::Json;

// Copies of the featured items, in rank order, each with "card" set to its card key. `cards`
// is a snapshot's "cards" object; its key order breaks ties.
std::vector<Json> select_spotlight_items(const Json& cards, int limit = 3);

// _normalized_text: lower case, every run of characters outside [a-z0-9] as one space, stripped.
std::string normalized_text(std::string_view text);
// _texts_overlap of two normalized texts: equal, word-set Jaccard >= 0.60, or, when the smaller
// set has at least 4 words, at least 0.75 of it shared.
bool texts_overlap(std::string_view norm, std::string_view other_norm);

} // namespace fv::meeting_agent
