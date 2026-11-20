#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "agents/agent_actions.h"
#include "agents/detail/opencode_cleanup.h"
#include "agents_test_support.h"
#include "base/utf8.h"
#include "pyport/json.h"

namespace fv::agents::detail {
namespace {

using namespace std::chrono_literals;
using Clock = std::chrono::steady_clock;

struct CleanupAgent {
    agents_test::AgentsFixture fixture;
    std::filesystem::path log = fixture.temp.path() / "cleanup.jsonl";
    InstalledAgent agent{std::string(OPENCODE), agents_test::exe(fixture.path_dir, "opencode"),
                         "2.0.18"};

    void install_fake(const char* mode) {
        std::filesystem::copy_file(FV_FAKE_AGENT, agent.path);
        fixture.env->insert_or_assign("FV_FAKE_AGENT_MODE", mode);
        fixture.env->insert_or_assign("FV_FAKE_AGENT_ACP_LOG", path_to_utf8(log));
        fixture.env->insert_or_assign("OPENCODE_CONFIG_CONTENT",
                                      R"({"mcp":{"servers":{"ambient":{}}}})");
        fixture.env->insert_or_assign("OPENCODE_PERMISSION", R"({"*":"allow"})");
    }

    std::vector<Json> entries() const {
        std::ifstream in(log);
        std::vector<Json> rows;
        std::string line;
        while (std::getline(in, line)) {
            rows.push_back(py::loads(line));
        }
        return rows;
    }
};

TEST_CASE("OpenCode cleanup denies tools in one private ACP session", "[agent_cleanup][platform]") {
#ifndef _WIN32
    SKIP("The native process fixture requires Windows");
#endif
    CleanupAgent fake;
    fake.install_fake("permission");
    InstalledAgents agents(make_process_launcher(), fake.fixture.base_env(), fake.fixture.home,
                           fake.fixture.tmp);
    const auto result = ask_opencode_cleanup(agents, fake.agent, "sample dictated words",
                                             "Format like a short note.", "prov/slow", "high",
                                             Clock::now() + 5s, {});
    REQUIRE(result.ok);
    CHECK(result.answer == "Answer: sample dictated words");

    const auto entries = fake.entries();
    REQUIRE(entries.size() == 7);
    const auto& overlay = entries[0].at("overlay");
    for (const auto* name : {"build", "plan"}) {
        const auto& profile = overlay.at("agents").at(name);
        const auto instructions = profile.at("system").get<std::string>();
        CHECK(instructions.starts_with("Format like a short note."));
        CHECK(instructions.ends_with(kCleanupClosing));
        CHECK(instructions.find("sample dictated words") == std::string::npos);
        CHECK(profile.at("permissions") ==
              Json::array({{{"action", "*"}, {"resource", "*"}, {"effect", "deny"}}}));
    }
    CHECK(overlay.at("policies").size() == 5);
    CHECK(overlay.at("policies").at(1) ==
          Json{{"action", "integration.use"}, {"resource", "mcp:*"}, {"effect", "deny"}});
    CHECK(overlay.at("policies").at(2) ==
          Json{{"action", "integration.use"}, {"resource", "plugin:*"}, {"effect", "deny"}});
    CHECK(overlay.at("policies").back() == Json{{"action", "integration.use"},
                                                {"resource", "plugin:opencode.provider.*"},
                                                {"effect", "allow"}});
    CHECK(overlay.at("plugins").empty());
    CHECK(overlay.at("instructions").empty());
    CHECK(entries[0].at("project_config_off") == "1");
    CHECK(entries[1].at("method") == "initialize");
    CHECK(entries[2].at("method") == "session/new");
    CHECK(entries[2].at("params").at("mcpServers") == Json::array());
    const auto cwd = path_from_utf8(entries[2].at("params").at("cwd").get<std::string>());
    CHECK(cwd.filename() == "workspace");
    CHECK_FALSE(std::filesystem::exists(cwd.parent_path()));
    CHECK(entries[3].at("method") == "session/set_config_option");
    CHECK(entries[3].at("params").at("value") == "prov/slow");
    CHECK(entries[4].at("method") == "session/prompt");
    CHECK(entries[4].at("params").at("prompt") ==
          Json::array({{{"type", "text"}, {"text", "sample dictated words"}}}));
    CHECK(entries[5].at("result").at("outcome").at("optionId") == "no");
    CHECK(entries[6].at("method") == "session/delete");
}

TEST_CASE("OpenCode cleanup with an unavailable configured model sends no transcript",
          "[agent_cleanup][platform]") {
#ifndef _WIN32
    SKIP("The native process fixture requires Windows");
#endif
    CleanupAgent fake;
    fake.install_fake("ok");
    InstalledAgents agents(make_process_launcher(), fake.fixture.base_env(), fake.fixture.home,
                           fake.fixture.tmp);
    const bool malformed = GENERATE(false, true);
    fake.fixture.env->insert_or_assign(
        "OPENCODE_CONFIG_CONTENT", malformed
                                       ? R"({"model":"#unsupported-variant"})"
                                       : R"({"model":{"providerID":"unknown","model":"missing"}})");
    const auto result = ask_opencode_cleanup(agents, fake.agent, "sample dictated words", "", "",
                                             "", Clock::now() + 5s, {});
    CHECK_FALSE(result.ok);
    CHECK(result.message == "OpenCode did not complete transcript cleanup.");
    const auto entries = fake.entries();
    if (malformed) {
        CHECK(entries.empty());
        return;
    }
    REQUIRE_FALSE(entries.empty());
    for (const auto& row : entries) {
        CHECK(row.value("method", Json()) != "session/prompt");
        CHECK(row.dump().find("sample dictated words") == std::string::npos);
        CHECK(row.dump().find("unknown/missing") == std::string::npos);
    }
}

TEST_CASE("OpenCode cleanup cannot redirect a privately configured provider",
          "[agent_cleanup][platform]") {
#ifndef _WIN32
    SKIP("The native process fixture requires Windows");
#endif
    CleanupAgent fake;
    fake.install_fake("ok");
    InstalledAgents agents(make_process_launcher(), fake.fixture.base_env(), fake.fixture.home,
                           fake.fixture.tmp);
    const bool same_provider = GENERATE(false, true);
    const std::string configured_provider = same_provider ? "prov" : "unrelated";
    const Json ambient{
        {"model", "prov/fast"},
        {"providers",
         {{configured_provider, {{"settings", {{"baseURL", "https://example.invalid"}}}}}}}};
    fake.fixture.env->insert_or_assign("OPENCODE_CONFIG_CONTENT", ambient.dump());
    const auto result = ask_opencode_cleanup(agents, fake.agent, "sample dictated words", "", "",
                                             "", Clock::now() + 5s, {});
    if (same_provider) {
        CHECK_FALSE(result.ok);
        CHECK(fake.entries().empty());
    } else {
        CHECK(result.ok);
        CHECK(result.answer == "Answer: sample dictated words");
    }
}

TEST_CASE("OpenCode cleanup checks cancellation and deadline before launching", "[agent_cleanup]") {
    CleanupAgent fake;
    InstalledAgents agents(fake.fixture.launcher, fake.fixture.base_env(), fake.fixture.home,
                           fake.fixture.tmp);
    CancelSource cancel;
    cancel.cancel();
    CHECK(ask_opencode_cleanup(agents, fake.agent, "words", "", "", "", Clock::now() + 5s,
                               cancel.token())
              .stopped);
    CHECK_FALSE(
        ask_opencode_cleanup(agents, fake.agent, "words", "", "", "", Clock::now() - 1s, {}).ok);
    CHECK(fake.fixture.launcher->specs().empty());
}

} // namespace
} // namespace fv::agents::detail
