// The meeting-agent resolvers of services/settings.py.

#include <catch2/catch_test_macros.hpp>

#include <string>
#include <utility>
#include <vector>

#include "meeting_agent/agent_settings.h"

using fv::Json;
using namespace fv::meeting_agent;

TEST_CASE("the agent core reads pi when unset, unknown or the retired direct", "[agent_settings]") {
    CHECK(resolve_meeting_agent_core(Json::object()) == "pi");
    CHECK(resolve_meeting_agent_core({{"meeting_agent_core", "direct"}}) == "pi");
    CHECK(resolve_meeting_agent_core({{"meeting_agent_core", "bogus"}}) == "pi");
    CHECK(resolve_meeting_agent_core({{"meeting_agent_core", 3}}) == "pi");
    for (const char* kind : {"pi", "opencode", "claude_code", "codex", "opencode_cli"}) {
        CHECK(resolve_meeting_agent_core({{"meeting_agent_core", kind}}) == kind);
    }
}

TEST_CASE("only the installed coding agents are installed cores", "[agent_settings]") {
    CHECK(is_installed_core("claude_code"));
    CHECK(is_installed_core("codex"));
    CHECK(is_installed_core("opencode_cli"));
    CHECK_FALSE(is_installed_core("pi"));
    CHECK_FALSE(is_installed_core("opencode"));
    CHECK_FALSE(is_installed_core("direct"));
    CHECK_FALSE(is_installed_core(""));
}

TEST_CASE("agent models keep installed agents with text models, stripped", "[agent_settings]") {
    const Json settings = {{"meeting_agent_models",
                            {{"codex", "  gpt-5.1-codex  "},
                             {"pi", "ignored"},
                             {"claude_code", ""},
                             {"opencode_cli", 5},
                             {"unknown", "x"}}}};
    const std::vector<std::pair<std::string, std::string>> expected = {{"codex", "gpt-5.1-codex"},
                                                                       {"claude_code", ""}};
    CHECK(resolve_meeting_agent_models(settings) == expected);
    CHECK(resolve_meeting_agent_model("codex", settings) == "gpt-5.1-codex");
    CHECK(resolve_meeting_agent_model("claude_code", settings) == "");
    CHECK(resolve_meeting_agent_model("opencode_cli", settings) == "");
    CHECK(resolve_meeting_agent_model("pi", settings) == "");
    CHECK(resolve_meeting_agent_models(Json::object()).empty());
    CHECK(resolve_meeting_agent_models({{"meeting_agent_models", "not a dict"}}).empty());
}

TEST_CASE("thinking levels and the backup agent fall back to their defaults", "[agent_settings]") {
    CHECK(resolve_meeting_live_thinking(Json::object()) == "low");
    CHECK(resolve_meeting_final_thinking(Json::object()) == "medium");
    CHECK(resolve_meeting_live_thinking({{"meeting_live_thinking", "high"}}) == "high");
    CHECK(resolve_meeting_final_thinking({{"meeting_final_thinking", "xhigh"}}) == "xhigh");
    CHECK(resolve_meeting_live_thinking({{"meeting_live_thinking", "extreme"}}) == "low");
    CHECK(resolve_meeting_backup_agent(Json::object()) == "");
    CHECK(resolve_meeting_backup_agent({{"meeting_backup_agent", "codex"}}) == "codex");
    // Only installed agents can stand in.
    CHECK(resolve_meeting_backup_agent({{"meeting_backup_agent", "pi"}}) == "");
}
