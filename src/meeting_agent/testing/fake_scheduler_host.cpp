#include "meeting_agent/testing/fake_scheduler_host.h"

#include <algorithm>
#include <stdexcept>

#include "meeting_agent/detail/scheduler_support.h"
#include "meeting_state/schema.h"

namespace fv::meeting_agent::testing {

namespace {

std::shared_ptr<meeting_state::MeetingStateStore> empty_store() {
    meeting_state::MeetingState state;
    state.meeting_id = "m_test";
    return std::make_shared<meeting_state::MeetingStateStore>(std::move(state));
}

} // namespace

FakeSchedulerHost::FakeSchedulerHost(const std::vector<Json>& segments)
    : FakeSchedulerHost(segments, Options{}) {}

FakeSchedulerHost::FakeSchedulerHost(const std::vector<Json>& segments, Options options)
    : options_(options), store_(empty_store()) {
    add_segments(segments);
}

std::shared_ptr<meeting_state::MeetingStateStore> FakeSchedulerHost::store() {
    std::lock_guard lock(mutex_);
    return store_;
}

std::vector<Json> FakeSchedulerHost::get_transcript(double after_start_s) {
    std::lock_guard lock(mutex_);
    transcript_reads_.push_back(after_start_s);
    if (fail_transcript_) {
        throw std::runtime_error(*fail_transcript_);
    }
    // The repository's rule: start_s strictly after the cursor.
    std::vector<Json> rows;
    for (const auto& segment : segments_) {
        if (detail::row_start_s(segment) > after_start_s) {
            Json row = segment;
            row.erase("delivery_seq");
            rows.push_back(std::move(row));
        }
    }
    return rows;
}

double FakeSchedulerHost::meeting_clock_s() const {
    std::lock_guard lock(mutex_);
    return options_.clock_s;
}

std::vector<Json> FakeSchedulerHost::delivery_page(std::int64_t after_seq, int limit) {
    std::lock_guard lock(mutex_);
    std::vector<Json> page;
    for (const auto& segment : segments_) {
        if (static_cast<int>(page.size()) >= limit) {
            break;
        }
        if (segment.value("delivery_seq", std::int64_t{0}) > after_seq) {
            page.push_back(segment);
        }
    }
    return page;
}

std::int64_t FakeSchedulerHost::delivery_cursor(std::string_view consumer) {
    std::lock_guard lock(mutex_);
    const auto it = cursors_.find(consumer);
    return it == cursors_.end() ? 0 : it->second;
}

void FakeSchedulerHost::advance_delivery_cursor(std::string_view consumer, std::int64_t seq) {
    std::lock_guard lock(mutex_);
    if (fail_advance_) {
        throw std::runtime_error(*fail_advance_);
    }
    cursor_acks_.emplace_back(std::string(consumer), seq);
    auto& cursor = cursors_[std::string(consumer)];
    cursor = std::max(cursor, seq);
}

std::int64_t FakeSchedulerHost::delivery_high_water() {
    std::lock_guard lock(mutex_);
    if (fail_high_water_) {
        throw std::runtime_error(*fail_high_water_);
    }
    return next_seq_ - 1;
}

void FakeSchedulerHost::revoke_agent_writes() {
    std::lock_guard lock(mutex_);
    ++revoke_calls_;
}

bool FakeSchedulerHost::has_meeting_clock() const {
    return options_.meeting_clock;
}

bool FakeSchedulerHost::has_delivery_ledger() const {
    return options_.ledger;
}

void FakeSchedulerHost::add_segments(const std::vector<Json>& segments) {
    std::lock_guard lock(mutex_);
    for (const auto& segment : segments) {
        Json row = segment;
        if (options_.ledger) {
            row["delivery_seq"] = next_seq_++;
        }
        segments_.push_back(std::move(row));
    }
}

void FakeSchedulerHost::set_segments(std::vector<Json> segments) {
    std::lock_guard lock(mutex_);
    segments_ = std::move(segments);
}

void FakeSchedulerHost::edit_segments(const std::function<void(std::vector<Json>&)>& edit) {
    std::lock_guard lock(mutex_);
    edit(segments_);
}

std::vector<Json> FakeSchedulerHost::segments() const {
    std::lock_guard lock(mutex_);
    return segments_;
}

void FakeSchedulerHost::set_store(std::shared_ptr<meeting_state::MeetingStateStore> store) {
    std::lock_guard lock(mutex_);
    store_ = std::move(store);
}

void FakeSchedulerHost::set_snapshot(const Json& snapshot) {
    auto store = std::make_shared<meeting_state::MeetingStateStore>(
        meeting_state::MeetingState::from_json(snapshot));
    set_store(std::move(store));
}

void FakeSchedulerHost::set_clock_s(double seconds) {
    std::lock_guard lock(mutex_);
    options_.clock_s = seconds;
}

void FakeSchedulerHost::set_cursor(std::string_view consumer, std::int64_t seq) {
    std::lock_guard lock(mutex_);
    cursors_[std::string(consumer)] = seq;
}

void FakeSchedulerHost::fail_transcript(std::optional<std::string> message) {
    std::lock_guard lock(mutex_);
    fail_transcript_ = std::move(message);
}

void FakeSchedulerHost::fail_high_water(std::optional<std::string> message) {
    std::lock_guard lock(mutex_);
    fail_high_water_ = std::move(message);
}

void FakeSchedulerHost::fail_advance(std::optional<std::string> message) {
    std::lock_guard lock(mutex_);
    fail_advance_ = std::move(message);
}

std::vector<double> FakeSchedulerHost::transcript_reads() const {
    std::lock_guard lock(mutex_);
    return transcript_reads_;
}

std::vector<std::pair<std::string, std::int64_t>> FakeSchedulerHost::cursor_acks() const {
    std::lock_guard lock(mutex_);
    return cursor_acks_;
}

int FakeSchedulerHost::revoke_calls() const {
    std::lock_guard lock(mutex_);
    return revoke_calls_;
}

} // namespace fv::meeting_agent::testing
