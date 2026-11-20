#pragma once

// Helpers for the dashboard server tests: a repository answering the hub's two reads, a log
// capture, golden-value untagging and a WebSocket client wrapper. Everything talks to 127.0.0.1.

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "base/json.h"
#include "base/log.h"
#include "meeting_state/repository.h"
#include "net/testing/ws_test_client.h"
#include "pyport/json.h"

namespace fv::meeting_web::test_support {

using Json = fv::Json;

// A MeetingRepository whose methods fail the test unless overridden here: the hub reads a
// meeting row and the last segments, nothing else.
class HelloRepository final : public meeting_state::MeetingRepository {
public:
    HelloRepository(Json meeting, std::vector<Json> segments)
        : meeting_(std::move(meeting)), segments_(std::move(segments)) {}

    // Runs once, inside the next get_last_segments: a change landing while a hello is built.
    void on_get_last_segments(std::function<void()> hook) {
        std::lock_guard lock(mutex_);
        hook_ = std::move(hook);
    }
    void fail_get_meeting(bool fail) {
        std::lock_guard lock(mutex_);
        fail_get_meeting_ = fail;
    }
    void set_meeting(Json meeting) {
        std::lock_guard lock(mutex_);
        meeting_ = std::move(meeting);
    }

    std::optional<Json> get_meeting(std::string_view meeting_id) override {
        std::lock_guard lock(mutex_);
        if (fail_get_meeting_) {
            throw std::runtime_error("database is locked");
        }
        if (meeting_.is_object() && meeting_.value("id", std::string()) == meeting_id) {
            return meeting_;
        }
        return std::nullopt;
    }
    std::vector<Json> get_last_segments(std::string_view, std::int64_t count) override {
        std::function<void()> hook;
        std::vector<Json> segments;
        {
            std::lock_guard lock(mutex_);
            hook = std::exchange(hook_, {});
            segments = segments_;
        }
        if (hook) {
            hook();
        }
        if (static_cast<std::int64_t>(segments.size()) > count) {
            segments.erase(segments.begin(), segments.end() - count);
        }
        return segments;
    }

    void create_meeting(const Json&) override { unused("create_meeting"); }
    void update_meeting(std::string_view, const Json&) override { unused("update_meeting"); }
    void rename_meeting(std::string_view, std::string_view) override { unused("rename_meeting"); }
    void replace_tokens(std::string_view, std::string_view, std::string_view) override {
        unused("replace_tokens");
    }
    std::vector<Json> list_meetings() override { unused("list_meetings"); }
    void delete_meeting(std::string_view) override { unused("delete_meeting"); }
    void heartbeat(std::string_view, std::int64_t, std::string_view) override {
        unused("heartbeat");
    }
    std::vector<Json> find_interrupted_meetings() override { unused("find_interrupted_meetings"); }
    std::int64_t register_chunk(const Json&) override { unused("register_chunk"); }
    meeting_state::ChunkRegistration register_chunk_if_missing(const Json&) override {
        unused("register_chunk_if_missing");
    }
    void set_chunk_status(std::int64_t, std::string_view,
                          std::optional<std::string_view>) override {
        unused("set_chunk_status");
    }
    std::vector<Json> get_pending_chunks(std::string_view, std::optional<std::int64_t>,
                                         const std::vector<std::int64_t>&) override {
        unused("get_pending_chunks");
    }
    std::int64_t count_unfinished_chunks(std::string_view) override {
        unused("count_unfinished_chunks");
    }
    std::int64_t reset_unfinished_chunks(std::string_view, bool) override {
        unused("reset_unfinished_chunks");
    }
    void defer_chunk_after_connection_failure(std::int64_t, std::string_view) override {
        unused("defer_chunk_after_connection_failure");
    }
    std::vector<Json> get_audio_chunks(std::string_view) override { unused("get_audio_chunks"); }
    std::int64_t next_chunk_seq(std::string_view, std::string_view) override {
        unused("next_chunk_seq");
    }
    std::int64_t mark_chunks_done(std::string_view) override { unused("mark_chunks_done"); }
    void add_segments(const std::vector<meeting_state::TranscriptSegment>&) override {
        unused("add_segments");
    }
    meeting_state::ChunkCommit
    commit_chunk_transcription(std::string_view, std::int64_t,
                               const std::vector<meeting_state::TranscriptSegment>&) override {
        unused("commit_chunk_transcription");
    }
    std::optional<Json> get_segment(std::string_view, std::string_view) override {
        unused("get_segment");
    }
    bool segment_exists(std::string_view, std::string_view) override { unused("segment_exists"); }
    std::map<std::string, bool, std::less<>>
    segment_flags(std::string_view, const std::vector<std::string>&) override {
        unused("segment_flags");
    }
    void update_segment_speaker(std::string_view, std::string_view,
                                const std::optional<std::string>&, std::string_view,
                                bool) override {
        unused("update_segment_speaker");
    }
    std::vector<Json> get_segments(std::string_view, double, std::optional<std::int64_t>) override {
        unused("get_segments");
    }
    std::vector<Json> get_segments_page(std::string_view, std::optional<double>,
                                        const std::optional<std::string>&, std::int64_t) override {
        unused("get_segments_page");
    }
    meeting_state::TranscriptReplacement
    replace_final_transcript(std::string_view,
                             const std::vector<meeting_state::TranscriptSegment>&) override {
        unused("replace_final_transcript");
    }
    std::vector<Json> get_segments_delivery_page(std::string_view, std::int64_t,
                                                 std::int64_t) override {
        unused("get_segments_delivery_page");
    }
    std::int64_t get_delivery_cursor(std::string_view, std::string_view) override {
        unused("get_delivery_cursor");
    }
    void advance_delivery_cursor(std::string_view, std::string_view, std::int64_t) override {
        unused("advance_delivery_cursor");
    }
    std::int64_t get_delivery_high_water(std::string_view) override {
        unused("get_delivery_high_water");
    }
    Json on_ops_applied(std::string_view, const Json&, std::vector<meeting_state::OpResult>&,
                        std::string_view, const std::optional<std::string>&) override {
        unused("on_ops_applied");
    }
    Json persist_state(std::string_view, const Json&) override { unused("persist_state"); }
    std::optional<Json> get_event(std::string_view, std::int64_t) override { unused("get_event"); }
    bool event_is_undone(std::string_view, std::int64_t) override { unused("event_is_undone"); }
    std::vector<Json> list_events(std::string_view, std::optional<std::int64_t>,
                                  std::int64_t) override {
        unused("list_events");
    }
    std::vector<Json> search_transcripts(std::string_view, const std::optional<std::string>&,
                                         std::int64_t) override {
        unused("search_transcripts");
    }

private:
    [[noreturn]] static void unused(const char* method) {
        throw std::logic_error(std::string("the test repository has no ") + method);
    }

    std::mutex mutex_;
    Json meeting_;
    std::vector<Json> segments_;
    std::function<void()> hook_;
    bool fail_get_meeting_ = false;
};

class LogCapture final : public log::Sink {
public:
    void write(const log::Record& record) override {
        std::lock_guard lock(mutex_);
        lines_.push_back(std::string(record.logger) + ": " + std::string(record.message));
    }
    std::vector<std::string> lines() const {
        std::lock_guard lock(mutex_);
        return lines_;
    }

private:
    mutable std::mutex mutex_;
    std::vector<std::string> lines_;
};

// Captures every record, DEBUG included, for the scope.
struct ScopedLogCapture {
    ScopedLogCapture() : capture(std::make_shared<LogCapture>()), previous(log::level()) {
        log::set_level(log::Level::Debug);
        log::add_sink(capture);
    }
    ~ScopedLogCapture() {
        log::remove_sink(capture);
        log::set_level(previous);
    }
    ScopedLogCapture(const ScopedLogCapture&) = delete;
    ScopedLogCapture& operator=(const ScopedLogCapture&) = delete;

    std::shared_ptr<LogCapture> capture;
    log::Level previous;
};

// A golden value with its tagged floats ({"$float": "nan" | "inf" | "-inf"}) as doubles.
Json untag(const Json& value);

// One dashboard socket of a test.
class Client {
public:
    static constexpr std::chrono::milliseconds kWait{5000};

    // Connects to /ws?<query>; the upgrade itself must succeed.
    Client(std::uint16_t port, std::string_view query);

    // The next frame as JSON; fails the test when none arrives within kWait.
    Json receive();
    // The next frame's text as the server wrote it; fails the test when none arrives.
    std::string receive_text();
    // Frames until one of `type` (at most `limit` of them).
    Json receive_until(std::string_view type, int limit = 10);
    // nullopt when nothing arrives within `wait`.
    std::optional<Json> try_receive(std::chrono::milliseconds wait);
    void send(const Json& message);
    void send_text(std::string_view text);
    void send_binary(std::string_view bytes);
    // Waits for the server's close and returns its code.
    std::optional<std::uint16_t> closed_with(std::chrono::milliseconds wait = kWait);
    void close();
    net::testing::WsTestClient& raw() { return client_; }

private:
    net::testing::WsTestClient client_;
};

} // namespace fv::meeting_web::test_support
