#include <catch2/catch_test_macros.hpp>

#include <string_view>

#include "agents/agent_actions.h"

// The prompt text is data copied from OpenWhisper (transcript_cleanup.py, _cleanup_with_agent's
// `closing`): the agent cleanup of Claude Code, Codex and OpenCode ends its instructions with it.
TEST_CASE("agent cleanup closes its instructions with OpenWhisper's sentence", "[unit]") {
    constexpr std::string_view python =
        "Return only the finished text requested by the instructions, without commentary.";
    const std::string_view closing = fv::agents::kCleanupClosing;
    REQUIRE(closing.starts_with("\n\n"));
    CHECK(closing.substr(2, python.size()) == python);
    // FioriVoce's cleanup path sends no tool contract, so the tool-free statement follows.
    CHECK(closing.substr(2 + python.size()) == " No tools are available.");
}
