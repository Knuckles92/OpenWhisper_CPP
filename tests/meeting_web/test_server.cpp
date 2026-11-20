// meeting_web/server.h: tests/test_meeting_web_server_stop.py, the display-host test of
// tests/test_meeting_web_server_address.py, and the server's links.

#include <catch2/catch_test_macros.hpp>

#include <boost/asio/connect.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/read.hpp>
#include <boost/asio/write.hpp>

#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>

#include "hub_harness.h"
#include "meeting_web/server.h"
#include "meeting_web/testing/fake_dashboard_engine.h"
#include "net/lan_address.h"
#include "net/reactor.h"
#include "web_test_support.h"

using fv::Json;
using namespace fv::meeting_web::test_support;
namespace mw = fv::meeting_web;
using namespace std::chrono_literals;

namespace {

std::uint16_t port_of(const std::string& url) {
    return static_cast<std::uint16_t>(std::stoi(url.substr(url.rfind(':') + 1)));
}

// The thread writing the endless response; set on the reactor, joined by the test.
struct StreamWriter {
    std::mutex mutex;
    std::thread thread;

    // Declared before the server, so the stream has closed by then and the writer has stopped.
    ~StreamWriter() { join(); }

    void join() {
        std::thread finished;
        {
            std::lock_guard lock(mutex);
            finished = std::move(thread);
        }
        if (finished.joinable()) {
            finished.join();
        }
    }
};

// An endless streaming response, as the recording player keeps one open: a chunk every 0.2 s
// until the client goes.
void install_endless_stream(fv::net::HttpServer& server, std::atomic<bool>& stop_writing,
                            StreamWriter& writer) {
    server.route("GET", "/test-endless-stream",
                 [&stop_writing, &writer](const fv::net::HttpRequest&,
                                          std::shared_ptr<fv::net::Responder> responder) {
                     auto stream =
                         responder->start_sse({{"content-type", "application/octet-stream"}});
                     std::lock_guard lock(writer.mutex);
                     writer.thread = std::thread([stream, &stop_writing] {
                         while (!stop_writing && stream->write(std::string(1024, 'x'))) {
                             std::this_thread::sleep_for(200ms);
                         }
                     });
                 });
}

} // namespace

// from tests/test_meeting_web_server_stop.py::test_stop_does_not_wait_for_an_open_response
TEST_CASE("stop does not wait for an open response", "[test_meeting_web_server_stop]") {
    auto repo = std::make_shared<HelloRepository>(Json::object(), std::vector<Json>{});
    mw::testing::FakeDashboardEngine engine; // no meeting, no store
    fv::net::Reactor reactor;
    std::atomic<bool> stop_writing{false};
    StreamWriter writer;
    mw::MeetingWebServer server(reactor, engine, repo, "localhost", 0,
                                [&](fv::net::HttpServer& http, mw::MeetingWebServer&) {
                                    install_endless_stream(http, stop_writing, writer);
                                });
    const auto url = server.start();
    REQUIRE(url.has_value());

    boost::asio::io_context io;
    boost::asio::ip::tcp::socket client(io);
    client.connect({boost::asio::ip::make_address("127.0.0.1"), port_of(*url)});
    const std::string request = "GET /test-endless-stream HTTP/1.1\r\nHost: 127.0.0.1\r\n\r\n";
    boost::asio::write(client, boost::asio::buffer(request));
    char buffer[4096];
    // The response is streaming.
    CHECK(client.read_some(boost::asio::buffer(buffer)) > 0);

    const auto started = std::chrono::steady_clock::now();
    server.stop();
    const auto took = std::chrono::steady_clock::now() - started;

    stop_writing = true;
    writer.join();
    boost::system::error_code ignored;
    client.close(ignored);
    server.stop();
    CHECK_FALSE(server.is_running());
    CHECK(took < 5s);
}

// from
// tests/test_meeting_web_server_address.py::test_lan_display_host_falls_back_only_when_no_interface_is_usable
TEST_CASE("lan display host falls back only when no interface is usable",
          "[test_meeting_web_server_address]") {
    CHECK(mw::display_host("lan", [] { return std::optional<std::string>(); }) == "127.0.0.1");
    CHECK(mw::display_host("lan", [] { return std::optional<std::string>("192.168.1.44"); }) ==
          "192.168.1.44");
    CHECK(mw::display_host("localhost", [] { return std::optional<std::string>("10.0.0.8"); }) ==
          "127.0.0.1");
}

TEST_CASE("the links come from the current meeting row", "[server]") {
    Harness app(EngineKind::Store);
    const std::string base = app.server->base_url();
    CHECK(base == "http://127.0.0.1:" + std::to_string(app.port));
    CHECK(app.server->host_url() == base + "/m/" + std::string(kHostToken));
    CHECK(app.server->guest_url() == base + "/m/" + std::string(kGuestToken));
    // Read fresh on every call.
    Json rotated = ws_meeting_row();
    rotated["host_token"] = "new-host";
    rotated["guest_token"] = nullptr;
    app.repo->set_meeting(rotated);
    CHECK(app.server->host_url() == base + "/m/new-host");
    CHECK(app.server->guest_url().empty());
    app.repo->fail_get_meeting(true);
    CHECK(app.server->host_url().empty());
    app.repo->fail_get_meeting(false);
    app.engine->set_meeting_id("");
    CHECK(app.server->host_url().empty());
    // A second start returns the same URL; after stop every link is empty.
    app.engine->set_meeting_id("m_ws");
    CHECK(app.server->start() == base);
    app.server->stop();
    CHECK(app.server->base_url().empty());
    CHECK(app.server->host_url().empty());
    CHECK_FALSE(app.server->is_running());
}

TEST_CASE("an unknown bind serves on loopback", "[server]") {
    auto repo = std::make_shared<HelloRepository>(Json::object(), std::vector<Json>{});
    mw::testing::FakeDashboardEngine engine;
    fv::net::Reactor reactor;
    mw::MeetingWebServer server(reactor, engine, repo, "everywhere", 0);
    CHECK(server.bind() == "localhost");
    const auto url = server.start();
    REQUIRE(url.has_value());
    CHECK(url->starts_with("http://127.0.0.1:"));
    server.stop();
}

TEST_CASE("a port in use fails the start", "[server]") {
    auto repo = std::make_shared<HelloRepository>(Json::object(), std::vector<Json>{});
    mw::testing::FakeDashboardEngine engine;
    fv::net::Reactor reactor;
    mw::MeetingWebServer first(reactor, engine, repo, "localhost", 0);
    const auto url = first.start();
    REQUIRE(url.has_value());
    mw::MeetingWebServer second(reactor, engine, repo, "localhost", port_of(*url));
    const auto failed = second.start();
    REQUIRE_FALSE(failed.has_value());
    CHECK(failed.error() == "Meeting web server failed to start");
    CHECK(second.base_url().empty());
    first.stop();
}

TEST_CASE("broadcasts outside the server's run are dropped", "[server]") {
    Harness app(EngineKind::Canned);
    auto host = app.host();
    host->receive_until("hello");
    app.server->stop();
    app.server->broadcast(Json{{"type", "status"}});
    app.server->invalidate_connections();
    CHECK(host->closed_with() == 1012);
}

TEST_CASE("the voice command guide follows the settings", "[server]") {
    Harness app(EngineKind::Canned);
    CHECK(app.server->voice_commands()["primary"] == "Assistant");
    app.server->set_settings_source(
        [] { return Json{{"typesafe_voice_command_names", Json::array({"scribe"})}}; });
    CHECK(app.server->voice_commands()["names"] == Json::array({"Scribe"}));
    auto host = app.host();
    CHECK(host->receive()["voice_commands"]["primary"] == "Scribe");
}

// Binds every interface and opens the dashboard through this computer's LAN address, which
// other devices on the network can then reach: never run unattended (lead amendment 14).
TEST_CASE("a LAN-bound server answers on the machine's LAN address", "[server][interactive]") {
    const auto lan = fv::net::discover_lan_ipv4();
    if (!lan) {
        SKIP("this computer has no usable LAN address");
    }
    auto repo = std::make_shared<HelloRepository>(ws_meeting_row(), ws_segments());
    fv::FakeClock clock;
    fv::meeting_state::testing::FixedIds ids;
    fv::meeting_state::MeetingState state;
    state.meeting_id = "m_ws";
    auto store = std::make_shared<fv::meeting_state::MeetingStateStore>(
        std::move(state), nullptr, fv::meeting_state::SegmentHandler{}, clock, ids);
    mw::testing::FakeDashboardEngine engine("m_ws", store);
    fv::net::Reactor reactor;
    mw::MeetingWebServer server(reactor, engine, repo, "lan", 0);
    const auto url = server.start();
    REQUIRE(url.has_value());
    CHECK(*url == "http://" + *lan + ":" + std::to_string(port_of(*url)));
    boost::asio::io_context io;
    boost::asio::ip::tcp::socket client(io);
    client.connect({boost::asio::ip::make_address(*lan), port_of(*url)});
    const std::string request = "GET /nothing-here HTTP/1.1\r\nHost: " + *lan + "\r\n\r\n";
    boost::asio::write(client, boost::asio::buffer(request));
    char buffer[256];
    const auto read = client.read_some(boost::asio::buffer(buffer));
    CHECK(std::string(buffer, read).starts_with("HTTP/1.1 404"));
    server.stop();
}
