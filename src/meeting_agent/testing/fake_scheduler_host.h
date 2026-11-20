#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "meeting_agent/scheduler.h"

namespace fv::meeting_agent::testing {

// A SchedulerHost for scheduler tests (tests/fakes/scheduler.py FakeSchedulerEngine): an
// in-memory transcript served in the order it was added, a settable meeting clock (200 s in by
// default, past the warm-up), a state store, and optionally an in-memory delivery ledger that
// gives each added row the next delivery_seq. It records the transcript reads, the cursor
// acknowledgements and the write revocations, and can be told to fail.
// Affinity: thread-safe.
class FakeSchedulerHost final : public SchedulerHost {
public:
    struct Options {
        // Serve the delivery_* members (Python's engines with the ledger methods).
        bool ledger = false;
        // Answer meeting_clock_s (Python's engines with a `clock`).
        bool meeting_clock = true;
        double clock_s = 200.0;
    };

    // `segments` are added as add_segments adds them; the store holds an empty meeting "m_test".
    explicit FakeSchedulerHost(const std::vector<Json>& segments = {});
    FakeSchedulerHost(const std::vector<Json>& segments, Options options);

    // SchedulerHost
    std::shared_ptr<meeting_state::MeetingStateStore> store() override;
    std::vector<Json> get_transcript(double after_start_s) override;
    double meeting_clock_s() const override;
    std::vector<Json> delivery_page(std::int64_t after_seq, int limit) override;
    std::int64_t delivery_cursor(std::string_view consumer) override;
    void advance_delivery_cursor(std::string_view consumer, std::int64_t seq) override;
    std::int64_t delivery_high_water() override;
    void revoke_agent_writes() override;
    bool has_meeting_clock() const override;
    bool has_delivery_ledger() const override;

    // Appends rows in commit order; with a ledger each gets the next delivery_seq.
    void add_segments(const std::vector<Json>& segments);
    // Replaces the transcript (the ledger keeps its sequences).
    void set_segments(std::vector<Json> segments);
    // Edits the stored rows in place.
    void edit_segments(const std::function<void(std::vector<Json>&)>& edit);
    std::vector<Json> segments() const;

    // Null makes store() answer "no store".
    void set_store(std::shared_ptr<meeting_state::MeetingStateStore> store);
    // A store over MeetingState::from_json(snapshot).
    void set_snapshot(const Json& snapshot);
    void set_clock_s(double seconds);
    // A consumer's acknowledged sequence, as a previous scheduler left it.
    void set_cursor(std::string_view consumer, std::int64_t seq);

    // While set, the matching calls throw std::runtime_error(message).
    void fail_transcript(std::optional<std::string> message);
    void fail_high_water(std::optional<std::string> message);
    void fail_advance(std::optional<std::string> message);

    // The after_start_s of every get_transcript call, in order.
    std::vector<double> transcript_reads() const;
    // Every advance_delivery_cursor call: (consumer, seq).
    std::vector<std::pair<std::string, std::int64_t>> cursor_acks() const;
    int revoke_calls() const;

private:
    mutable std::mutex mutex_;
    Options options_;
    std::vector<Json> segments_;
    std::int64_t next_seq_ = 1;
    std::shared_ptr<meeting_state::MeetingStateStore> store_;
    std::map<std::string, std::int64_t, std::less<>> cursors_;
    std::optional<std::string> fail_transcript_;
    std::optional<std::string> fail_high_water_;
    std::optional<std::string> fail_advance_;
    std::vector<double> transcript_reads_;
    std::vector<std::pair<std::string, std::int64_t>> cursor_acks_;
    int revoke_calls_ = 0;
};

} // namespace fv::meeting_agent::testing
