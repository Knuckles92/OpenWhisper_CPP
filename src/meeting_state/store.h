#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "base/clock.h"
#include "base/json.h"
#include "base/subscription.h"
#include "meeting_state/ids.h"
#include "meeting_state/repository.h"
#include "meeting_state/schema.h"
#include "meeting_state/segment_ops.h"
#include "meeting_state/types.h"

// meeting/state/store.py: the single writer of one meeting's state document. Every mutation
// (agent output, dashboard actions, diarizer relabels, host undo) flows through apply, which
// is what keeps attribution, the audit trail, human-over-agent protection and multi-client
// broadcast consistent. Two ordering rules:
// - write-through persistence completes before the live document is replaced or anyone is
//   told, so a database failure rejects the batch without exposing state a restart would lose;
// - subscribers are called while the store's lock is held, so the fan-out order never inverts
//   the seq order. Subscribers must not block (they only enqueue).
// Affinity: thread-safe. Lock order (appcore/locks.h): inside the meeting runtime lock, outside
// the database writer.
namespace fv::meeting_state {

using Json = fv::Json;

// Bulk {segment_id: speaker_pinned} for the ids that exist.
using SegmentLookup = std::function<std::map<std::string, bool, std::less<>>(
    const std::vector<std::string>& segment_ids)>;

// The optional segment predicates of a store. segment_exists validates evidence ids;
// segment_pinned reports a human speaker pin, so an automated relabel cannot revert it;
// segment_lookup answers both for every id a batch cites in one call (the predicates still
// decide ids the batch did not cite, and stand in when the lookup fails).
struct SegmentPredicates {
    std::function<bool(std::string_view)> segment_exists;
    std::function<bool(std::string_view)> segment_pinned;
    SegmentLookup segment_lookup;
};

// repository_segment_lookup: segment_flags bound to one meeting; empty for a null repository.
SegmentLookup repository_segment_lookup(std::shared_ptr<MeetingRepository> repository,
                                        std::string meeting_id);

// Called when finalization enters "running": review_ops.h's invalidate_checks unless a test
// replaced it (an empty hook does nothing). The hook runs under the store's lock on the
// candidate document.
void set_invalidate_checks(std::function<void(MeetingState&)> hook);

class MeetingStateStore {
public:
    using Subscriber = std::function<void(std::int64_t max_seq, const std::vector<OpResult>&)>;

    // Without a repository nothing is persisted and undo has nothing to read. Without a
    // segment handler, segment ops are rejected "segments_unavailable". `clock` and `ids`
    // stamp and name new entities and must outlive the store.
    MeetingStateStore(MeetingState state, std::shared_ptr<MeetingRepository> repository = nullptr,
                      SegmentHandler segment_handler = {}, const Clock& clock = system_clock(),
                      IdSource& ids = random_ids(), SegmentPredicates predicates = {});

    MeetingStateStore(const MeetingStateStore&) = delete;
    MeetingStateStore& operator=(const MeetingStateStore&) = delete;

    std::string meeting_id() const;
    std::int64_t seq() const;
    // The full document as a fresh JSON object (to_dict()).
    Json snapshot() const;
    // Runs fn(const MeetingState&) against the live document under the lock.
    template <class F> auto with_state(F&& fn) const {
        std::lock_guard lock(mutex_);
        return std::forward<F>(fn)(static_cast<const MeetingState&>(state_));
    }

    // Validates and applies a batch (a JSON list of op objects), persists and audits the
    // applied ops, then tells the subscribers. One result per op: a rejected op carries its
    // reason and never consumes a seq; an applied one carries its seq and effect. A persistence
    // failure marks every applied op "persistence_error" ("stale_selection" for a stale
    // one-occurrence correction) and leaves the document and subscribers untouched.
    std::vector<OpResult> apply(std::string_view actor_type,
                                const std::optional<std::string>& actor_id, const Json& ops);

    // Lifecycle and status fields (status, finalization, capture, speech, intelligence_online,
    // ...; any MeetingState field by its to_json key), persisted before they become
    // observable. finalization is coerced (FinalizationState::coerce) against the new status;
    // entering "running" calls the invalidate-checks hook. False when persistence failed, with
    // the document unchanged. Throws py::PyError("AttributeError") for an unknown field.
    bool update_runtime_fields(const Json& fields);

    // Adopts the title an independent rename wrote to the meeting row, without rewriting the
    // snapshot; true when it changed.
    bool refresh_title();
    // Replaces the in-memory document after an out-of-band write (the final transcript's
    // evidence remap), which is already persisted.
    void replace_document(MeetingState state);
    // Host undo: applies the recorded inverse of event `event_seq` as the system actor. Empty
    // when there is no repository, the event is unknown, has no inverse or was already undone.
    std::vector<OpResult> undo(std::int64_t event_seq, const std::optional<std::string>& actor_id);

    // Ending the subscription removes the subscriber.
    Subscription subscribe(Subscriber subscriber);

private:
    struct Subscribers;

    std::pair<std::function<bool(std::string_view)>, std::function<bool(std::string_view)>>
    batch_predicates(const Json& ops) const;
    void sequence_results(MeetingState& candidate, std::vector<OpResult>& results);
    bool persist_batch(MeetingState& candidate, std::vector<OpResult>& applied,
                       std::string_view actor_type, const std::optional<std::string>& actor_id);
    void reconcile_persisted(MeetingState& candidate, const Json& persisted) const;
    void notify(std::int64_t seq, const std::vector<OpResult>& applied);

    mutable std::recursive_mutex mutex_;
    MeetingState state_;
    std::shared_ptr<MeetingRepository> repository_;
    SegmentHandler segment_handler_;
    const Clock& clock_;
    IdSource& ids_;
    SegmentPredicates predicates_;
    std::shared_ptr<Subscribers> subscribers_;
};

} // namespace fv::meeting_state
