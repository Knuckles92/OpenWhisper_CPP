// What each pass reads: the durable delivery ledger in commit order, or (for hosts without one)
// a time window behind the newest segment already sent, and the bookkeeping of what was sent.

#include <algorithm>
#include <utility>

#include "meeting_agent/detail/py_value.h"
#include "meeting_agent/detail/scheduler_support.h"
#include "meeting_agent/scheduler.h"
#include "pyport/number.h"

namespace fv::meeting_agent {

namespace {

std::int64_t delivery_seq_of(const Json& row, std::int64_t fallback) {
    const Json& value = detail::get(row, "delivery_seq");
    if (value.is_number_integer()) {
        return value.get<std::int64_t>();
    }
    return py::py_int(value).value_or(fallback);
}

} // namespace

CheckpointScheduler::Fetch CheckpointScheduler::delivery_page(std::string_view consumer) {
    const std::int64_t cursor = consumer == "cards" ? card_cursor_seq_ : notes_cursor_seq_;
    auto fetched = host_.delivery_page(cursor, DELIVERY_PAGE_SIZE + 1);
    const bool more = fetched.size() > static_cast<std::size_t>(DELIVERY_PAGE_SIZE);
    if (more) {
        fetched.resize(static_cast<std::size_t>(DELIVERY_PAGE_SIZE));
    }
    const std::int64_t last_seq =
        fetched.empty() ? cursor : delivery_seq_of(fetched.back(), cursor);
    // The agent reads speech in timeline order while the acknowledgement stays in commit order,
    // so a late segment that starts early cannot disappear.
    std::stable_sort(fetched.begin(), fetched.end(), [](const Json& a, const Json& b) {
        const double a_start = detail::row_start_s(a);
        const double b_start = detail::row_start_s(b);
        if (a_start != b_start) {
            return a_start < b_start;
        }
        return detail::row_id(a) < detail::row_id(b);
    });
    return Fetch{std::move(fetched), more, last_seq};
}

CheckpointScheduler::Fetch CheckpointScheduler::fetch_for_checkpoint(bool guidance) {
    // Guidance re-reads the whole meeting; otherwise the ledger is paged, or the time window
    // for hosts without one.
    if (delivery_enabled_ && !guidance) {
        return delivery_page("cards");
    }
    return Fetch{host_.get_transcript(guidance ? -1.0 : fetch_cursor_s()), false, card_cursor_seq_};
}

std::vector<Json> CheckpointScheduler::checkpoint_segments(const std::vector<Json>& fetched,
                                                           bool guidance) const {
    if (delivery_enabled_ && !guidance) {
        return fetched;
    }
    if (guidance) {
        // Segments are read with the term corrections applied, so a guidance pass shows the
        // agent the corrected recent transcript.
        const std::size_t first =
            fetched.size() > GUIDANCE_MAX_SEGMENTS ? fetched.size() - GUIDANCE_MAX_SEGMENTS : 0;
        return std::vector<Json>(fetched.begin() + static_cast<std::ptrdiff_t>(first),
                                 fetched.end());
    }
    std::vector<Json> unsent;
    for (const auto& segment : fetched) {
        if (!sent_starts_.contains(detail::row_id(segment))) {
            unsent.push_back(segment);
        }
    }
    return unsent;
}

double CheckpointScheduler::fetch_cursor_s() const {
    // options_.refetch_window_s behind the newest sent segment, so late segments of the other
    // channel are still read; ids already sent are filtered out of the result.
    if (max_sent_start_s_ < 0.0) {
        return -1.0;
    }
    return std::max(-1.0, max_sent_start_s_ - options_.refetch_window_s);
}

void CheckpointScheduler::mark_sent(const std::vector<Json>& segments) {
    for (const auto& segment : segments) {
        const Json& id = detail::get(segment, "id");
        if (!detail::py_truthy(id)) {
            continue;
        }
        const double start_s = detail::row_start_s(segment);
        sent_starts_[detail::py_str(id)] = start_s;
        if (start_s > max_sent_start_s_) {
            max_sent_start_s_ = start_s;
        }
    }
    // Anything at or before the cursor is outside every future fetch.
    const double cursor = fetch_cursor_s();
    std::erase_if(sent_starts_, [cursor](const auto& entry) { return entry.second <= cursor; });
}

CheckpointScheduler::Fetch CheckpointScheduler::fetch_for_notes(bool guidance) {
    double cursor = -1.0;
    if (!guidance && notes_max_sent_start_s_ >= 0.0) {
        cursor = std::max(-1.0, notes_max_sent_start_s_ - options_.refetch_window_s);
    }
    if (delivery_enabled_ && !guidance) {
        return delivery_page("notes");
    }
    return Fetch{host_.get_transcript(cursor), false, notes_cursor_seq_};
}

std::vector<Json> CheckpointScheduler::notes_segments(const std::vector<Json>& fetched,
                                                      bool guidance) const {
    // Unsent segments, oldest first; a guidance review takes the newest instead.
    const std::size_t cap = options_.notes_max_segments;
    if (guidance) {
        const std::size_t first = fetched.size() > cap ? fetched.size() - cap : 0;
        return std::vector<Json>(fetched.begin() + static_cast<std::ptrdiff_t>(first),
                                 fetched.end());
    }
    std::vector<Json> segments;
    for (const auto& segment : fetched) {
        if (segments.size() >= cap) {
            break;
        }
        if (delivery_enabled_ || !notes_sent_starts_.contains(detail::row_id(segment))) {
            segments.push_back(segment);
        }
    }
    return segments;
}

void CheckpointScheduler::mark_notes_sent(const std::vector<Json>& segments) {
    double newest = -1.0;
    bool any = false;
    for (const auto& segment : segments) {
        const double start_s = detail::row_start_s(segment);
        const Json& id = detail::get(segment, "id");
        if (detail::py_truthy(id)) {
            notes_sent_starts_[detail::py_str(id)] = start_s;
        }
        newest = any ? std::max(newest, start_s) : start_s;
        any = true;
    }
    notes_max_sent_start_s_ = std::max(notes_max_sent_start_s_, newest);
    prune_notes_sent();
}

void CheckpointScheduler::prune_notes_sent() {
    // Forget sent notes segments that no future fetch can return.
    const double cursor = std::max(-1.0, notes_max_sent_start_s_ - options_.refetch_window_s);
    std::erase_if(notes_sent_starts_,
                  [cursor](const auto& entry) { return entry.second <= cursor; });
}

} // namespace fv::meeting_agent
