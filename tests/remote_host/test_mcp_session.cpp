#include <catch2/catch_test_macros.hpp>

#include <atomic>

#include "base/testing/manual_executor.h"
#include "host_rig.h"
#include "remote_host/mcp_session.h"

using namespace std::chrono_literals;

TEST_CASE("a peer MCP grant binds the exact device and connection and never returns its token") {
    fv::ManualExecutor timers;
    std::atomic<bool> running = false;
    std::atomic<int> starts = 0;
    std::atomic<int> stops = 0;
    using fv::Json;
    using fv::remote_host::PeerMcpSession;
    auto session = std::make_shared<PeerMcpSession>(
        timers, PeerMcpSession::Listener{
                    .start =
                        [&] {
                            ++starts;
                            running = true;
                        },
                    .stop =
                        [&] {
                            ++stops;
                            running = false;
                        },
                    .status =
                        [&] {
                            return Json{{"state", running.load() ? "running" : "stopped"},
                                        {"port", PeerMcpSession::kPort},
                                        {"tailscale", false},
                                        {"token", "fixture-local-token"}};
                        },
                    .local_token = [] { return "fixture-local-token"; },
                });

    namespace rig = fv::remote_host::rig;
    rig::HostRig host([&](fv::remote_host::SpeechHostDeps& deps) {
        deps.mcp_peer_allowed = [session](std::string_view device) {
            return session->allowed(device);
        };
        deps.mcp_peer_request = [session](std::string_view op, const Json& header,
                                          std::string_view device, std::string_view connection) {
            return session->request(op, header, device, connection);
        };
        deps.mcp_peer_disconnect = [session](std::string_view connection) {
            session->disconnect(connection);
        };
    });
    const auto paired = rig::pair_with_code(host.port, host.host->open_pairing(), "Authorized");
    const auto other = rig::pair_with_code(host.port, host.host->open_pairing(), "Other");
    REQUIRE_FALSE(paired.device_id.empty());
    REQUIRE(paired.device_id != other.device_id);
    REQUIRE(session->grant(paired.device_id));
    CHECK_FALSE(session->grant(other.device_id));
    auto authorized = rig::connect(host.port, paired.token);
    auto denied = rig::connect(host.port, other.token);
    REQUIRE(authorized.ready.at("capabilities").at("mcp_control") == true);
    REQUIRE(denied.ready.at("capabilities").at("mcp_control") == false);
    const auto begin = Json{{"settings", {{"enabled", true}}}};
    CHECK(denied.client->request("mcp_configure", begin).at("code") == "forbidden");
    CHECK(authorized.client
              ->request("mcp_configure", {{"settings", {{"enabled", true}, {"tailscale", true}}}})
              .at("code") == "bad_request");
    CHECK_FALSE(running.load());
    const auto state = authorized.client->request("mcp_configure", begin).at("result");
    CHECK(state.at("state") == "running");
    CHECK(state.at("port") == PeerMcpSession::kPort);
    CHECK(state.dump().find("fixture-local-token") == std::string::npos);
    CHECK(session->local_token() == "fixture-local-token");
    auto same_device = rig::connect(host.port, paired.token);
    CHECK(same_device.client->request("mcp_configure", {{"settings", {{"enabled", false}}}})
              .at("code") == "forbidden");
    CHECK(running.load());

    authorized.client->close();
    REQUIRE(rig::wait_for([&] { return !running.load(); }));
    CHECK_FALSE(session->allowed(paired.device_id));
    CHECK(starts.load() == 1);
    CHECK(stops.load() == 1);
    CHECK(same_device.client->request("mcp_configure", begin).at("code") == "forbidden");

    REQUIRE(session->grant(paired.device_id));
    REQUIRE(same_device.client->request("mcp_configure", begin).at("result").at("state") ==
            "running");
    timers.advance(15min);
    CHECK_FALSE(running.load());
    CHECK_FALSE(session->allowed(paired.device_id));
    CHECK(starts.load() == 2);
    CHECK(stops.load() == 2);
}
