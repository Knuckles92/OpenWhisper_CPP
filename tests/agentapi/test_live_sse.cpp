// The live event stream: tests/test_automation_api_sse.py over real sockets (the listener's app
// on a loopback port, read by fv_http's curl client as a client reads a stream), and
// tests/test_automation_fix_api_sse_order.py and the slow-reader test on agentapi::EventStream
// itself, with a fake socket.
#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "agentapi/history_server.h"
#include "agentapi/live_api.h"
#include "agentapi/mcp_runtime.h"
#include "agentapi/sse_stream.h"
#include "automation/catalog.h"
#include "automation/control.h"
#include "automation/event_bus.h"
#include "automation/filters.h"
#include "automation/permissions.h"
#include "base/cancel.h"
#include "base/executor.h"
#include "base/identity.h"
#include "base/testing/fake_clock.h"
#include "base/testing/manual_executor.h"
#include "credentials/store.h"
#include "credentials/testing/memory_backend.h"
#include "http/client.h"
#include "live_support.h"
#include "net/http_server.h"
#include "net/reactor.h"
#include "pyport/json.h"
#include "settings/testing/memory_store.h"
#include "support.h"
#include "testkit/testkit.h"

namespace {

using fv::Json;
using namespace fv::agentapi;
using namespace fv::agentapi::test;
using namespace std::chrono_literals;
namespace catalog = fv::automation::catalog;
namespace permissions = fv::automation::permissions;
using Message = std::map<std::string, std::string>;

const std::string kSseToken = "live-sse-test-token-" + std::string(32, 'c');

// One GET of an event stream through the curl client, read on a thread of its own as Python's
// Reader reads it: messages of fields, or {"comment": ...}.
class SseReader {
public:
    SseReader(std::uint16_t port, const std::string& path, const std::string& token,
              std::vector<std::pair<std::string, std::string>> headers = {}) {
        fv::http::Request request;
        request.url = "http://127.0.0.1:" + std::to_string(port) + path;
        request.bypass_proxy = true;
        request.connect_timeout = 5s;
        request.idle_timeout = 20s;
        request.headers.emplace_back("Authorization", "Bearer " + token);
        for (auto& header : headers) {
            request.headers.push_back(std::move(header));
        }
        thread_ = std::thread([this, request] {
            auto client = fv::http::make_curl_client();
            Sink sink(*this);
            const auto result = client->send(request, cancel_.token(), &sink);
            std::lock_guard lock(mutex_);
            failed_ = !result.has_value();
            ended_ = true;
            changed_.notify_all();
        });
    }
    ~SseReader() { close(); }
    SseReader(const SseReader&) = delete;
    SseReader& operator=(const SseReader&) = delete;

    // Drops the connection, as a client that goes away.
    void close() {
        cancel_.cancel();
        if (thread_.joinable()) {
            thread_.join();
        }
    }

    // The response head, once it arrived.
    std::optional<fv::http::Response> head(std::chrono::milliseconds timeout = 5s) {
        std::unique_lock lock(mutex_);
        changed_.wait_for(lock, timeout, [&] { return head_.has_value() || ended_; });
        return head_;
    }

    // The next message; nullopt once the stream ended (or nothing came within `timeout`).
    std::optional<Message> message(std::chrono::milliseconds timeout = 5s) {
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        Message fields;
        while (true) {
            std::optional<std::string> line = next_line(deadline);
            if (!line) {
                return fields.empty() ? std::nullopt : std::optional(fields);
            }
            if (line->empty()) {
                if (!fields.empty()) {
                    return fields;
                }
                continue;
            }
            if (line->front() == ':') {
                std::string comment = line->substr(1);
                while (!comment.empty() && comment.front() == ' ') {
                    comment.erase(0, 1);
                }
                return Message{{"comment", comment}};
            }
            const std::size_t colon = line->find(':');
            std::string value = colon == std::string::npos ? "" : line->substr(colon + 1);
            if (value.starts_with(' ')) {
                value.erase(0, 1);
            }
            fields[line->substr(0, colon)] = value;
        }
    }

    // The next message with an event type.
    std::optional<Message> event(std::chrono::milliseconds timeout = 5s) {
        while (true) {
            std::optional<Message> next = message(timeout);
            if (!next || next->contains("event")) {
                return next;
            }
        }
    }

    // Every message until the stream ends.
    std::vector<Message> rest(std::chrono::milliseconds timeout = 5s) {
        std::vector<Message> out;
        while (std::optional<Message> next = message(timeout)) {
            out.push_back(*next);
        }
        return out;
    }

    bool ended(std::chrono::milliseconds timeout) {
        std::unique_lock lock(mutex_);
        return changed_.wait_for(lock, timeout, [&] { return ended_; });
    }
    bool failed() {
        std::lock_guard lock(mutex_);
        return failed_;
    }

private:
    class Sink final : public fv::http::BodySink {
    public:
        explicit Sink(SseReader& reader) : reader_(reader) {}
        bool on_head(const fv::http::Response& head) override {
            std::lock_guard lock(reader_.mutex_);
            reader_.head_ = head;
            reader_.changed_.notify_all();
            return true;
        }
        bool write(std::span<const char> bytes) override {
            std::lock_guard lock(reader_.mutex_);
            reader_.buffer_.append(bytes.data(), bytes.size());
            reader_.changed_.notify_all();
            return true;
        }

    private:
        SseReader& reader_;
    };

    std::optional<std::string> next_line(std::chrono::steady_clock::time_point deadline) {
        std::unique_lock lock(mutex_);
        changed_.wait_until(lock, deadline,
                            [&] { return buffer_.find('\n') != std::string::npos || ended_; });
        const std::size_t end = buffer_.find('\n');
        if (end == std::string::npos) {
            return std::nullopt;
        }
        std::string line = buffer_.substr(0, end);
        buffer_.erase(0, end + 1);
        if (line.ends_with('\r')) {
            line.pop_back();
        }
        return line;
    }

    std::mutex mutex_;
    std::condition_variable changed_;
    std::string buffer_;
    std::optional<fv::http::Response> head_;
    bool ended_ = false;
    bool failed_ = false;
    fv::CancelSource cancel_;
    std::thread thread_;
};

// The in-app listener's app with the live API on a loopback port, as LiveServer runs it under
// uvicorn: requests go to the io strand, as McpRuntime sends them.
struct LiveServer {
    explicit LiveServer(std::size_t buffer_size = fv::automation::DEFAULT_BUFFER_SIZE)
        : database(copy_history_fixture(root.path(), "live.db")),
          bus(std::make_shared<fv::automation::EventBus>(buffer_size)),
          live(settings, [this] { return on.load(); }, *bus, leases, io) {
        McpAppOptions options;
        options.database = database;
        options.token = kSseToken;
        options.enabled = [this] {
            return on.load();
        };
        options.settings = &settings;
        app = std::make_shared<McpApp>(std::move(options));
        live.install(app->live_routes());
        server = fv::net::HttpServer::create(reactor);
        const auto endpoint = server->listen({"127.0.0.1", 0, std::nullopt, true});
        REQUIRE(endpoint);
        port = endpoint->port;
        server->set_fallback([app = app, strand = &io](const fv::net::HttpRequest& request, bool,
                                                       std::shared_ptr<fv::net::Responder> out) {
            auto copy = std::make_shared<fv::net::HttpRequest>(request);
            strand->post(
                [app, copy, out = std::move(out)]() mutable { app->serve(*copy, std::move(out)); });
        });
    }
    ~LiveServer() {
        stop(0ms);
        live.close_streams();
        bus->close();
    }
    void stop(std::chrono::milliseconds grace) {
        if (server) {
            server->stop(grace);
            server.reset();
        }
    }

    std::unique_ptr<SseReader> open(const std::string& path = "/v1/events",
                                    std::vector<std::pair<std::string, std::string>> headers = {}) {
        return std::make_unique<SseReader>(port, path, kSseToken, std::move(headers));
    }
    Json listeners() {
        auto client = fv::http::make_curl_client();
        fv::http::Request request;
        request.url = "http://127.0.0.1:" + std::to_string(port) + "/v1/live/state";
        request.bypass_proxy = true;
        request.headers.emplace_back("Authorization", "Bearer " + kSseToken);
        const auto response = client->send(request, {});
        REQUIRE(response);
        REQUIRE(response->status == 200);
        return fv::py::loads(response->body).at("listeners");
    }

    fv::testkit::TempDataRoot root;
    std::filesystem::path database;
    fv::MemorySettingsStore settings;
    std::atomic<bool> on{true};
    fv::net::Reactor reactor{2};
    fv::ThreadPool pool{"live-sse", 4};
    fv::Strand io{pool, "live-sse-io", 4, fv::ShutdownMode::CancelQueued};
    std::shared_ptr<fv::automation::EventBus> bus;
    fv::automation::FilterLeases leases;
    std::shared_ptr<RouteControl> control = std::make_shared<RouteControl>();
    ScopedAppControl registration{control};
    LiveApi live;
    std::shared_ptr<McpApp> app;
    std::unique_ptr<fv::net::HttpServer> server;
    std::uint16_t port = 0;
};

Json payload(const Message& message) {
    return fv::py::loads(message.at("data"));
}

std::string seq_of(const fv::automation::Event& event) {
    return std::to_string(event.seq);
}

} // namespace

// --- tests/test_automation_api_sse.py
// --------------------------------------------------------------

TEST_CASE("test_events_arrive_live_as_server_sent_events", "[platform][test_automation_api_sse]") {
    LiveServer s;
    auto reader = s.open();
    const auto head = reader->head();
    REQUIRE(head);
    CHECK(head->status == 200);
    CHECK(fv::http::find_header(head->headers, "content-type") ==
          "text/event-stream; charset=utf-8");
    CHECK(fv::http::find_header(head->headers, "cache-control") == "no-store");
    CHECK(fv::http::find_header(head->headers, "x-accel-buffering") == "no");
    CHECK(reader->message() == Message{{"retry", "2000"}});
    const auto published =
        s.bus->publish_event(catalog::RECORDING_STARTED, {{"job_id", "j1"}, {"mode", "dictation"}});
    const auto message = reader->event();
    REQUIRE(message);
    CHECK(message->at("id") == seq_of(published));
    CHECK(message->at("event") == "recording.started");
    CHECK(payload(*message) == published.to_dict());
}

TEST_CASE("test_events_published_as_the_stream_opens_are_not_missed",
          "[platform][test_automation_api_sse]") {
    LiveServer s;
    auto reader = s.open();
    REQUIRE(reader->head());
    // The client sees the head before the stream may have subscribed; an event published now
    // must still arrive.
    const auto published = s.bus->publish_event(
        catalog::HISTORY_SAVED, {{"job_id", "j"}, {"record_id", "1"}, {"kind", "x"}});
    const auto message = reader->event();
    REQUIRE(message);
    CHECK(message->at("id") == seq_of(published));
}

TEST_CASE("test_line_break_characters_in_text_survive_line_readers",
          "[platform][test_automation_api_sse]") {
    LiveServer s;
    const std::string text = "one\xE2\x80\xA8two\xE2\x80\xA9three\xC2\x85"
                             "four\nfive";
    auto reader = s.open();
    REQUIRE(reader->message());
    s.bus->publish_event(catalog::TRANSCRIPTION_COMPLETED, {{"job_id", "j"}, {"text", text}});
    const auto message = reader->event();
    REQUIRE(message);
    CHECK(payload(*message).at("data").at("text") == text);
}

TEST_CASE("test_types_limit_the_stream", "[platform][test_automation_api_sse]") {
    LiveServer s;
    auto reader = s.open("/v1/events?types=history.saved,recording.stopped");
    REQUIRE(reader->message());
    s.bus->publish_event(catalog::RECORDING_STARTED, {{"job_id", "j"}});
    const auto stopped =
        s.bus->publish_event(catalog::RECORDING_STOPPED, {{"job_id", "j"}, {"duration_s", 1.0}});
    s.bus->publish_event(catalog::TRANSCRIPTION_PREVIEW, {{"job_id", "j"}, {"text", "x"}}, false);
    const auto saved = s.bus->publish_event(catalog::HISTORY_SAVED,
                                            {{"job_id", "j"}, {"record_id", "1"}, {"kind", "x"}});
    const auto first = reader->event();
    const auto second = reader->event();
    REQUIRE(first);
    REQUIRE(second);
    CHECK(std::vector<std::string>{first->at("id"), second->at("id")} ==
          std::vector<std::string>{seq_of(stopped), seq_of(saved)});
}

TEST_CASE("test_reconnecting_with_last_event_id_replays_what_was_missed",
          "[platform][test_automation_api_sse]") {
    LiveServer s;
    auto reader = s.open();
    REQUIRE(reader->message());
    const auto first = s.bus->publish_event(catalog::RECORDING_STARTED, {{"job_id", "j"}});
    REQUIRE(reader->event()->at("id") == seq_of(first));
    reader->close();
    const std::vector<fv::automation::Event> missed = {
        s.bus->publish_event(catalog::RECORDING_STOPPED, {{"job_id", "j"}, {"duration_s", 2.0}}),
        s.bus->publish_event(catalog::TRANSCRIPTION_COMPLETED,
                             {{"job_id", "j"}, {"text", "hello"}})};
    // EventSource reconnects to its original URL; the header is the newer position.
    auto again = s.open("/v1/events?since=0", {{"Last-Event-ID", seq_of(first)}});
    const auto one = again->event();
    const auto two = again->event();
    REQUIRE(one);
    REQUIRE(two);
    const auto live = s.bus->publish_event(catalog::HISTORY_SAVED,
                                           {{"job_id", "j"}, {"record_id", "1"}, {"kind", "x"}});
    const auto after = again->event();
    REQUIRE(after);
    CHECK(one->at("id") == seq_of(missed[0]));
    CHECK(two->at("id") == seq_of(missed[1]));
    CHECK(payload(*one) == missed[0].to_dict());
    CHECK(payload(*two) == missed[1].to_dict());
    CHECK(after->at("id") == seq_of(live));
}

TEST_CASE("test_since_replays_buffered_events_of_the_wanted_types",
          "[platform][test_automation_api_sse]") {
    LiveServer s;
    const auto started = s.bus->publish_event(catalog::RECORDING_STARTED, {{"job_id", "j"}});
    s.bus->publish_event(catalog::RECORDING_STOPPED, {{"job_id", "j"}, {"duration_s", 1.0}});
    const auto saved = s.bus->publish_event(catalog::HISTORY_SAVED,
                                            {{"job_id", "j"}, {"record_id", "1"}, {"kind", "x"}});
    auto reader = s.open("/v1/events?since=0&types=recording.started,history.saved");
    const auto one = reader->event();
    const auto two = reader->event();
    REQUIRE(one);
    REQUIRE(two);
    CHECK(one->at("id") == seq_of(started));
    CHECK(two->at("id") == seq_of(saved));
}

TEST_CASE("test_a_position_the_buffer_no_longer_holds_starts_with_a_gap",
          "[platform][test_automation_api_sse]") {
    LiveServer s(3);
    std::vector<fv::automation::Event> published;
    for (int n = 0; n < 6; ++n) {
        published.push_back(
            s.bus->publish_event(catalog::RECORDING_STARTED, {{"job_id", std::to_string(n)}}));
    }
    auto reader = s.open("/v1/events?since=1");
    const auto gap = reader->event();
    REQUIRE(gap);
    std::vector<std::string> replayed;
    for (int i = 0; i < 3; ++i) {
        replayed.push_back(reader->event()->at("id"));
    }
    CHECK_FALSE(gap->contains("id")); // synthetic: a reconnect keeps its real position
    CHECK(gap->at("event") == "events.gap");
    CHECK(payload(*gap).at("seq") == 0);
    CHECK(payload(*gap).at("data") ==
          Json{{"missed_from_seq", 2}, {"reason", std::string(catalog::GAP_EVICTED)}});
    CHECK(replayed == std::vector<std::string>{seq_of(published[3]), seq_of(published[4]),
                                               seq_of(published[5])});
}

TEST_CASE("test_a_position_from_an_earlier_app_session_replays_this_one_after_a_gap",
          "[platform][test_automation_api_sse]") {
    LiveServer s;
    const std::vector<fv::automation::Event> published = {
        s.bus->publish_event(catalog::RECORDING_STARTED, {{"job_id", "0"}}),
        s.bus->publish_event(catalog::RECORDING_STARTED, {{"job_id", "1"}})};
    auto reader = s.open("/v1/events", {{"Last-Event-ID", "900"}});
    const auto gap = reader->event();
    REQUIRE(gap);
    const auto one = reader->event();
    const auto two = reader->event();
    REQUIRE(one);
    REQUIRE(two);
    CHECK(payload(*gap).at("data") ==
          Json{{"missed_from_seq", 1}, {"reason", std::string(catalog::GAP_RESTARTED)}});
    CHECK(one->at("id") == seq_of(published[0]));
    CHECK(two->at("id") == seq_of(published[1]));
}

TEST_CASE("test_channel_clients_are_counted_apart_and_released_on_disconnect",
          "[platform][test_automation_api_sse]") {
    LiveServer s;
    auto channel = s.open(
        "/v1/events", {{"X-" + std::string(fv::identity().product) + "-Client", "claude-channel"}});
    REQUIRE(channel->message());
    {
        auto plain = s.open();
        REQUIRE(plain->message());
        CHECK(s.listeners() == Json{{"events", 1}, {"claude_channel", 1}});
    }
    CHECK(wait_until([&] { return s.bus->subscribers(SSE_LABEL) == 0; }));
    CHECK(s.listeners() == Json{{"events", 0}, {"claude_channel", 1}});
    channel.reset();
    CHECK(wait_until([&] { return s.bus->subscribers() == 0; }));

    // OpenWhisper's header still marks a channel.
    auto legacy = s.open(
        "/v1/events",
        {{"X-" + std::string(fv::legacy_identity().product) + "-Client", " claude-channel "}});
    REQUIRE(legacy->message());
    CHECK(s.listeners() == Json{{"events", 0}, {"claude_channel", 1}});
}

TEST_CASE("test_streams_end_when_mcp_is_switched_off", "[platform][test_automation_api_sse]") {
    LiveServer s;
    auto reader = s.open();
    REQUIRE(reader->message());
    const auto started = std::chrono::steady_clock::now();
    s.on = false;
    CHECK(reader->ended(5s));
    const auto elapsed = std::chrono::steady_clock::now() - started;
    CHECK(reader->rest().empty());
    CHECK(elapsed < StreamOptions{}.enabled_check + 1s);
    CHECK(wait_until([&] { return s.bus->subscribers() == 0; }));
}

TEST_CASE("test_streams_end_when_live_events_are_disallowed",
          "[platform][test_automation_api_sse]") {
    LiveServer s;
    StreamOptions options;
    options.permission_check = 100ms;
    s.live.set_stream_options(options);
    auto reader = s.open();
    REQUIRE(reader->message());
    const auto started = std::chrono::steady_clock::now();
    s.settings.set_contents(Json{{std::string(permissions::LIVE_EVENTS), false}});
    CHECK(reader->ended(5s));
    CHECK(std::chrono::steady_clock::now() - started < 2s);
    CHECK(reader->rest().empty());
    CHECK(wait_until([&] { return s.bus->subscribers() == 0; }));
}

TEST_CASE("test_an_idle_stream_pings", "[platform][test_automation_api_sse]") {
    LiveServer s;
    StreamOptions options;
    options.ping_interval = 200ms;
    s.live.set_stream_options(options);
    auto reader = s.open();
    CHECK(reader->message() == Message{{"retry", "2000"}});
    CHECK(reader->message() == Message{{"comment", "ping"}});
}

TEST_CASE("test_stopping_the_listener_ends_open_streams_promptly",
          "[platform][test_automation_api_sse]") {
    fv::testkit::TempDataRoot root;
    const auto database = copy_history_fixture(root.path(), "live.db");
    fv::net::Reactor reactor{2};
    fv::ThreadPool pool{"live-sse-runtime", 4};
    fv::Strand io{pool, "live-sse-runtime-io", 4, fv::ShutdownMode::CancelQueued};
    fv::MemorySettingsStore settings;
    fv::CredentialStore credentials{std::make_shared<fv::MemoryCredentialBackend>(),
                                    fv::identity()};
    fv::automation::EventBus bus;
    fv::automation::FilterLeases leases;
    auto control = std::make_shared<RouteControl>();
    ScopedAppControl registration{control};
    LiveApi live(settings, nullptr, bus, leases, io);
    McpRuntimeDeps deps;
    deps.database = database;
    deps.reactor = &reactor;
    deps.settings = &settings;
    deps.credentials = &credentials;
    deps.io = &io;
    McpRuntime runtime(deps);
    mount_live_api(runtime, live);
    auto finder = fv::net::HttpServer::create(reactor);
    const auto endpoint = finder->listen({"127.0.0.1", 0, std::nullopt, true});
    REQUIRE(endpoint);
    const auto port = endpoint->port;
    finder->stop(0ms);
    runtime.start(port);
    REQUIRE(wait_until([&] { return runtime.status().state == "running"; }, 10s));
    SseReader reader(port, "/v1/events", runtime.token());
    REQUIRE(reader.message());
    const auto started = std::chrono::steady_clock::now();
    runtime.stop(true);
    const auto stopped = std::chrono::steady_clock::now() - started;
    CHECK(reader.ended(5s));
    CHECK(reader.rest().empty());
    // The runtime switches enabled() off before the server shuts down, so the stream ends on its
    // own instead of waiting out the graceful timeout.
    CHECK(stopped < 2s);
    CHECK(runtime.status().state == "stopped");
    CHECK(wait_until([&] { return bus.subscribers() == 0; }));
    bus.close();
}

TEST_CASE("test_a_server_stopped_without_disabling_still_releases_its_streams",
          "[platform][test_automation_api_sse]") {
    LiveServer s;
    auto reader = s.open();
    REQUIRE(reader->message());
    CHECK(s.bus->subscribers() == 1);
    // The server gives in-flight streams no signal: they end when the grace runs out.
    s.stop(1000ms);
    CHECK(wait_until([&] { return s.bus->subscribers() == 0; }));
    CHECK(reader->ended(5s));
    CHECK(reader->rest().empty());
}

TEST_CASE("test_a_reader_too_slow_for_the_queue_gets_a_gap_and_the_end",
          "[test_automation_api_sse]") {
    fv::automation::EventBus bus;
    fv::ManualExecutor timers;
    StreamOptions options;
    options.queue_size = 5;
    auto stream =
        std::make_shared<EventStream>(std::nullopt, bus.last_seq(), std::string(SSE_LABEL), nullptr,
                                      nullptr, bus, timers, fv::system_clock(), options);
    auto out = std::make_shared<FakeSse>();
    stream->start(out);
    REQUIRE(out->frames() == std::vector<std::string>{"retry: 2000\n\n"});
    const auto first = bus.publish_event(catalog::RECORDING_STARTED, {{"job_id", "first"}});
    REQUIRE(out->wait_for([](const auto& frames) { return frames.size() == 2; }));
    CHECK(out->frames()[1] == sse_frame(first));
    // The client now stops reading while more arrive than the queue holds.
    out->set_queued(SOCKET_FRAMES);
    std::vector<fv::automation::Event> later;
    for (int n = 0; n < 7; ++n) {
        later.push_back(
            bus.publish_event(catalog::RECORDING_STARTED, {{"job_id", std::to_string(n)}}));
    }
    REQUIRE(wait_until([&] { return stream->overflow_seq() != 0; }));
    REQUIRE(wait_until([&] { return out->closed(); }));
    const auto frames = out->frames();
    REQUIRE(frames.size() == 3);
    const std::string& gap = frames[2];
    CHECK(gap.starts_with("event: events.gap\n"));
    CHECK(fv::py::loads(gap.substr(gap.find("data: ") + 6)).at("data") ==
          Json{{"missed_from_seq", later[0].seq}, {"reason", std::string(catalog::GAP_SLOW)}});
    CHECK(wait_until([&] { return bus.subscribers() == 0; }));
    bus.close();
}

TEST_CASE("an idle stream pings every 15 seconds and rereads its permission every 2",
          "[test_automation_api_sse]") {
    fv::automation::EventBus bus;
    fv::FakeClock clock;
    fv::ManualExecutor timers{clock};
    std::atomic<int> permission_checks{0};
    bool allowed = true;
    auto stream = std::make_shared<EventStream>(
        std::nullopt, 0, std::string(SSE_LABEL), [] { return true; },
        [&] {
            ++permission_checks;
            return allowed;
        },
        bus, timers, clock);
    auto out = std::make_shared<FakeSse>();
    stream->start(out);
    timers.advance(14'900ms);
    CHECK(out->frames() == std::vector<std::string>{"retry: 2000\n\n"});
    CHECK(permission_checks == 7);
    timers.advance(100ms);
    CHECK(out->frames() == std::vector<std::string>{"retry: 2000\n\n", ": ping\n\n"});
    timers.advance(15s);
    CHECK(out->frames().size() == 3);
    allowed = false;
    timers.advance(2s);
    CHECK(stream->ended());
    CHECK(out->closed());
    CHECK(out->frames().size() == 3);
    CHECK(bus.subscribers() == 0);
    bus.close();
}

TEST_CASE("a stream whose client left is noticed without a ping", "[test_automation_api_sse]") {
    fv::automation::EventBus bus;
    fv::FakeClock clock;
    fv::ManualExecutor timers{clock};
    auto stream = std::make_shared<EventStream>(std::nullopt, 0, std::string(SSE_LABEL), nullptr,
                                                nullptr, bus, timers, clock);
    auto out = std::make_shared<FakeSse>();
    stream->start(out);
    CHECK(bus.subscribers(SSE_LABEL) == 1);
    out->set_gone(true);
    timers.advance(StreamOptions{}.enabled_check);
    CHECK(stream->ended());
    CHECK(bus.subscribers() == 0);
    bus.close();
}

TEST_CASE("no live event goes out once the live API is off, before the next check too",
          "[test_automation_api_sse]") {
    // api.py's loop asks enabled() before each event it sends, not only every half second.
    fv::automation::EventBus bus;
    fv::FakeClock clock;
    fv::ManualExecutor timers{clock}; // never advanced: no check runs
    std::atomic<bool> on{true};
    auto stream = std::make_shared<EventStream>(
        std::nullopt, 0, std::string(SSE_LABEL), [&] { return on.load(); }, nullptr, bus, timers,
        clock);
    auto out = std::make_shared<FakeSse>();
    stream->start(out);
    const auto first = bus.publish_event(catalog::RECORDING_STARTED, {{"job_id", "a"}});
    REQUIRE(out->wait_for([](const auto& frames) { return frames.size() == 2; }));
    CHECK(out->frames()[1] == sse_frame(first));
    on = false;
    bus.publish_event(catalog::RECORDING_STOPPED, {{"job_id", "a"}});
    REQUIRE(wait_until([&] { return out->closed(); }));
    CHECK(stream->ended());
    CHECK(out->frames().size() == 2);
    CHECK(wait_until([&] { return bus.subscribers() == 0; }));
    bus.close();
}

TEST_CASE("events held for a paused socket are not sent once the live API is off",
          "[test_automation_api_sse]") {
    fv::automation::EventBus bus;
    fv::FakeClock clock;
    fv::ManualExecutor timers{clock};
    std::atomic<bool> on{true};
    auto stream = std::make_shared<EventStream>(
        std::nullopt, 0, std::string(SSE_LABEL), [&] { return on.load(); }, nullptr, bus, timers,
        clock);
    auto out = std::make_shared<FakeSse>();
    stream->start(out);
    out->set_queued(SOCKET_FRAMES); // the client stops reading: events wait in the stream
    bus.publish_event(catalog::RECORDING_STARTED, {{"job_id", "a"}});
    // The event waits for the socket: a drain timer beside the stream's tick.
    REQUIRE(wait_until([&] { return timers.pending_timers() == 2; }));
    out->set_queued(0);
    on = false;
    timers.advance(StreamOptions{}.drain_poll);
    CHECK(stream->ended());
    CHECK(out->closed());
    CHECK(out->frames() == std::vector<std::string>{"retry: 2000\n\n"});
    CHECK(bus.subscribers() == 0);
    bus.close();
}

// --- tests/test_automation_fix_api_sse_order.py ------------------------------------------------

namespace {

// A subscriber that holds the dispatch thread on the first event until released.
struct StalledDispatch {
    explicit StalledDispatch(fv::automation::EventBus& bus) {
        subscription = bus.subscribe(
            [this](const fv::automation::Event&) {
                std::unique_lock lock(mutex);
                if (!holding) {
                    holding = true;
                    changed.notify_all();
                    changed.wait_for(lock, 5s, [&] { return released; });
                }
            },
            std::nullopt, "slow");
    }
    bool wait_holding() {
        std::unique_lock lock(mutex);
        return changed.wait_for(lock, 5s, [&] { return holding; });
    }
    void release() {
        std::lock_guard lock(mutex);
        released = true;
        changed.notify_all();
    }

    std::mutex mutex;
    std::condition_variable changed;
    bool holding = false;
    bool released = false;
    std::shared_ptr<fv::automation::EventSubscription> subscription;
};

struct Sent {
    std::optional<std::string> id;
    std::string type;
    Json data;
};

// Opens a stream at `replay_from`, runs the two steps, and collects for `wait`: the (id, type,
// data) of each message it sent after "retry".
std::vector<Sent> run_stream(fv::automation::EventBus& bus, std::int64_t replay_from,
                             const std::function<void()>& after_subscribe,
                             const std::function<void()>& more = {},
                             std::chrono::milliseconds wait = 600ms) {
    fv::ManualExecutor timers;
    auto stream = std::make_shared<EventStream>(std::nullopt, replay_from, std::string(SSE_LABEL),
                                                nullptr, nullptr, bus, timers, fv::system_clock());
    auto out = std::make_shared<FakeSse>();
    stream->start(out);
    after_subscribe();
    if (more) {
        more();
    }
    std::this_thread::sleep_for(wait);
    stream->close();
    std::vector<Sent> sent;
    const auto frames = out->frames();
    for (std::size_t i = 1; i < frames.size(); ++i) {
        Sent message;
        std::size_t start = 0;
        const std::string& frame = frames[i];
        while (start < frame.size()) {
            const std::size_t end = frame.find('\n', start);
            const std::string line = frame.substr(start, end - start);
            start = end + 1;
            if (line.starts_with("id: ")) {
                message.id = line.substr(4);
            } else if (line.starts_with("event: ")) {
                message.type = line.substr(7);
            } else if (line.starts_with("data: ")) {
                message.data = fv::py::loads(line.substr(6));
            }
        }
        if (!message.type.empty()) {
            sent.push_back(std::move(message));
        }
    }
    return sent;
}

std::vector<std::string> ids_of(const std::vector<Sent>& sent) {
    std::vector<std::string> out;
    for (const Sent& message : sent) {
        out.push_back(message.id.value_or("-"));
    }
    return out;
}

} // namespace

TEST_CASE("test_events_the_client_already_has_are_not_sent_again",
          "[test_automation_fix_api_sse_order]") {
    fv::automation::EventBus bus;
    StalledDispatch stalled(bus);
    std::vector<fv::automation::Event> published;
    for (int n = 0; n < 3; ++n) {
        published.push_back(
            bus.publish_event(catalog::RECORDING_STARTED, {{"job_id", std::to_string(n)}}));
    }
    REQUIRE(stalled.wait_holding()); // the dispatch thread is stuck on the first; the rest wait
    std::vector<fv::automation::Event> newer;
    const auto sent = run_stream(
        bus, published.back().seq, [&] { stalled.release(); },
        [&] { newer.push_back(bus.publish_event(catalog::RECORDING_STOPPED, {{"job_id", "x"}})); });
    CHECK(ids_of(sent) == std::vector<std::string>{seq_of(newer[0])});
    bus.close();
}

TEST_CASE("test_a_stream_without_a_position_starts_after_what_was_already_published",
          "[test_automation_fix_api_sse_order]") {
    fv::automation::EventBus bus;
    StalledDispatch stalled(bus);
    for (int n = 0; n < 3; ++n) {
        bus.publish_event(catalog::RECORDING_STARTED, {{"job_id", std::to_string(n)}});
    }
    REQUIRE(stalled.wait_holding());
    std::vector<fv::automation::Event> newer;
    // What GET /v1/events without Last-Event-ID or since does: start at last_seq.
    const auto sent = run_stream(
        bus, bus.last_seq(), [&] { stalled.release(); },
        [&] { newer.push_back(bus.publish_event(catalog::HISTORY_SAVED, {{"job_id", "x"}})); });
    CHECK(ids_of(sent) == std::vector<std::string>{seq_of(newer[0])});
    bus.close();
}

TEST_CASE("test_a_preview_older_than_the_replay_is_not_sent_after_it",
          "[test_automation_fix_api_sse_order]") {
    fv::automation::EventBus bus;
    StalledDispatch stalled(bus);
    const auto started = bus.publish_event(catalog::RECORDING_STARTED, {{"job_id", "p"}});
    REQUIRE(stalled.wait_holding());
    // Live preview text is never kept for replay.
    bus.publish_event(catalog::TRANSCRIPTION_PREVIEW,
                      {{"job_id", "p"}, {"text", "hel"}, {"final", false}}, false);
    const auto stopped =
        bus.publish_event(catalog::RECORDING_STOPPED, {{"job_id", "p"}, {"duration_s", 1.0}});
    std::vector<fv::automation::Event> newer;
    const auto sent = run_stream(
        bus, 0, [&] { stalled.release(); },
        [&] { newer.push_back(bus.publish_event(catalog::HISTORY_SAVED, {{"job_id", "p"}})); });
    CHECK(ids_of(sent) ==
          std::vector<std::string>{seq_of(started), seq_of(stopped), seq_of(newer[0])});
    bus.close();
}

TEST_CASE("test_live_events_after_the_replay_still_arrive_in_order",
          "[test_automation_fix_api_sse_order]") {
    fv::automation::EventBus bus;
    const auto first = bus.publish_event(catalog::RECORDING_STARTED, {{"job_id", "a"}});
    std::vector<fv::automation::Event> newer;
    const auto sent = run_stream(
        bus, 0, [] {},
        [&] {
            for (int n = 0; n < 3; ++n) {
                newer.push_back(bus.publish_event(catalog::RECORDING_CANCELED,
                                                  {{"job_id", std::to_string(n)}}));
            }
        });
    std::vector<std::string> expected{seq_of(first)};
    for (const auto& event : newer) {
        expected.push_back(seq_of(event));
    }
    CHECK(ids_of(sent) == expected);
    bus.close();
}

TEST_CASE("test_a_position_from_a_previous_session_gets_a_gap_then_everything",
          "[test_automation_fix_api_sse_order]") {
    // A new app session: the bus numbers past every old position.
    fv::automation::EventBus restarted(fv::automation::DEFAULT_BUFFER_SIZE, 100);
    std::vector<fv::automation::Event> new_session;
    for (int n = 0; n < 2; ++n) {
        new_session.push_back(restarted.publish_event(catalog::RECORDING_STARTED,
                                                      {{"job_id", "new" + std::to_string(n)}}));
    }
    const auto sent = run_stream(restarted, 3, [] {});
    REQUIRE(sent.size() == 3);
    CHECK_FALSE(sent[0].id);
    CHECK(sent[0].type == catalog::EVENTS_GAP);
    CHECK(sent[0].data.at("data") ==
          Json{{"missed_from_seq", 4}, {"reason", std::string(catalog::GAP_RESTARTED)}});
    CHECK(sent[1].id == seq_of(new_session[0]));
    CHECK(sent[2].id == seq_of(new_session[1]));
    restarted.close();
}

TEST_CASE("test_eighteen_digit_positions_are_accepted", "[test_automation_fix_api_sse_order]") {
    CHECK(parse_last_event_id(std::string(18, '9')) == 999'999'999'999'999'999LL);
    CHECK_THROWS_AS(parse_last_event_id(std::string(19, '1')), Invalid);
}

TEST_CASE("test_a_stream_from_a_large_position_still_delivers_live_events",
          "[test_automation_fix_api_sse_order]") {
    // A position past this session's numbering is from another session.
    fv::automation::EventBus bus;
    bus.publish_event(catalog::RECORDING_STARTED, {{"job_id", "a"}});
    std::vector<fv::automation::Event> newer;
    const auto sent = run_stream(
        bus, 100'000'000'000'000'000LL, [] {},
        [&] { newer.push_back(bus.publish_event(catalog::RECORDING_STOPPED, {{"job_id", "a"}})); });
    REQUIRE_FALSE(sent.empty());
    CHECK(sent.back().id == seq_of(newer[0]));
    bus.close();
}

TEST_CASE("test_streams_release_their_subscription", "[test_automation_fix_api_sse_order]") {
    fv::automation::EventBus bus;
    run_stream(bus, 0, [] {}, {}, 100ms);
    CHECK(wait_until([&] { return bus.subscribers(SSE_LABEL) == 0; }, 2s));
    bus.close();
}
