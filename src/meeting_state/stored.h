#pragma once

#include <memory>
#include <optional>
#include <string>
#include <string_view>

#include "base/clock.h"
#include "base/json.h"
#include "meeting_state/ids.h"
#include "meeting_state/repository.h"
#include "meeting_state/schema.h"
#include "meeting_state/store.h"

// meeting/stored.py: the one way to load a persisted meeting row. Rows carry the state
// document as state_json text; a corrupt or missing snapshot is handled the same way everywhere.
// Not here: meeting_endpoint (the resolved text endpoint, with the meeting agent).
// Affinity: thread-safe (functions); open_store's store is the store's.
namespace fv::meeting_state {

// A row's saved state document, or {} when there is none (a null row included).
Json stored_state_dict(const Json& meeting);

// The row's raw agent_endpoint_json snapshot as an object, or nullopt.
std::optional<Json> stored_endpoint(const Json& meeting);

// The insights payload stored on a row, normalized for list UIs (normalize_historical against
// the snapshot's AI insights flag, else the row's, and the row's status, "ended" when blank).
FinalizationState finalization_from_meeting_row(const Json& meeting);

// {finalization_status, finalization_deferred[, insights_pill, insights_tone]} for meeting
// lists; never state_json or step details.
Json compact_finalization_list_fields(const Json& meeting);

// Rebuilds a meeting's saved document. `historical` serves a past meeting no live worker owns:
// the row's id, title, status and AI insights flag win over the snapshot's, and work a restart
// interrupted (final insights, an insight review, a tailored report) reads as stopped. A
// missing or unreadable snapshot yields a fresh state that keeps the row's title and AI
// insights flag, with the report views `settings` enables. The meeting id is the row's "id".
MeetingState load_state(const Json& meeting, bool historical, const Json& settings,
                        const Clock& clock = system_clock());
MeetingState load_state(const Json& meeting, std::string_view meeting_id, bool historical,
                        const Json& settings, const Clock& clock = system_clock());

// A write-through store over a persisted meeting (see load_state), with the repository's
// segment handler, predicates and bulk lookup.
std::shared_ptr<MeetingStateStore> open_store(std::shared_ptr<MeetingRepository> repository,
                                              std::string_view meeting_id, const Json& meeting,
                                              bool historical, const Json& settings,
                                              const Clock& clock = system_clock(),
                                              IdSource& ids = random_ids());
// The meeting id is the row's "id".
std::shared_ptr<MeetingStateStore> open_store(std::shared_ptr<MeetingRepository> repository,
                                              const Json& meeting, bool historical,
                                              const Json& settings,
                                              const Clock& clock = system_clock());

} // namespace fv::meeting_state
