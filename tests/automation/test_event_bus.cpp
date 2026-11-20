#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <set>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "automation/catalog.h"
#include "automation/event_bus.h"
#include "automation/testing/recording_bus_subscriber.h"
#include "base/log.h"
#include "base/testing/fake_clock.h"
#include "dictation/job.h"
#include "pyport/errors.h"
#include "pyport/json.h"

// from tests/test_automation_foundation_events.py, test_automation_fix_events_gap_reason.py,
// test_automation_r3_gap_per_type.py and test_automation_fix_pipeline_events.py

using namespace fv::automation;
using fv::Json;
using namespace std::chrono_literals;

namespace {

using Types = std::optional<std::vector<std::string>>;

std::vector<std::int64_t> seqs(const std::vector<Event>& events) {
    std::vector<std::int64_t> numbers;
    for (const Event& event : events) {
        numbers.push_back(event.seq);
    }
    return numbers;
}

// A one-shot gate, as Python's threading.Event.
class Gate {
public:
    void set() {
        std::lock_guard lock(mutex_);
        open_ = true;
        changed_.notify_all();
    }
    bool wait(std::chrono::milliseconds timeout) {
        std::unique_lock lock(mutex_);
        return changed_.wait_for(lock, timeout, [&] { return open_; });
    }

private:
    std::mutex mutex_;
    std::condition_variable changed_;
    bool open_ = false;
};

// Swaps the process-wide bus for the test's and puts the previous one back.
class ScopedBus {
public:
    explicit ScopedBus(std::shared_ptr<EventBus> bus) : previous_(set_bus(std::move(bus))) {}
    ~ScopedBus() {
        if (auto mine = set_bus(previous_)) {
            mine->close();
        }
    }

private:
    std::shared_ptr<EventBus> previous_;
};

class CapturedLog final : public fv::log::Sink {
public:
    void write(const fv::log::Record& record) override {
        std::lock_guard lock(mutex_);
        if (record.logger == "services.automation.events") {
            messages_.emplace_back(record.message);
        }
    }
    std::vector<std::string> messages() const {
        std::lock_guard lock(mutex_);
        return messages_;
    }

private:
    mutable std::mutex mutex_;
    std::vector<std::string> messages_;
};

class ScopedLog {
public:
    ScopedLog() : sink_(std::make_shared<CapturedLog>()) { fv::log::add_sink(sink_); }
    ~ScopedLog() { fv::log::remove_sink(sink_); }
    std::vector<std::string> messages() const { return sink_->messages(); }

private:
    std::shared_ptr<CapturedLog> sink_;
};

// restart(): the last position an earlier run gave out, and the bus of the run after it.
std::pair<std::int64_t, std::unique_ptr<EventBus>> restart(std::size_t buffer_size = 500) {
    EventBus earlier(500, 1'000);
    std::int64_t position = 0;
    for (int i = 0; i < 3; ++i) {
        position = earlier.publish_event(catalog::RECORDING_STARTED, Json::object()).seq;
    }
    earlier.close();
    return {position, std::make_unique<EventBus>(buffer_size, 2'000)};
}

std::vector<Event> publish_n(EventBus& bus, int count) {
    std::vector<Event> events;
    for (int n = 0; n < count; ++n) {
        events.push_back(
            bus.publish_event(catalog::RECORDING_STARTED, Json{{"job_id", std::to_string(n)}}));
    }
    return events;
}

Event channel(EventBus& bus, std::string_view text) {
    return bus.publish_event(catalog::CHANNEL_MESSAGE, Json{{"job_id", "j"},
                                                            {"source", "hotkey"},
                                                            {"origin", "dictated"},
                                                            {"action_id", "c"},
                                                            {"text", std::string(text)},
                                                            {"app", nullptr}});
}

std::vector<Event> segments(EventBus& bus, int count) {
    std::vector<Event> events;
    for (int n = 0; n < count; ++n) {
        events.push_back(bus.publish_event(catalog::MEETING_SEGMENTS,
                                           Json{{"meeting_id", "m"}, {"items", Json::array()}}));
    }
    return events;
}

const Types kChannel = std::vector<std::string>{std::string(catalog::CHANNEL_MESSAGE)};

SinceResult since_result(std::vector<Event> events, std::optional<std::string_view> gap) {
    SinceResult result;
    result.events = std::move(events);
    if (gap) {
        result.gap = std::string(*gap);
    }
    return result;
}

} // namespace

// ---- test_automation_foundation_events.py ----

TEST_CASE("test_subscribers_get_events_in_order_on_another_thread",
          "[test_automation_foundation_events]") {
    EventBus bus;
    RecordingBusSubscriber seen(bus);
    for (int n = 0; n < 5; ++n) {
        bus.publish_event("recording.started", Json{{"n", n}});
    }
    REQUIRE(seen.wait_for(5));
    CHECK(seqs(seen.events()) == std::vector<std::int64_t>{1, 2, 3, 4, 5});
    CHECK(seen.types() == std::vector<std::string>(5, "recording.started"));
    for (const auto id : seen.threads()) {
        CHECK(id != std::this_thread::get_id());
    }
    bus.close();
}

TEST_CASE("test_type_filter_and_labels", "[test_automation_foundation_events]") {
    EventBus bus;
    RecordingBusSubscriber wanted(bus, std::vector<std::string>{"history.saved"}, "claude-channel");
    auto other = bus.subscribe([](const Event&) {}, std::nullopt, "other");
    bus.publish_event("recording.started", Json::object());
    bus.publish_event("history.saved", Json{{"record_id", "1"}});
    REQUIRE(wanted.wait_for(1));
    CHECK(wanted.events().front().type == "history.saved");
    CHECK(bus.subscribers() == 2);
    CHECK(bus.subscribers("claude-channel") == 1);
    wanted.subscription().close();
    CHECK(wanted.subscription().closed());
    CHECK(bus.subscribers("claude-channel") == 0);
    bus.close();
}

TEST_CASE("test_a_failing_subscriber_does_not_stop_the_others",
          "[test_automation_foundation_events]") {
    EventBus bus;
    auto bad = bus.subscribe([](const Event&) { throw std::runtime_error("boom"); });
    RecordingBusSubscriber good(bus);
    bus.publish_event("recording.started", Json::object());
    bus.publish_event("recording.stopped", Json::object());
    CHECK(good.wait_for(2));
    bus.close();
}

// Python's case is an unserializable object() in the data; every C++ Json serializes, so the
// data that cannot be an event here is data that is not a mapping.
TEST_CASE("test_publish_rejects_data_that_is_not_json", "[test_automation_foundation_events]") {
    EventBus bus;
    CHECK_THROWS_AS(bus.publish_event("recording.started", Json::array({1, 2})), fv::py::TypeError);
    CHECK(bus.last_seq() == 0);
}

// dict(data or {}): data that Python reads as false is no data at all.
TEST_CASE("falsy data publishes as an empty object", "[event_bus]") {
    EventBus bus;
    for (const Json& data : {Json(), Json::array(), Json(false), Json(0), Json("")}) {
        CAPTURE(data.dump());
        CHECK(bus.publish_event("recording.started", data).data == Json::object());
    }
    CHECK(bus.last_seq() == 5);
}

TEST_CASE("test_module_publish_never_raises", "[test_automation_foundation_events]") {
    ScopedBus scoped(std::make_shared<EventBus>());
    CHECK_FALSE(publish("recording.started", Json::array({1})).has_value());
    const auto event = publish("recording.started", Json{{"ok", true}});
    REQUIRE(event.has_value());
    CHECK(event->seq == 1);
}

TEST_CASE("test_wire_shape", "[test_automation_foundation_events]") {
    EventBus bus;
    const Event event = bus.publish_event("history.saved", Json{{"record_id", "7"}});
    const Json data = fv::py::loads(event.to_json());
    std::vector<std::string> keys;
    for (const auto& [key, value] : data.items()) {
        keys.push_back(key);
    }
    CHECK(keys == std::vector<std::string>{"id", "seq", "type", "time", "v", "data"});
    CHECK(data["v"] == SCHEMA_VERSION);
    CHECK(data["id"].get<std::string>().starts_with("evt_"));
    CHECK(data["id"].get<std::string>().size() == 28);
    CHECK(data["time"].get<std::string>().ends_with("Z"));
    CHECK(data["data"] == Json{{"record_id", "7"}});
}

TEST_CASE("test_since_replays_and_reports_gaps", "[test_automation_foundation_events]") {
    EventBus bus(3);
    for (int n = 0; n < 5; ++n) {
        bus.publish_event("recording.started", Json{{"n", n}});
    }
    // The buffer holds 3, 4 and 5; 1 and 2 were evicted.
    auto result = bus.since(3);
    CHECK(seqs(result.events) == std::vector<std::int64_t>{4, 5});
    CHECK_FALSE(result.gap);
    result = bus.since(2);
    CHECK(seqs(result.events) == std::vector<std::int64_t>{3, 4, 5});
    CHECK_FALSE(result.gap);
    result = bus.since(1);
    CHECK(seqs(result.events) == std::vector<std::int64_t>{3, 4, 5});
    CHECK(result.gap);
    result = bus.since(5);
    CHECK(result.events.empty());
    CHECK_FALSE(result.gap);
}

TEST_CASE("test_since_from_an_earlier_app_session_replays_everything",
          "[test_automation_foundation_events]") {
    EventBus bus;
    bus.publish_event("recording.started", Json::object());
    const auto result = bus.since(900);
    CHECK(seqs(result.events) == std::vector<std::int64_t>{1});
    CHECK(result.gap);
}

TEST_CASE("test_clear_drops_the_buffer_and_marks_a_gap", "[test_automation_foundation_events]") {
    EventBus bus;
    bus.publish_event("recording.started", Json::object());
    bus.publish_event("recording.stopped", Json::object());
    bus.clear();
    CHECK(bus.recent().empty());
    auto result = bus.since(1);
    CHECK(result.events.empty());
    CHECK(result.gap);
    result = bus.since(2);
    CHECK(result.events.empty());
    CHECK_FALSE(result.gap);
}

TEST_CASE("test_live_only_events_are_delivered_but_never_replayed",
          "[test_automation_foundation_events]") {
    EventBus bus;
    RecordingBusSubscriber seen(bus);
    bus.publish_event("recording.started", Json::object());
    bus.publish_event("transcription.preview", Json{{"text", "hel"}}, false);
    bus.publish_event("recording.stopped", Json::object());
    REQUIRE(seen.wait_for(3));
    std::vector<std::string> recent_types;
    for (const Event& event : bus.recent()) {
        recent_types.push_back(event.type);
    }
    CHECK(recent_types == std::vector<std::string>{"recording.started", "recording.stopped"});
    // The skipped number is not a missed event.
    const auto result = bus.since(0);
    CHECK_FALSE(result.gap);
    CHECK(result.events.size() == 2);
    bus.close();
}

TEST_CASE("test_recent_filters_and_limits", "[test_automation_foundation_events]") {
    EventBus bus;
    for (const char* type : {"a", "b", "a", "a"}) {
        bus.publish_event(type, Json::object());
    }
    CHECK(seqs(bus.recent(2, std::vector<std::string>{"a"})) == std::vector<std::int64_t>{3, 4});
    CHECK(bus.recent(0).empty());
    CHECK(bus.recent(-1).empty());
    // An empty list follows no type, unlike no list at all.
    CHECK(bus.recent(10, std::vector<std::string>{}).empty());
    CHECK(bus.recent(10).size() == 4);
}

TEST_CASE("test_close_stops_delivery", "[test_automation_foundation_events]") {
    EventBus bus;
    RecordingBusSubscriber seen(bus);
    bus.close();
    bus.publish_event("recording.started", Json::object());
    std::this_thread::sleep_for(50ms);
    CHECK(seen.events().empty());
    CHECK(bus.subscribe([](const Event&) {})->closed());
}

TEST_CASE("test_catalog_lists_every_public_type_once", "[test_automation_foundation_events]") {
    const auto& types = catalog::PUBLIC_TYPES();
    CHECK(std::ranges::find(types, catalog::EVENTS_GAP) == types.end());
    const std::set<std::string_view> unique(types.begin(), types.end());
    CHECK(unique.size() == types.size());
    CHECK(catalog::known_types({"history.saved", "nope", "history.saved", "events.gap"}) ==
          std::vector<std::string>{"history.saved"});
}

// test_completed_payload_for_a_file_and_for_a_job is in tests/dictation/test_event_payloads.cpp,
// beside the builder it checks.

// ---- test_automation_fix_events_gap_reason.py ----

TEST_CASE("test_since_zero_is_everything_since_this_run_started",
          "[test_automation_fix_events_gap_reason]") {
    auto [position, bus] = restart();
    CHECK(bus->since(0) == since_result({}, std::nullopt));
    const auto published = publish_n(*bus, 2);
    CHECK(bus->since(0) == since_result(published, std::nullopt));
}

TEST_CASE("test_a_position_from_before_a_restart_is_told_apart_from_a_loss",
          "[test_automation_fix_events_gap_reason]") {
    auto [position, bus] = restart();
    const auto published = publish_n(*bus, 2);
    CHECK(bus->since(position) == since_result(published, catalog::GAP_RESTARTED));
    // It still never matches an event of this run.
    for (const Event& event : published) {
        CHECK(event.seq > position);
    }
}

TEST_CASE("test_events_this_run_dropped_are_a_loss_from_any_earlier_position",
          "[test_automation_fix_events_gap_reason]") {
    auto [position, bus] = restart(2);
    const auto published = publish_n(*bus, 3);
    const Event& first = published.front();
    const std::vector<Event> held(published.begin() + 1, published.end());
    for (const std::int64_t start : {position, std::int64_t{0}, first.seq - 1}) {
        CHECK(bus->since(start) == since_result(held, catalog::GAP_EVICTED));
    }
    // A client that already had the dropped one missed nothing.
    CHECK(bus->since(first.seq) == since_result(held, std::nullopt));
}

TEST_CASE("test_deleted_history_is_a_loss_only_for_what_the_client_had_not_seen",
          "[test_automation_fix_events_gap_reason]") {
    auto [position, bus] = restart();
    const auto published = publish_n(*bus, 2);
    bus->clear();
    CHECK(bus->since(published[0].seq) == since_result({}, catalog::GAP_EVICTED));
    CHECK(bus->since(published[1].seq) == since_result({}, std::nullopt));
}

// The stream (M5.16) replays bus().since(Last-Event-ID); from 0 on a fresh run it is every
// event of this run and no gap.
TEST_CASE("test_a_stream_from_the_start_sends_no_gap", "[test_automation_fix_events_gap_reason]") {
    auto [position, fresh] = restart();
    std::shared_ptr<EventBus> app_bus(std::move(fresh));
    ScopedBus scoped(app_bus);
    const auto published = publish_n(bus(), 2);
    const auto result = bus().since(0);
    CHECK_FALSE(result.gap);
    CHECK(result.events == published);
}

// MCP recent_events (M5.9) reports `gap` as since()'s answer being set.
TEST_CASE("test_recent_events_from_zero_reports_no_gap_on_a_fresh_run",
          "[test_automation_fix_events_gap_reason]") {
    auto [position, fresh] = restart();
    std::shared_ptr<EventBus> app_bus(std::move(fresh));
    ScopedBus scoped(app_bus);
    const auto published = publish_n(bus(), 2);
    const auto page = bus().since(0);
    const auto earlier = bus().since(position);
    CHECK_FALSE(page.gap.has_value());
    CHECK(seqs(page.events) == seqs(published));
    // Events the earlier run published after that position are gone.
    CHECK(earlier.gap.has_value());
}

// ---- test_automation_r3_gap_per_type.py ----

TEST_CASE("test_other_types_evicted_are_no_gap_for_a_client_that_doesnt_follow_them",
          "[test_automation_r3_gap_per_type]") {
    EventBus bus(5);
    const Event seen = channel(bus, "fix the failing test");
    auto all_segments = segments(bus, 6);
    const std::vector<Event> held(all_segments.begin() + 1, all_segments.end());

    CHECK(bus.since(seen.seq, kChannel) == since_result({}, std::nullopt));
    CHECK(bus.since(seen.seq, std::vector<std::string>{std::string(catalog::MEETING_SEGMENTS)}) ==
          since_result(held, catalog::GAP_EVICTED));
    CHECK(bus.since(seen.seq) == since_result(held, catalog::GAP_EVICTED));
}

TEST_CASE("test_a_followed_type_evicted_after_the_position_is_a_gap",
          "[test_automation_r3_gap_per_type]") {
    EventBus bus(5);
    const Event seen = channel(bus, "first");
    channel(bus, "second, never received");
    segments(bus, 5);

    CHECK(bus.since(seen.seq, kChannel) == since_result({}, catalog::GAP_EVICTED));
    const Event newest = bus.recent(1).front();
    CHECK(bus.since(newest.seq, kChannel) == since_result({}, std::nullopt));
}

TEST_CASE("test_since_zero_is_no_gap_when_only_other_types_were_evicted",
          "[test_automation_r3_gap_per_type]") {
    EventBus bus(3, 2'000);
    segments(bus, 5);
    const Event latest = channel(bus, "hello");

    CHECK(bus.since(0, kChannel) == since_result({latest}, std::nullopt));
    CHECK(bus.since(0).gap == std::string(catalog::GAP_EVICTED));
}

TEST_CASE("test_a_position_from_before_a_restart_still_reads_as_restarted",
          "[test_automation_r3_gap_per_type]") {
    EventBus earlier(500, 1'000);
    const std::int64_t position = channel(earlier, "before the restart").seq;
    earlier.close();
    EventBus bus(3, 2'000);
    segments(bus, 5);
    const Event latest = channel(bus, "after the restart");

    CHECK(bus.since(position, kChannel) == since_result({latest}, catalog::GAP_RESTARTED));
    CHECK(bus.since(position).gap == std::string(catalog::GAP_EVICTED));
}

TEST_CASE("test_a_clear_is_a_gap_only_for_cleared_types_the_client_hadnt_got",
          "[test_automation_r3_gap_per_type]") {
    EventBus bus;
    const Event seen = channel(bus, "fix the failing test");
    bus.publish_event(catalog::HISTORY_SAVED,
                      Json{{"job_id", "j"}, {"record_id", "1"}, {"kind", "action"}});
    bus.clear();

    CHECK(bus.since(seen.seq, kChannel) == since_result({}, std::nullopt));
    CHECK(bus.since(seen.seq, std::vector<std::string>{std::string(catalog::HISTORY_SAVED)}) ==
          since_result({}, catalog::GAP_EVICTED));

    const Event unseen = channel(bus, "a message the client never got");
    bus.clear();
    CHECK(bus.since(seen.seq, kChannel) == since_result({}, catalog::GAP_EVICTED));
    CHECK(bus.since(unseen.seq, kChannel) == since_result({}, std::nullopt));
}

// The next three are the channel bridge reconnecting with Last-Event-ID (M5.16's stream and
// M5.20's bridge repeat them end to end): the stream sends an events.gap only when since()
// reports one for the followed types, and replays the events since() returns.
TEST_CASE("test_a_meeting_filling_the_buffer_tells_claude_nothing",
          "[test_automation_r3_gap_per_type]") {
    EventBus bus(50);
    const Event seen = channel(bus, "fix the failing test");
    segments(bus, 60);
    CHECK(bus.since(seen.seq, kChannel) == since_result({}, std::nullopt));
}

TEST_CASE("test_deleted_history_tells_claude_nothing_when_no_message_was_missed",
          "[test_automation_r3_gap_per_type]") {
    EventBus bus(50);
    const Event seen = channel(bus, "fix the failing test");
    bus.publish_event(catalog::HISTORY_SAVED,
                      Json{{"job_id", "j"}, {"record_id", "1"}, {"kind", "action"}});
    bus.clear();
    CHECK(bus.since(seen.seq, kChannel) == since_result({}, std::nullopt));
}

TEST_CASE("test_a_message_dropped_while_away_is_still_announced",
          "[test_automation_r3_gap_per_type]") {
    EventBus bus(50);
    const Event seen = channel(bus, "fix the failing test");
    channel(bus, "and run it again");
    segments(bus, 50);
    const auto result = bus.since(seen.seq, kChannel);
    CHECK(result.events.empty());
    CHECK(result.gap == std::string(catalog::GAP_EVICTED));
}

// MCP recent_events (M5.9) asks since() with the client's types.
TEST_CASE("test_recent_events_reports_a_gap_only_for_the_types_asked",
          "[test_automation_r3_gap_per_type]") {
    EventBus bus(50);
    const Event seen = channel(bus, "fix the failing test");
    segments(bus, 60);
    const auto followed = bus.since(seen.seq, kChannel);
    const auto everything = bus.since(seen.seq);
    CHECK_FALSE(followed.gap.has_value());
    CHECK(followed.events.empty());
    CHECK(everything.gap.has_value());
}

// ---- test_automation_fix_pipeline_events.py ----

namespace {

// fresh_bus: lets bus() create the process-wide bus anew, then puts the previous one back.
class FreshBus {
public:
    FreshBus() : previous_(set_bus(nullptr)) {}
    ~FreshBus() {
        set_run_clock(nullptr);
        if (auto created = set_bus(previous_)) {
            created->close();
        }
    }

private:
    std::shared_ptr<EventBus> previous_;
};

std::chrono::system_clock::time_point at_ms(std::int64_t ms) {
    return std::chrono::system_clock::time_point(
        std::chrono::duration_cast<std::chrono::system_clock::duration>(
            std::chrono::milliseconds(ms)));
}

} // namespace

TEST_CASE("test_the_app_bus_numbers_from_a_base_that_grows_across_runs",
          "[test_automation_fix_pipeline_events]") {
    FreshBus fresh;
    fv::FakeClock clock(at_ms(1'790'000'000'123));
    set_run_clock(&clock);
    const Event first = bus().publish_event("recording.started", Json::object());
    CHECK(first.seq == 1'790'000'000'123 + 1);
    CHECK(std::to_string(first.seq).size() <= 18);

    // The next run starts later, so every number it gives is new.
    set_bus(nullptr)->close();
    clock.set_utc(at_ms(1'790'000'060'000));
    const Event second = bus().publish_event("recording.started", Json::object());
    CHECK(second.seq > first.seq);
}

TEST_CASE("test_a_position_from_an_earlier_run_reads_as_a_restart",
          "[test_automation_fix_pipeline_events]") {
    FreshBus fresh;
    fv::FakeClock clock(at_ms(1'790'000'000'000));
    set_run_clock(&clock);
    EventBus& app = bus();
    const std::int64_t base = app.last_seq();
    const Event one = app.publish_event("recording.started", Json::object());

    // The earlier run's last event had a smaller number than this run's base.
    auto result = app.since(base - 5);
    CHECK(seqs(result.events) == std::vector<std::int64_t>{one.seq});
    CHECK(result.gap == std::string(catalog::GAP_RESTARTED));
    // 0 asks for everything since this run started, none of which is missing.
    CHECK(app.since(0) == since_result({one}, std::nullopt));
    // From this run's own positions, nothing was missed.
    result = app.since(base);
    CHECK(seqs(result.events) == std::vector<std::int64_t>{one.seq});
    CHECK_FALSE(result.gap);
    CHECK(app.since(one.seq) == since_result({}, std::nullopt));
}

TEST_CASE("test_a_bus_made_directly_still_counts_from_one",
          "[test_automation_fix_pipeline_events]") {
    EventBus bus;
    CHECK(bus.publish_event("recording.started", Json::object()).seq == 1);
    CHECK(bus.since(0) == since_result(bus.recent(), std::nullopt));
}

TEST_CASE("test_a_late_subscriber_never_gets_events_published_before_it_subscribed",
          "[test_automation_fix_pipeline_events]") {
    EventBus bus;
    Gate release;
    Gate busy;
    auto slow = bus.subscribe([&](const Event&) {
        busy.set();
        release.wait(2s);
    });
    bus.publish_event("recording.started", Json{{"n", 1}});
    REQUIRE(busy.wait(2s)); // the dispatcher is held on the first event
    bus.publish_event("recording.stopped", Json{{"n", 2}}); // queued, not yet handed out
    RecordingBusSubscriber late(bus);
    release.set();
    const Event after = bus.publish_event("history.saved", Json{{"n", 3}});
    REQUIRE(late.wait_for(1));
    std::this_thread::sleep_for(50ms);
    CHECK(seqs(late.events()) == std::vector<std::int64_t>{after.seq});
    bus.close();
}

TEST_CASE("test_replacing_a_subscription_new_before_old_loses_nothing",
          "[test_automation_fix_pipeline_events]") {
    EventBus bus;
    Gate release;
    Gate busy;
    auto slow = bus.subscribe([&](const Event&) {
        busy.set();
        release.wait(2s);
    });
    auto old_seen = std::make_unique<RecordingBusSubscriber>(bus);
    bus.publish_event("a", Json::object());
    REQUIRE(busy.wait(2s));
    bus.publish_event("b", Json::object()); // published while only the old one is open
    RecordingBusSubscriber new_seen(bus);
    old_seen->subscription().close();
    bus.publish_event("c", Json::object());
    release.set();
    REQUIRE(new_seen.wait_for(1));
    REQUIRE(old_seen->wait_for(2));
    std::this_thread::sleep_for(50ms);
    CHECK(old_seen->types() == std::vector<std::string>{"a", "b"});
    CHECK(new_seen.types() == std::vector<std::string>{"c"});
    bus.close();
}

TEST_CASE("test_a_lone_surrogate_is_refused_at_the_publisher",
          "[test_automation_fix_pipeline_events]") {
    EventBus bus;
    // "hi \ud83d" as json.loads hands it over: the surrogate in three bytes.
    CHECK_THROWS_AS(bus.publish_event("channel.message", Json{{"text", "hi \xED\xA0\xBD"}}),
                    fv::py::TypeError);
    CHECK(bus.last_seq() == 0);
    CHECK(bus.recent().empty());
    const Event ok = bus.publish_event("channel.message", Json{{"text", "hi \xF0\x9F\x98\x80"}});
    CHECK(ok.seq == 1);
}

TEST_CASE("test_the_module_publish_drops_it_quietly", "[test_automation_fix_pipeline_events]") {
    ScopedBus scoped(std::make_shared<EventBus>());
    CHECK_FALSE(publish("channel.message", Json{{"text", "\xED\xB0\x80"}}).has_value());
    CHECK(bus().recent().empty());
    // So does the EventSink a lower layer publishes through.
    fv::EventSink& sink = bus();
    CHECK_NOTHROW(sink.publish("channel.message", Json{{"text", "\xED\xB0\x80"}}));
    CHECK(bus().recent().empty());
}

TEST_CASE("test_clear_on_the_module_forgets_only_an_existing_bus",
          "[test_automation_fix_pipeline_events]") {
    FreshBus fresh;
    clear_bus(); // creates nothing
    CHECK(set_bus(nullptr) == nullptr);
}

// ---- acceptance: a slow subscriber is reported once and never blocks a publisher ----

TEST_CASE("a subscriber that sleeps 1.5 s logs one warning and never blocks publish",
          "[event_bus]") {
    ScopedLog log;
    EventBus bus;
    std::atomic<int> calls{0};
    auto slow = bus.subscribe(
        [&](const Event&) {
            if (calls.fetch_add(1) == 0) {
                std::this_thread::sleep_for(1500ms);
            }
        },
        std::nullopt, "sse");
    RecordingBusSubscriber after(bus);
    const auto started = std::chrono::steady_clock::now();
    bus.publish_event("recording.started", Json::object());
    bus.publish_event("recording.stopped", Json::object());
    CHECK(std::chrono::steady_clock::now() - started < 500ms);
    REQUIRE(after.wait_for(2, 5s));
    std::vector<std::string> slow_lines;
    for (const auto& message : log.messages()) {
        if (message.find("took") != std::string::npos) {
            slow_lines.push_back(message);
        }
    }
    REQUIRE(slow_lines.size() == 1);
    CHECK(slow_lines.front().starts_with("Event subscriber sse took 1."));
    CHECK(slow_lines.front().ends_with(" s; callbacks must only enqueue"));
    bus.close();
}

TEST_CASE("a failing subscriber is logged by its label and the error's type only", "[event_bus]") {
    ScopedLog log;
    EventBus bus;
    auto bad = bus.subscribe([](const Event&) { throw fv::py::ValueError("my secret words"); },
                             std::nullopt, "webhooks");
    RecordingBusSubscriber good(bus);
    bus.publish_event("recording.started", Json::object());
    REQUIRE(good.wait_for(1));
    bus.close();
    CHECK(log.messages() ==
          std::vector<std::string>{"Event subscriber webhooks failed: ValueError"});
}

TEST_CASE("a subscription closing itself and the bus closing from a subscriber do not hang",
          "[event_bus]") {
    auto bus = std::make_shared<EventBus>();
    Gate done;
    std::shared_ptr<EventSubscription> self;
    self = bus->subscribe([&](const Event&) {
        self->close();
        bus->close();
        done.set();
    });
    bus->publish_event("recording.started", Json::object());
    CHECK(done.wait(2s));
    CHECK(self->closed());
    CHECK(bus->subscribers() == 0);
}

TEST_CASE("a subscription outlives its bus", "[event_bus]") {
    std::shared_ptr<EventSubscription> subscription;
    {
        EventBus bus;
        subscription = bus.subscribe([](const Event&) {});
    }
    CHECK(subscription->closed());
    CHECK_NOTHROW(subscription->close());
}

TEST_CASE("events carry an evt_ id, the bus clock's UTC time and ASCII-safe JSON", "[event_bus]") {
    fv::FakeClock clock(at_ms(1'790'000'000'045));
    EventBus bus(500, 0, clock);
    const Event event = bus.publish_event("history.saved", Json{{"text", "caf\xC3\xA9"}});
    CHECK(event.time == "2026-09-21T14:13:20.045Z");
    CHECK(event.to_json() == "{\"id\":\"" + event.id +
                                 "\",\"seq\":1,\"type\":\"history.saved\",\"time\":\"2026-09-21T14:"
                                 "13:20.045Z\",\"v\":1,\"data\":{\"text\":\"caf\xC3\xA9\"}}");
    CHECK(event.to_json(true).ends_with("\"data\":{\"text\":\"caf\\u00e9\"}}"));
    CHECK(run_seq_base(clock) == 1'790'000'000'045);
}
