// The host's MCP grant to one paired computer (PeerMcpSession behind RemoteEngineService), in
// place of OpenWhisper's HostMcpControl (tests/test_remote_mcp_control.py): the peer can start
// and stop one read-only loopback session for 15 minutes and read its state; it can change none
// of the host's MCP settings, never receives the token, and cannot grant itself
// (docs/port/differences.md a17-paired-mcp).

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <atomic>
#include <chrono>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "base/testing/fake_clock.h"
#include "base/testing/manual_executor.h"
#include "base/testing/map_env_reader.h"
#include "base/utf8.h"
#include "credentials/testing/memory_backend.h"
#include "host_rig.h"
#include "pyport/errors.h"
#include "remote/errors.h"
#include "remote/testing/fake_tailscale_cli.h"
#include "remote_host/mcp_session.h"
#include "remote_host/service.h"
#include "settings/keys.h"

using namespace fv;
using namespace fv::remote_host;
using namespace std::chrono_literals;
using Catch::Matchers::ContainsSubstring;
using Catch::Matchers::StartsWith;

namespace {

const std::string kToken = "host-session-token-" + std::string(30, 'b');

// The loopback listener the grant starts and stops; the real one binds a socket.
struct FakeListener {
    std::atomic<bool> running = false;
    std::atomic<int> starts = 0;
    std::atomic<int> stops = 0;

    PeerMcpSession::Listener make() {
        return PeerMcpSession::Listener{
            .start =
                [this] {
                    ++starts;
                    running = true;
                },
            .stop =
                [this] {
                    ++stops;
                    running = false;
                },
            // What McpRuntime reports, token and remote address included: none of it beyond
            // the state may reach the peer.
            .status =
                [this] {
                    return Json{{"state", running.load() ? "running" : "stopped"},
                                {"message", running.load() ? "Ready" : "MCP is off."},
                                {"port", PeerMcpSession::kPort},
                                {"tailscale", false},
                                {"remote_url", "http://100.64.0.9:8768/mcp"},
                                {"token", kToken}};
                },
            .local_token = [] { return kToken; },
        };
    }
};

std::string refusal(const std::function<void()>& call) {
    try {
        call();
    } catch (const py::ValueError& error) {
        return error.what();
    }
    return {};
}

MapEnvReader private_env(const std::filesystem::path& root) {
    return MapEnvReader{
        {std::string(identity().env_prefix) + "DATA_DIR", path_to_utf8(root)},
        {"LOCALAPPDATA", path_to_utf8(root / "local")},
        {"XDG_DATA_HOME", path_to_utf8(root / "local")},
        {"HOME", path_to_utf8(root / "home")},
        {"USERPROFILE", path_to_utf8(root / "home")},
    };
}

// A whole RemoteEngineService per data root, with or without the grant (test_service.cpp's
// ServiceRig plus the mcp_session it is given).
struct GrantRig {
    MapEnvReader env;
    Paths paths;
    SettingsStore settings{paths.user_file(identity().settings_file)};
    CredentialStore credentials{std::make_shared<MemoryCredentialBackend>(), identity()};
    std::shared_ptr<RemoteEngineService> service;

    GrantRig(const std::filesystem::path& root, std::string name, net::Reactor& reactor,
             Clock& clock, Executor& io, std::shared_ptr<PeerMcpSession> session = {})
        : env(private_env(root)), paths(Paths::resolve(identity(), env)),
          service(std::make_shared<RemoteEngineService>(RemoteEngineServiceDeps{
              .paths = paths,
              .identity = identity(),
              .settings = settings,
              .credentials = credentials,
              .env = env,
              .clock = clock,
              .io = io,
              .client_env = {reactor},
              .computer_name = std::move(name),
              .tailscale = std::make_shared<remote::testing::FakeTailscaleCli>(),
              .engine = [] { return unavailable_engine("No speech engine."); },
              .bind = "127.0.0.1",
              .discovery_port = std::nullopt,
              .listen_port = 0,
              .mcp_session = std::move(session)})) {}
};

// A host sharing its engine and a client paired with it by code.
struct PairedServices {
    testkit::TempDataRoot folder;
    net::Reactor reactor;
    FakeClock clock;
    rig::InlineExecutor io;
    ManualExecutor timers;
    FakeListener listener;
    std::shared_ptr<PeerMcpSession> session;
    std::optional<GrantRig> host;
    std::optional<GrantRig> client;
    remote::ClientPairing pairing;

    explicit PairedServices(bool with_grant = true) {
        if (with_grant) {
            session = std::make_shared<PeerMcpSession>(timers, listener.make());
        }
        host.emplace(folder.path() / "host", "Desk", reactor, clock, io, session);
        client.emplace(folder.path() / "client", "Laptop", reactor, clock, io);
        host->settings.save_setting(keys::remote_host_enabled.key(), true);
        host->service->apply_host_settings();
        const auto state = host->service->host_state();
        REQUIRE(state.running);
        const auto code = host->service->open_pairing();
        REQUIRE(code);
        pairing =
            client->service->pair(remote::format_address("127.0.0.1", state.port), *code, false);
    }
    ~PairedServices() {
        if (client) {
            client->service->close_mcp_control();
        }
    }
    Json request(std::string_view op, const Json& fields = Json::object()) {
        return client->service->remote_mcp_request(op, fields, pairing);
    }
};

const Json kStart{{"settings", {{"enabled", true}}}};

} // namespace

TEST_CASE("a session request other than the enabled switch changes nothing", "[mcp_grant]") {
    ManualExecutor timers;
    FakeListener listener;
    auto session = std::make_shared<PeerMcpSession>(timers, listener.make());
    REQUIRE(session->grant("laptop"));
    // test_invalid_requests_change_nothing's requests, each with an allowed change beside it.
    const std::vector<Json> changes{
        {{"unknown", true}},
        {{"enabled", "yes"}},
        {{"enabled", 1}},
        {{"port", 0}},
        {{"port", 70000}},
        {{"port", true}},
        {{"port", "8767"}},
        {{"writable", {{"remote_host_manage_mcp", true}}}},
        {{"writable", {{"auto_paste", "true"}}}},
        {{"writable", Json::array()}},
        {{"retitle_meetings", nullptr}},
        {{"tailscale", true}},
        {{"retitle_transcriptions", true}},
        {{"settings_access", true}},
    };
    for (const auto& change : changes) {
        Json settings = change;
        settings["enabled"] = true;
        if (change.contains("enabled")) {
            settings = change;
            settings["retitle_transcriptions"] = true;
        }
        INFO(settings.dump());
        CHECK_THAT(refusal([&] {
                       session->request("mcp_configure", {{"settings", settings}}, "laptop", "c1");
                   }),
                   StartsWith("bad_request:"));
    }
    for (const Json& header :
         {Json::object(), Json{{"settings", Json::array()}}, Json{{"settings", true}}}) {
        CHECK_THAT(refusal([&] { session->request("mcp_configure", header, "laptop", "c1"); }),
                   StartsWith("bad_request:"));
    }
    CHECK_THAT(refusal([&] { session->request("mcp_unknown", kStart, "laptop", "c1"); }),
               StartsWith("bad_request:"));
    CHECK(listener.starts == 0);
    CHECK_FALSE(listener.running);
}

TEST_CASE("a paired computer cannot reach its own grant", "[mcp_grant]") {
    ManualExecutor timers;
    FakeListener listener;
    auto session = std::make_shared<PeerMcpSession>(timers, listener.make());
    // Nothing a computer without the grant sends gives it one.
    for (const Json& header :
         {kStart, Json{{"settings", {{"remote_host_manage_mcp", true}}}},
          Json{{"settings", {{"writable", {{"remote_host_manage_mcp", true}}}}}}}) {
        CHECK_THAT(refusal([&] { session->request("mcp_configure", header, "laptop", "c1"); }),
                   StartsWith("forbidden:"));
    }
    CHECK_THAT(refusal([&] { session->request("mcp_state", Json::object(), "laptop", "c1"); }),
               StartsWith("forbidden:"));
    CHECK_FALSE(session->allowed("laptop"));
    // With a grant, the gate itself is still not something the peer can set, and a second
    // grant waits for the first to end.
    REQUIRE(session->grant("laptop"));
    CHECK_THAT(refusal([&] {
                   session->request("mcp_configure",
                                    {{"settings", {{"remote_host_manage_mcp", true}}}}, "laptop",
                                    "c1");
               }),
               StartsWith("bad_request:"));
    CHECK_FALSE(session->grant("laptop"));
    CHECK_FALSE(session->grant("phone"));
    CHECK(listener.starts == 0);
}

TEST_CASE("the state a peer reads names only the session, never its token", "[mcp_grant]") {
    ManualExecutor timers;
    FakeListener listener;
    auto session = std::make_shared<PeerMcpSession>(timers, listener.make());
    REQUIRE(session->grant("laptop"));
    const auto before = session->request("mcp_state", Json::object(), "laptop", "c1");
    CHECK(before ==
          Json{{"state", "stopped"}, {"port", PeerMcpSession::kPort}, {"tailscale", false}});
    CHECK(session->local_token().empty());
    const auto started = session->request("mcp_configure", kStart, "laptop", "c1");
    CHECK(started ==
          Json{{"state", "running"}, {"port", PeerMcpSession::kPort}, {"tailscale", false}});
    CHECK(session->request("mcp_state", Json::object(), "laptop", "c1") == started);
    // The token is the host's to show (Remote engine's "Show local MCP session connection").
    CHECK(session->local_token() == kToken);
    CHECK(session->local_status().at("token") == kToken);
    session->request("mcp_configure", {{"settings", {{"enabled", false}}}}, "laptop", "c1");
    CHECK_FALSE(listener.running);
    CHECK(session->local_token().empty());
    CHECK_FALSE(session->allowed("laptop"));
}

TEST_CASE("the host grants only a paired computer while it shares, and never the old opt-in",
          "[mcp_grant]") {
    PairedServices paired;
    auto& host = *paired.host->service;
    CHECK_THROWS_AS(host.set_manage_mcp(true), remote::RemoteEngineError);
    CHECK_THROWS_AS(host.grant_mcp_session("no-such-device"), remote::RemoteEngineError);
    CHECK_FALSE(host.host_state().manage_mcp);
    host.grant_mcp_session(paired.pairing.device_id);
    CHECK(host.host_state().manage_mcp);
    CHECK_THROWS_WITH(host.grant_mcp_session(paired.pairing.device_id),
                      ContainsSubstring("End the current MCP grant"));
    // Turning the old opt-in off ends the grant.
    host.set_manage_mcp(false);
    CHECK_FALSE(host.host_state().manage_mcp);
    host.grant_mcp_session(paired.pairing.device_id);
    host.set_host_enabled(false);
    CHECK_FALSE(paired.session->allowed(paired.pairing.device_id));
    CHECK_THROWS_AS(host.grant_mcp_session(paired.pairing.device_id), remote::RemoteEngineError);
}

TEST_CASE("a granted computer starts the host's session over pinned TLS and ending it revokes",
          "[mcp_grant]") {
    PairedServices paired;
    // No grant yet: the host turns the peer away and says so.
    try {
        paired.request("mcp_state");
        FAIL("the host answered without a grant");
    } catch (const remote::RemoteRequestError& refused) {
        CHECK(refused.code == "forbidden");
        // The host's page offers a grant to one computer, not OpenWhisper's opt-in switch.
        CHECK(std::string(refused.what()) ==
              "The host isn't letting this computer manage its MCP server. On the host, grant "
              "this computer MCP control in Settings → Remote engine.");
    }
    paired.host->service->grant_mcp_session(paired.pairing.device_id);
    CHECK(paired.request("mcp_state").at("state") == "stopped");
    // The peer may not change the host's settings, even with the grant.
    CHECK_THROWS_AS(paired.request("mcp_configure", {{"settings", {{"port", 9000}}}}),
                    py::ValueError);
    CHECK_THROWS_AS(paired.client->service->remote_mcp_request("mcp_configure", kStart),
                    py::ValueError);
    CHECK(paired.listener.starts == 0);

    const auto started = paired.request("mcp_configure", kStart);
    CHECK(started.at("state") == "running");
    CHECK(started.at("port") == PeerMcpSession::kPort);
    CHECK(started.dump().find(kToken) == std::string::npos);
    CHECK(started.dump().find("100.64.0.9") == std::string::npos);
    CHECK(paired.host->service->local_mcp_session_token() == kToken);
    CHECK(paired.request("mcp_state").at("state") == "running");

    // End host session: the client disconnects and the host revokes the grant.
    paired.client->service->close_mcp_control();
    REQUIRE(rig::wait_for([&] { return !paired.listener.running.load(); }));
    CHECK_FALSE(paired.session->allowed(paired.pairing.device_id));
    CHECK(paired.host->service->local_mcp_session_token().empty());
    CHECK(paired.listener.starts == 1);
    CHECK(paired.listener.stops == 1);
    try {
        paired.request("mcp_configure", kStart);
        FAIL("the grant outlived the session");
    } catch (const remote::RemoteRequestError& refused) {
        CHECK(refused.code == "forbidden");
    }
}

TEST_CASE("the grant ends by itself after 15 minutes", "[mcp_grant]") {
    PairedServices paired;
    paired.host->service->grant_mcp_session(paired.pairing.device_id);
    REQUIRE(paired.request("mcp_configure", kStart).at("state") == "running");
    paired.timers.advance(15min - 1s);
    CHECK(paired.listener.running);
    paired.timers.advance(1s);
    CHECK_FALSE(paired.listener.running);
    CHECK_FALSE(paired.session->allowed(paired.pairing.device_id));
    CHECK_FALSE(paired.host->service->host_state().manage_mcp);
}

TEST_CASE("a host without the grant is reported as unsupported", "[mcp_grant]") {
    PairedServices paired(false);
    try {
        paired.request("mcp_state");
        FAIL("a host without the capability answered");
    } catch (const remote::RemoteRequestError& refused) {
        CHECK(refused.code == "unsupported");
        CHECK_THAT(std::string(refused.what()), ContainsSubstring("Update"));
    }
    CHECK_THROWS_AS(paired.host->service->grant_mcp_session(paired.pairing.device_id),
                    remote::RemoteEngineError);
}
