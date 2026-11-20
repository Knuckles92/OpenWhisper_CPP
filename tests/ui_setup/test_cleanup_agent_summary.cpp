// guided_setup.py provider_summary for a cleanup runner that is an installed coding agent: it
// reads the session's cached agent scan, never a probe.
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <optional>
#include <string>
#include <utility>

#include "agents/installed_agents.h"
#include "ui_setup/cleanup_provider_row.h"

using fv::Json;

namespace {

fv::agents::InstalledAgent installed(std::string id, std::optional<bool> signedIn) {
    fv::agents::InstalledAgent agent;
    agent.id = std::move(id);
    agent.path = "/bin/agent";
    agent.version = "9.0.0";
    agent.signed_in = signedIn;
    return agent;
}

} // namespace

TEST_CASE("test_cleanup_agent_summary_uses_sign_in_without_probing", "[unit]") {
    const auto [agentId, name] =
        GENERATE(std::pair<std::string, QString>{"claude_code", "Claude Code"},
                 std::pair<std::string, QString>{"codex", "Codex"},
                 std::pair<std::string, QString>{"opencode_cli", "OpenCode"});
    const Json settings{{"transcript_cleanup_provider", agentId}, {"transcript_cleanup_model", ""}};
    int keyLookups = 0;
    const auto keySaved = [&](const std::string&) {
        ++keyLookups;
        return false;
    };
    // No scan yet (cached_scan() is None): the agent's own sign-in is trusted.
    const auto result = fv::providerSummary(settings, keySaved, std::nullopt);
    CHECK(result.name == name);
    CHECK(result.status.contains("sign-in"));
    CHECK(result.privacy.contains(name));
    CHECK(result.privacy.contains("No audio"));
    CHECK(result.remote);
    CHECK_FALSE(result.needsKey);
    CHECK(keyLookups == 0);

    // A scan that found it usable says the same; one that did not says what to do.
    const fv::agents::ScanResult usable{{agentId, installed(agentId, true)}};
    CHECK(fv::providerSummary(settings, keySaved, usable).status == "Uses your agent's sign-in.");
    for (const auto& scan :
         {fv::agents::ScanResult{}, fv::agents::ScanResult{{agentId, std::nullopt}},
          fv::agents::ScanResult{{agentId, installed(agentId, false)}}}) {
        CHECK(fv::providerSummary(settings, keySaved, scan).status ==
              "Install or sign in to this agent in All settings.");
    }
    // Without an agent instance the default overload has no scan to read and probes nothing.
    CHECK(fv::providerSummary(settings, keySaved).status == "Uses your agent's sign-in.");
}
