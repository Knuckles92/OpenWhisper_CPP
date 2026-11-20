// What the desktop's local MCP session serves instead of the live API (difference
// a20-local-mcp-saved-content, a19-live-api-not-served): built with allow_live_tools=false
// (ui_automation/session_mcp.cpp), it lists and runs none of the live, recording, events or
// notice tools, so no MCP client can run an action, start a dictation or read live events, even
// with every live permission granted. The saved-content tools still answer.
#include <catch2/catch_test_macros.hpp>

#include <memory>
#include <string>

#include "agentapi/history_server.h"
#include "mcp_support.h"
#include "settings/testing/memory_store.h"
#include "support.h"
#include "testkit/testkit.h"

namespace {

using fv::Json;
using namespace fv::agentapi;
using namespace fv::agentapi::test;

const std::string kSessionToken = "mcp-session-token-" + std::string(32, 'c');

} // namespace

TEST_CASE("the desktop's local MCP session has no live tools", "[unit]") {
    fv::testkit::TempDataRoot root;
    // Every live permission granted: the tools are absent, not merely refused.
    fv::MemorySettingsStore settings(Json{{"mcp_enabled", true},
                                          {"api_live_events", true},
                                          {"api_run_actions", true},
                                          {"api_control_recording", true},
                                          {"api_text_filters", true},
                                          {"api_agent_prompts", true}});
    McpAppOptions options;
    options.database = copy_history_fixture(root.path(), "history.db");
    options.token = kSessionToken;
    options.enabled = [] {
        return true;
    };
    options.settings = &settings;
    options.allow_live_tools = false;
    McpApp app(std::move(options));

    for (const char* tool :
         {"get_live_state", "list_actions", "run_action", "start_dictation", "stop_dictation",
          "cancel_dictation", "recent_events", "notify_user"}) {
        INFO(tool);
        const Json result = call_tool(app, kSessionToken, tool, Json::object());
        CHECK(
            (result.contains("isError") ? result.at("isError") == true : result.contains("error")));
    }
    CHECK_FALSE(call_tool(app, kSessionToken, "get_status", Json::object()).value("isError", false));
}
