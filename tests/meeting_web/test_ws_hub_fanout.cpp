// The hub's fan-out limits (ws.py broadcast_json, _flush_pending, _send_guarded): the pre-hello
// buffer of 512 broadcasts and its overflow, and the 5 s send budget with its drop after three
// consecutive misses, over a scripted socket that completes its timed sends on the sending
// thread.

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "hub_harness.h"
#include "meeting_web/ws_hub.h"
#include "meeting_web/ws_protocol.h"
#include "net/types.h"

using fv::Json;
using namespace fv::meeting_web::test_support;
namespace mw = fv::meeting_web;
using namespace std::chrono_literals;

namespace {

// A dashboard socket whose timed sends complete inside send_text, as WsConnection's default
// does and as a stopped reactor does, either written or timed out as the test says.
class ScriptedSocket final : public fv::net::WsConnection {
public:
    using fv::net::WsConnection::send_text;

    void send_text(std::shared_ptr<const std::string> text) override {
        std::lock_guard lock(mutex_);
        sent_.push_back(*text);
        changed_.notify_all();
    }
    void send_text(std::shared_ptr<const std::string> text, std::chrono::milliseconds timeout,
                   std::function<void(bool timed_out)> done) override {
        bool time_out = false;
        {
            std::lock_guard lock(mutex_);
            timeouts_.push_back(timeout);
            time_out = time_out_;
            if (!time_out) {
                sent_.push_back(*text);
            }
            changed_.notify_all();
        }
        if (done) {
            done(time_out);
        }
    }
    void send_binary(std::vector<std::byte>) override {}
    void close(std::uint16_t code, std::string) override {
        std::lock_guard lock(mutex_);
        closes_.push_back(code);
        open_ = false;
        changed_.notify_all();
    }
    std::size_t queued_bytes() const override { return 0; }
    bool is_open() const override {
        std::lock_guard lock(mutex_);
        return open_;
    }

    void time_out_sends(bool time_out) {
        std::lock_guard lock(mutex_);
        time_out_ = time_out;
    }
    bool wait_for_frames(std::size_t count) {
        std::unique_lock lock(mutex_);
        return changed_.wait_for(lock, 5s, [&] { return sent_.size() >= count; });
    }
    std::vector<std::string> sent() const {
        std::lock_guard lock(mutex_);
        return sent_;
    }
    std::vector<std::uint16_t> closes() const {
        std::lock_guard lock(mutex_);
        return closes_;
    }
    std::vector<std::chrono::milliseconds> timeouts() const {
        std::lock_guard lock(mutex_);
        return timeouts_;
    }

private:
    mutable std::mutex mutex_;
    std::condition_variable changed_;
    std::vector<std::string> sent_;
    std::vector<std::uint16_t> closes_;
    std::vector<std::chrono::milliseconds> timeouts_;
    bool time_out_ = false;
    bool open_ = true;
};

// A hub with no server around it: the test plays the reactor's part.
struct BareHub {
    BareHub() : store(make_store(clock, ids)), engine("m_ws", store) {
        hub = std::make_unique<mw::WsHub>(reactor, engine, repo);
        hub->on_startup();
        handlers = hub->handlers();
    }
    ~BareHub() { hub.reset(); }
    BareHub(const BareHub&) = delete;
    BareHub& operator=(const BareHub&) = delete;

    static std::shared_ptr<fv::meeting_state::MeetingStateStore>
    make_store(fv::FakeClock& clock, fv::meeting_state::testing::FixedIds& ids) {
        fv::meeting_state::MeetingState state;
        state.meeting_id = "m_ws";
        auto store = std::make_shared<fv::meeting_state::MeetingStateStore>(
            std::move(state), nullptr, fv::meeting_state::SegmentHandler{}, clock, ids);
        store->apply("system", std::nullopt,
                     Json::array({Json{{"op", "upsert_participant"},
                                       {"display_name", "Me"},
                                       {"kind", "me"},
                                       {"is_provisional", false}}}));
        return store;
    }

    // Opens a host socket and waits for its hello.
    std::shared_ptr<ScriptedSocket> open_host() {
        auto socket = std::make_shared<ScriptedSocket>();
        fv::net::HttpRequest request{};
        request.path = "/ws";
        request.query = {{"token", std::string(kHostToken)}};
        handlers.on_open(socket, request);
        REQUIRE(socket->wait_for_frames(1));
        CHECK(fv::py::loads(socket->sent().front())["type"] == "hello");
        return socket;
    }

    fv::FakeClock clock;
    fv::meeting_state::testing::FixedIds ids;
    std::shared_ptr<HelloRepository> repo =
        std::make_shared<HelloRepository>(ws_meeting_row(), ws_segments());
    std::shared_ptr<fv::meeting_state::MeetingStateStore> store;
    mw::testing::FakeDashboardEngine engine;
    fv::net::Reactor reactor;
    std::unique_ptr<mw::WsHub> hub;
    fv::net::WsHandlers handlers;
};

Json status_message(int n) {
    return Json{{"type", "status"}, {"n", n}};
}

} // namespace

TEST_CASE("a stalled socket is dropped after three consecutive send timeouts", "[ws_hub]") {
    ScopedLogCapture logs;
    BareHub app;
    auto socket = app.open_host();
    REQUIRE(app.hub->connection_count() == 1);

    socket->time_out_sends(true);
    app.hub->schedule_broadcast(status_message(1));
    app.hub->schedule_broadcast(status_message(2));
    CHECK(socket->closes().empty());
    CHECK(app.hub->connection_count() == 1);
    app.hub->schedule_broadcast(status_message(3));
    CHECK(socket->closes() == std::vector<std::uint16_t>{mw::kWsCloseServerError});
    CHECK(app.hub->connection_count() == 0);
    // Every broadcast send had the 5 s budget.
    for (const auto timeout : socket->timeouts()) {
        CHECK(timeout == mw::kBroadcastSendTimeout);
    }
    // A dropped socket receives nothing more.
    app.hub->schedule_broadcast(status_message(4));
    CHECK(socket->timeouts().size() == 3);

    const auto lines = logs.capture->lines();
    CHECK(std::ranges::count_if(lines, [](const std::string& line) {
              return line.find("Dashboard socket stalled") != std::string::npos;
          }) == 3);
    CHECK(std::ranges::any_of(lines, [](const std::string& line) {
        return line.find("Dropping dashboard socket: send timed out repeatedly") !=
               std::string::npos;
    }));
    app.handlers.on_close(socket, 1011, {});
}

TEST_CASE("a written broadcast resets the timeout count", "[ws_hub]") {
    BareHub app;
    auto socket = app.open_host();
    socket->time_out_sends(true);
    app.hub->schedule_broadcast(status_message(1));
    app.hub->schedule_broadcast(status_message(2));
    socket->time_out_sends(false);
    app.hub->schedule_broadcast(status_message(3));
    socket->time_out_sends(true);
    app.hub->schedule_broadcast(status_message(4));
    app.hub->schedule_broadcast(status_message(5));
    CHECK(socket->closes().empty());
    CHECK(app.hub->connection_count() == 1);
    app.hub->schedule_broadcast(status_message(6));
    CHECK(socket->closes() == std::vector<std::uint16_t>{mw::kWsCloseServerError});
    app.handlers.on_close(socket, 1011, {});
}

TEST_CASE("a send that fails on a closed socket drops it", "[ws_hub]") {
    BareHub app;
    auto socket = app.open_host();
    // The socket went away without its close reaching the hub yet: the send completes
    // unwritten (false) on a socket that is no longer open.
    socket->close(1006, {});
    app.hub->schedule_broadcast(status_message(1));
    CHECK(socket->closes() == std::vector<std::uint16_t>{1006, mw::kWsCloseServerError});
    CHECK(app.hub->connection_count() == 0);
    app.handlers.on_close(socket, 1006, {});
}

TEST_CASE("all 512 broadcasts buffered during a hello follow it in order", "[ws_hub]") {
    Harness app(EngineKind::Canned);
    app.repo->on_get_last_segments([&] {
        for (std::size_t i = 0; i < mw::kMaxPendingMessages; ++i) {
            app.server->broadcast(status_message(static_cast<int>(i)));
        }
    });
    auto ws = app.host();
    CHECK(ws->receive()["type"] == "hello");
    for (std::size_t i = 0; i < mw::kMaxPendingMessages; ++i) {
        const Json message = ws->receive();
        REQUIRE(message["type"] == "status");
        REQUIRE(message["n"] == static_cast<int>(i));
    }
    ws->send({{"type", "ping"}});
    CHECK(ws->receive()["type"] == "pong");
}

TEST_CASE("a pre-hello buffer that overflows closes the socket with 4409", "[ws_hub]") {
    ScopedLogCapture logs;
    Harness app(EngineKind::Canned);
    app.repo->on_get_last_segments([&] {
        for (std::size_t i = 0; i <= mw::kMaxPendingMessages; ++i) {
            app.server->broadcast(status_message(static_cast<int>(i)));
        }
    });
    auto ws = app.host();
    // The hello still goes out, then the client is asked to resync; nothing buffered follows.
    CHECK(ws->receive()["type"] == "hello");
    CHECK_FALSE(ws->try_receive(200ms).has_value());
    CHECK(ws->closed_with() == mw::kWsCloseResync);
    CHECK(app.server->hub().connection_count() == 0);
    const auto lines = logs.capture->lines();
    CHECK(std::ranges::any_of(lines, [](const std::string& line) {
        return line.find("Pre-hello buffer overflowed; asking client to resync") !=
               std::string::npos;
    }));
    // A fresh connection is served normally.
    auto again = app.host();
    CHECK(again->receive()["type"] == "hello");
}
