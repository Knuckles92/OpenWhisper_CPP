// Which agent runs a meeting, and with what model (tests/test_installed_agent_core.py's route
// tests). OpenWhisper's packaged payload directory is gone, so the packaged-OpenCode route is the
// built-in loop's route.

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <optional>

#include "meeting_agent/route.h"

using fv::Json;
using namespace fv::meeting_agent;

// from tests/test_installed_agent_core.py::test_route_for_an_installed_agent
TEST_CASE("test_route_for_an_installed_agent", "[test_installed_agent_core]") {
    const Json settings{{"meeting_agent_core", "codex"},
                        {"meeting_agent_models", Json{{"codex", "gpt-6-sol"}, {"bogus", "x"}}}};
    const auto route = resolve_meeting_agent_route(settings);
    CHECK(route == MeetingAgentRoute{"codex", "codex", "gpt-6-sol", Json()});
    CHECK(route.installed());
}

// from tests/test_installed_agent_core.py::test_a_meeting_recorded_with_an_agent_keeps_it
TEST_CASE("test_a_meeting_recorded_with_an_agent_keeps_it", "[test_installed_agent_core]") {
    const Json settings{{"meeting_agent_core", "direct"}};
    const auto route = resolve_meeting_agent_route(
        settings, Json{{"agent_provider", "opencode_cli"}, {"agent_model", "prov/fast"}});
    CHECK(route.kind == "opencode_cli");
    CHECK(route.model == "prov/fast");
    CHECK(route.provider == "opencode_cli");
    CHECK(route.endpoint.is_null());
}

// from
// tests/test_installed_agent_core.py::test_a_text_endpoint_meeting_rerun_on_a_newly_chosen_agent
TEST_CASE("test_a_text_endpoint_meeting_rerun_on_a_newly_chosen_agent",
          "[test_installed_agent_core]") {
    const Json settings{{"meeting_agent_core", "claude_code"}};
    const auto route = resolve_meeting_agent_route(
        settings, Json{{"agent_provider", "openrouter"}, {"agent_model", "deepseek/v4"}});
    CHECK(route.kind == "claude_code");
    CHECK(route.provider == "claude_code");
    CHECK(route.model == "");
}

// from
// tests/test_installed_agent_core.py::test_the_archive_ignores_a_recorded_endpoint_for_an_agent
// OpenWhisper_CPP's archive dashboard carries no agent options of its own: its re-runs and reports
// resolve the destination here from the settings and the saved row (MeetingRuntime's
// approve_saved_agent), so the row's recorded text endpoint is what must be ignored.
TEST_CASE("test_the_archive_ignores_a_recorded_endpoint_for_an_agent",
          "[test_installed_agent_core]") {
    const Json settings{{"meeting_agent_core", "claude_code"},
                        {"meeting_agent_models", Json{{"claude_code", "haiku"}}}};
    const Json meeting{
        {"id", "m1"},
        {"agent_provider", "openrouter"},
        {"agent_model", "deepseek/v4"},
        {"agent_endpoint_json",
         R"({"provider": "openrouter", "base_url": "https://openrouter.ai/api/v1"})"}};
    const auto route = resolve_meeting_agent_route(settings, std::optional<Json>(meeting));
    CHECK(route == MeetingAgentRoute{"claude_code", "claude_code", "haiku", Json()});
    CHECK(route.installed());
}

// from
// tests/test_installed_agent_core.py::test_a_new_meeting_without_pi_keeps_pi_without_a_fallback
TEST_CASE("test_a_new_meeting_without_pi_keeps_pi_without_a_fallback",
          "[test_installed_agent_core]") {
    const auto route = resolve_meeting_agent_route(Json{{"meeting_agent_core", "pi"}});
    CHECK(route.kind == "pi");
    CHECK_FALSE(route.endpoint.is_null());
    CHECK_FALSE(route.installed());
}

// from
// tests/test_installed_agent_core.py::test_retired_direct_route_migrates_to_pi_for_new_and_saved_meetings
TEST_CASE("test_retired_direct_route_migrates_to_pi_for_new_and_saved_meetings",
          "[test_installed_agent_core]") {
    const bool saved = GENERATE(false, true);
    const Json settings{{"meeting_agent_core", "direct"},
                        {"meeting_llm_provider", "openrouter"},
                        {"meeting_llm_model", "deepseek/current"}};
    std::optional<Json> meeting;
    if (saved) {
        meeting.emplace(Json{{"agent_provider", "openrouter"}, {"agent_model", "deepseek/saved"}});
    }
    const auto route = resolve_meeting_agent_route(settings, meeting);
    CHECK(route.kind == "pi");
    CHECK(route.provider == "openrouter");
    CHECK_FALSE(route.endpoint.is_null());
    CHECK(route.model == (saved ? "deepseek/saved" : "deepseek/current"));
}

// from
// tests/test_installed_agent_core.py::test_packaged_opencode_and_installed_opencode_have_distinct_routes
TEST_CASE("test_packaged_opencode_and_installed_opencode_have_distinct_routes",
          "[test_installed_agent_core]") {
    Json settings{{"meeting_agent_core", "opencode"},
                  {"meeting_llm_provider", "openrouter"},
                  {"meeting_llm_model", "deepseek/test"}};
    const auto sdk = resolve_meeting_agent_route(settings);
    CHECK(sdk.kind == "opencode");
    CHECK(sdk.provider == "openrouter");
    CHECK(sdk.model == "deepseek/test");
    CHECK_FALSE(sdk.installed());
    CHECK_FALSE(sdk.endpoint.is_null());
    settings["meeting_agent_core"] = "opencode_cli";
    const auto cli = resolve_meeting_agent_route(settings);
    CHECK(cli == MeetingAgentRoute{"opencode_cli", "opencode_cli", "", Json()});
    CHECK(cli.installed());
}

TEST_CASE("an empty meeting row is a new meeting", "[route]") {
    const Json settings{{"meeting_agent_core", "pi"},
                        {"meeting_llm_provider", "openrouter"},
                        {"meeting_llm_model", "deepseek/current"}};
    CHECK(resolve_meeting_agent_route(settings, Json::object()) ==
          resolve_meeting_agent_route(settings));
}
