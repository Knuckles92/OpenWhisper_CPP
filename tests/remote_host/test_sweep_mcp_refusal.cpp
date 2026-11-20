// What FioriVoce's host says to a paired computer it has not granted MCP control. Its Remote
// engine page offers "Grant session MCP control to one paired computer", not OpenWhisper's
// "Allow paired computers to manage MCP" (a17-paired-mcp), so the refusal names the grant. A
// host built without the session (OpenWhisper's opt-in, as the recorded wire transcripts
// replay it) keeps OpenWhisper's words.

#include <catch2/catch_test_macros.hpp>

#include <string>

#include "host_rig.h"

using fv::Json;
namespace rig = fv::remote_host::rig;

TEST_CASE("sweep A38: a host that grants MCP by session names the grant when it refuses") {
    rig::HostRig host([](fv::remote_host::SpeechHostDeps& deps) {
        deps.mcp_peer_allowed = [](std::string_view) {
            return false;
        };
        deps.mcp_peer_request = [](std::string_view, const Json&, std::string_view,
                                   std::string_view) -> Json {
            return Json::object();
        };
    });
    auto connection = host.connected();
    CHECK(connection.ready.at("capabilities").at("mcp_control") == false);

    const Json refused = connection.client->request("mcp_state");
    CHECK(refused.at("code") == "forbidden");
    CHECK(refused.at("error") ==
          "devbox hasn't granted this computer MCP control. Turn on \"Grant session MCP control "
          "to one paired computer\" in Settings → Remote engine there and choose this "
          "computer.");
}

TEST_CASE("sweep A38: a host with OpenWhisper's MCP opt-in keeps its refusal") {
    rig::HostRig host([](fv::remote_host::SpeechHostDeps& deps) {
        deps.mcp_enabled = [] {
            return false;
        };
        deps.mcp = [](std::string_view, const Json&, std::string_view) -> Json {
            return Json::object();
        };
    });
    auto connection = host.connected();

    const Json refused = connection.client->request("mcp_state");
    CHECK(refused.at("code") == "forbidden");
    CHECK(refused.at("error") ==
          "devbox isn't letting paired computers manage its MCP server. Turn on \"Allow paired "
          "computers to manage MCP\" in Settings → Remote engine there.");
}
