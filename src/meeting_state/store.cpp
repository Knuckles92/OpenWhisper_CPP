#include "meeting_state/store.h"

#include <algorithm>
#include <exception>
#include <set>
#include <utility>

#include "base/log.h"
#include "meeting_state/corrections.h"
#include "meeting_state/detail/py_value.h"
#include "meeting_state/patches.h"
#include "meeting_state/review_ops.h"
#include "pyport/errors.h"

namespace fv::meeting_state {

namespace {

constexpr log::Logger logger("meeting.state.store");

using detail::get;
using detail::truthy;

std::mutex& hook_mutex() {
    static std::mutex mutex;
    return mutex;
}

std::function<void(MeetingState&)>& invalidate_checks_hook() {
    // store.py calls review.invalidate_checks itself; the hook only lets a test replace it.
    static std::function<void(MeetingState&)> hook = invalidate_checks;
    return hook;
}

using IdSet = std::set<std::string, std::less<>>;

// _referenced_segment_ids: the segment ids a batch cites ("segment_id", and the "evidence" and
// "segment_ids" lists, two object levels deep), so one query can answer them all. Anything
// missed still goes through the per-id predicates.
void visit_cited(const Json& value, int depth, IdSet& ids) {
    if (depth > 2 || !value.is_object()) {
        return;
    }
    for (const auto& [key, item] : value.items()) {
        if (key == "segment_id" && item.is_string()) {
            ids.insert(item.get<std::string>());
        } else if ((key == "evidence" || key == "segment_ids") && item.is_array()) {
            for (const auto& id : item) {
                if (id.is_string()) {
                    ids.insert(id.get<std::string>());
                }
            }
        } else if (item.is_object()) {
            visit_cited(item, depth + 1, ids);
        }
    }
}

void fail_results(std::vector<OpResult>& results, const std::string& reason) {
    for (auto& result : results) {
        result.ok = false;
        result.reason = reason;
        result.seq.reset();
    }
}

std::string text_or_empty(const Json& value) {
    return truthy(value) ? detail::py_str(value) : std::string();
}

std::vector<std::string> string_list(const Json& value) {
    std::vector<std::string> out;
    if (value.is_array()) {
        for (const auto& item : value) {
            out.push_back(detail::py_str(item));
        }
    }
    return out;
}

} // namespace

struct MeetingStateStore::Subscribers {
    struct Entry {
        std::shared_ptr<CallbackGuard> guard;
        Subscriber fn;
    };
    std::mutex mutex;
    std::vector<std::shared_ptr<Entry>> entries;
};

SegmentLookup repository_segment_lookup(std::shared_ptr<MeetingRepository> repository,
                                        std::string meeting_id) {
    if (!repository) {
        return {};
    }
    return [repository = std::move(repository),
            meeting_id = std::move(meeting_id)](const std::vector<std::string>& segment_ids) {
        return repository->segment_flags(meeting_id, segment_ids);
    };
}

void set_invalidate_checks(std::function<void(MeetingState&)> hook) {
    std::lock_guard lock(hook_mutex());
    invalidate_checks_hook() = std::move(hook);
}

MeetingStateStore::MeetingStateStore(MeetingState state,
                                     std::shared_ptr<MeetingRepository> repository,
                                     SegmentHandler segment_handler, const Clock& clock,
                                     IdSource& ids, SegmentPredicates predicates)
    : state_(std::move(state)), repository_(std::move(repository)),
      segment_handler_(std::move(segment_handler)), clock_(clock), ids_(ids),
      predicates_(std::move(predicates)), subscribers_(std::make_shared<Subscribers>()) {}

std::string MeetingStateStore::meeting_id() const {
    std::lock_guard lock(mutex_);
    return state_.meeting_id;
}

std::int64_t MeetingStateStore::seq() const {
    std::lock_guard lock(mutex_);
    return state_.seq;
}

Json MeetingStateStore::snapshot() const {
    std::lock_guard lock(mutex_);
    return state_.to_json();
}

std::vector<OpResult> MeetingStateStore::apply(std::string_view actor_type,
                                               const std::optional<std::string>& actor_id,
                                               const Json& ops) {
    std::lock_guard lock(mutex_);
    // The live document stays untouched until persistence succeeds. Handlers work on their own
    // copy of each op, so nothing the caller keeps is shared with the document.
    MeetingState candidate = state_;
    auto [exists, pinned] = batch_predicates(ops);
    OpContext ctx;
    ctx.actor_type = std::string(actor_type);
    ctx.actor_id = actor_id;
    ctx.segment_exists = std::move(exists);
    ctx.segment_pinned = std::move(pinned);
    ctx.clock = &clock_;
    ctx.ids = &ids_;
    std::vector<OpResult> results = apply_ops(candidate, ctx, ops);
    sequence_results(candidate, results);

    std::vector<std::size_t> applied_index;
    std::vector<OpResult> applied;
    for (std::size_t index = 0; index < results.size(); ++index) {
        if (results[index].ok) {
            applied_index.push_back(index);
            applied.push_back(results[index]);
        }
    }
    if (applied.empty()) {
        return results;
    }
    const bool persisted = persist_batch(candidate, applied, actor_type, actor_id);
    // The repository may enrich effects (a revised segment's stored row) or fail them all.
    for (std::size_t index = 0; index < applied.size(); ++index) {
        results[applied_index[index]] = applied[index];
    }
    if (!persisted) {
        return results;
    }
    state_ = std::move(candidate);
    std::int64_t max_seq = 0;
    for (const auto& result : applied) {
        max_seq = std::max(max_seq, result.seq.value_or(0));
    }
    // Notified under the lock: two concurrent writers can never hand their batches to
    // subscribers out of seq order.
    notify(max_seq, applied);
    return results;
}

void MeetingStateStore::sequence_results(MeetingState& candidate, std::vector<OpResult>& results) {
    for (auto& result : results) {
        if (!result.ok) {
            continue;
        }
        const Json& name = get(result.op, "op");
        const auto flags =
            name.is_string() ? op_flags(name.get_ref<const std::string&>()) : std::nullopt;
        if (flags && flags->segment) {
            if (!segment_handler_) {
                result.ok = false;
                result.reason = "segments_unavailable";
                continue;
            }
            try {
                result.inverse = segment_handler_(result);
            } catch (const std::exception& error) {
                logger.error_with(error, "Segment op failed: " + detail::py_str(result.op));
                result.ok = false;
                result.reason = "segment_error";
                continue;
            }
        }
        candidate.seq += 1;
        result.seq = candidate.seq;
    }
}

bool MeetingStateStore::persist_batch(MeetingState& candidate, std::vector<OpResult>& applied,
                                      std::string_view actor_type,
                                      const std::optional<std::string>& actor_id) {
    if (!repository_) {
        return true;
    }
    try {
        const Json persisted = repository_->on_ops_applied(state_.meeting_id, candidate.to_json(),
                                                           applied, actor_type, actor_id);
        if (persisted.is_object()) {
            reconcile_persisted(candidate, persisted);
        }
    } catch (const StaleCorrectionSelection&) {
        fail_results(applied, "stale_selection");
        return false;
    } catch (const std::exception& error) {
        logger.error_with(error, "State persistence failed (meeting " + state_.meeting_id + ")");
        fail_results(applied, "persistence_error");
        return false;
    }
    return true;
}

void MeetingStateStore::reconcile_persisted(MeetingState& candidate, const Json& persisted) const {
    // A repository that reconciles only metadata hands back the title alone; any other may
    // transform the whole snapshot.
    if (repository_->state_metadata_only()) {
        candidate.title = text_or_empty(get(persisted, "title"));
        return;
    }
    candidate = MeetingState::from_json(persisted, clock_);
}

bool MeetingStateStore::update_runtime_fields(const Json& fields) {
    std::lock_guard lock(mutex_);
    MeetingState candidate = state_;
    if (fields.is_object()) {
        for (const auto& [key, value] : fields.items()) {
            if (key == "meeting_id") {
                candidate.meeting_id = text_or_empty(value);
            } else if (key == "seq") {
                candidate.seq = detail::py_int_or_throw(value);
            } else if (key == "status") {
                candidate.status = text_or_empty(value);
            } else if (key == "cloud_enabled") {
                candidate.cloud_enabled = truthy(value);
            } else if (key == "intelligence_online") {
                candidate.intelligence_online = truthy(value);
            } else if (key == "diarization_available") {
                candidate.diarization_available = truthy(value);
            } else if (key == "diarization_reason") {
                candidate.diarization_reason = text_or_empty(value);
            } else if (key == "speech") {
                candidate.speech = value;
            } else if (key == "title") {
                candidate.title = text_or_empty(value);
            } else if (key == "intent") {
                candidate.intent = MeetingIntent::from_json(value);
            } else if (key == "topic") {
                candidate.topic = TopicState::from_json(value);
            } else if (key == "rolling_summary") {
                candidate.rolling_summary = text_or_empty(value);
            } else if (key == "rolling_summary_evidence") {
                candidate.rolling_summary_evidence = string_list(value);
            } else if (key == "capture") {
                candidate.capture = value;
            } else if (key == "report_views") {
                candidate.report_views = string_list(value);
            } else if (key == "insight_review") {
                candidate.insight_review = value;
            } else if (key == "live_highlights") {
                candidate.live_highlights = value;
            } else if (key == "live_highlights_status") {
                candidate.live_highlights_status = text_or_empty(value);
            } else if (key == "voice_feedback") {
                candidate.voice_feedback = value;
            } else if (key == "finalization") {
                const Json& status = get(fields, "status");
                const std::string meeting_status =
                    status.is_string() ? status.get<std::string>() : candidate.status;
                FinalizationState finalization =
                    FinalizationState::coerce(value, candidate.cloud_enabled, meeting_status);
                if (finalization.status == "running" &&
                    candidate.finalization.status != "running") {
                    std::function<void(MeetingState&)> hook;
                    {
                        std::lock_guard hook_lock(hook_mutex());
                        hook = invalidate_checks_hook();
                    }
                    if (hook) {
                        hook(candidate);
                    }
                }
                candidate.finalization = std::move(finalization);
            } else if (key == "participants" || key == "cards" || key == "questions" ||
                       key == "custom_reports") {
                // Collections read as the document reads them.
                Json document = candidate.to_json();
                document[key] = value;
                const MeetingState rebuilt = MeetingState::from_json(document, clock_);
                if (key == "participants") {
                    candidate.participants = rebuilt.participants;
                } else if (key == "cards") {
                    candidate.cards = rebuilt.cards;
                } else if (key == "questions") {
                    candidate.questions = rebuilt.questions;
                } else {
                    candidate.custom_reports = rebuilt.custom_reports;
                }
            } else {
                throw py::PyError("AttributeError", key);
            }
        }
    }
    if (repository_) {
        try {
            const Json persisted =
                repository_->persist_state(state_.meeting_id, candidate.to_json());
            if (persisted.is_object()) {
                reconcile_persisted(candidate, persisted);
            }
        } catch (const std::exception& error) {
            logger.error_with(error, "Runtime state persistence failed (meeting " +
                                         state_.meeting_id + ")");
            return false;
        }
    }
    state_ = std::move(candidate);
    return true;
}

bool MeetingStateStore::refresh_title() {
    if (!repository_) {
        return false;
    }
    std::lock_guard lock(mutex_);
    const std::optional<Json> meeting = repository_->get_meeting(state_.meeting_id);
    if (!meeting) {
        return false;
    }
    std::string title = text_or_empty(get(*meeting, "title"));
    const bool changed = title != state_.title;
    state_.title = std::move(title);
    return changed;
}

void MeetingStateStore::replace_document(MeetingState state) {
    std::lock_guard lock(mutex_);
    state_ = std::move(state);
}

std::vector<OpResult> MeetingStateStore::undo(std::int64_t event_seq,
                                              const std::optional<std::string>& actor_id) {
    if (!repository_) {
        return {};
    }
    std::lock_guard lock(mutex_);
    const std::optional<Json> event = repository_->get_event(state_.meeting_id, event_seq);
    if (!event || !truthy(*event) || !truthy(get(*event, "inverse"))) {
        return {};
    }
    if (repository_->event_is_undone(state_.meeting_id, event_seq)) {
        return {};
    }
    // The marker is trusted only for system actors by the repository. It makes one undo request
    // idempotent while the inverse's own audit event stays undoable as an explicit redo. Inverse
    // ops carry force flags honored only for system actors.
    Json inverse = get(*event, "inverse");
    if (!inverse.is_object()) {
        throw py::TypeError("cannot convert dictionary update sequence element");
    }
    inverse["_undo_event_seq"] = event_seq;
    return apply("system", actor_id, Json::array({std::move(inverse)}));
}

Subscription MeetingStateStore::subscribe(Subscriber subscriber) {
    auto entry = std::make_shared<Subscribers::Entry>(
        Subscribers::Entry{std::make_shared<CallbackGuard>(), std::move(subscriber)});
    {
        std::lock_guard lock(subscribers_->mutex);
        subscribers_->entries.push_back(entry);
    }
    std::weak_ptr<Subscribers> weak = subscribers_;
    const Subscribers::Entry* key = entry.get();
    return Subscription(entry->guard, [weak, key] {
        if (auto subscribers = weak.lock()) {
            std::lock_guard lock(subscribers->mutex);
            std::erase_if(subscribers->entries,
                          [key](const auto& candidate) { return candidate.get() == key; });
        }
    });
}

void MeetingStateStore::notify(std::int64_t seq, const std::vector<OpResult>& applied) {
    std::vector<std::shared_ptr<Subscribers::Entry>> entries;
    {
        std::lock_guard lock(subscribers_->mutex);
        entries = subscribers_->entries;
    }
    for (const auto& entry : entries) {
        try {
            entry->guard->run([&] { entry->fn(seq, applied); });
        } catch (const std::exception& error) {
            logger.error_with(error, "State subscriber raised");
        }
    }
}

std::pair<std::function<bool(std::string_view)>, std::function<bool(std::string_view)>>
MeetingStateStore::batch_predicates(const Json& ops) const {
    // Validation sees the segment log as it was before the batch either way: segment ops only
    // run after every op has been validated.
    auto exists_fallback = predicates_.segment_exists;
    auto pinned_fallback = predicates_.segment_pinned;
    if (!predicates_.segment_lookup || (!exists_fallback && !pinned_fallback)) {
        return {exists_fallback, pinned_fallback};
    }
    auto cited = std::make_shared<IdSet>();
    if (ops.is_array()) {
        for (const auto& op : ops) {
            visit_cited(op, 0, *cited);
        }
    }
    if (cited->empty()) {
        return {exists_fallback, pinned_fallback};
    }
    std::map<std::string, bool, std::less<>> flags;
    try {
        flags = predicates_.segment_lookup(std::vector<std::string>(cited->begin(), cited->end()));
    } catch (const std::exception& error) {
        logger.error_with(error, "Bulk segment lookup failed; checking ids one by one");
        return {exists_fallback, pinned_fallback};
    }
    auto found = std::make_shared<std::map<std::string, bool, std::less<>>>(std::move(flags));
    std::function<bool(std::string_view)> exists;
    std::function<bool(std::string_view)> pinned;
    if (exists_fallback) {
        exists = [cited, found, exists_fallback](std::string_view segment_id) {
            if (cited->contains(segment_id)) {
                return found->contains(segment_id);
            }
            return exists_fallback(segment_id);
        };
    }
    if (pinned_fallback) {
        pinned = [cited, found, pinned_fallback](std::string_view segment_id) {
            if (cited->contains(segment_id)) {
                const auto flag = found->find(segment_id);
                return flag != found->end() && flag->second;
            }
            return pinned_fallback(segment_id);
        };
    }
    return {exists, pinned};
}

} // namespace fv::meeting_state
