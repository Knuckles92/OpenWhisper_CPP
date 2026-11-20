// The channel's process entry and setup snippets.
// from tests/test_automation_channel_protocol.py (the tests of main, update_in_progress and
// services/agent_mcp/setup.py's channel entry)
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <algorithm>
#include <filesystem>
#include <string>
#include <vector>

#include "agentapi/setup.h"
#include "base/identity.h"
#include "base/json.h"
#include "base/utf8.h"
#include "cli/channel_command.h"
#include "cli_process.h"
#include "pyport/json.h"
#include "pyport/text.h"
#include "testkit/testkit.h"

using Catch::Matchers::ContainsSubstring;
using fv::Json;
using namespace fv::clitest;
namespace fs = std::filesystem;

namespace {

const std::string kToken = "t0ken-for-the-channel-tests-0123456789abcdef";

std::string server_name() {
    return fv::py::lower(fv::identity().product);
}

} // namespace

TEST_CASE("test_no_update_check_outside_the_frozen_windows_app",
          "[test_automation_channel_protocol]") {
    // This test's executable is a build tree's, never the registered install.
    CHECK_FALSE(fv::cli::update_in_progress());
}

TEST_CASE("test_help_and_unexpected_arguments", "[platform][test_automation_channel_protocol]") {
    const CliResult help = run_cli({"--channel", "--help"}, {});
    CHECK(help.exit_code == 0u);
    CHECK_THAT(help.err,
               ContainsSubstring("claude --dangerously-load-development-channels server:" +
                                 server_name()));
    CHECK(help.out.empty());
    const CliResult short_help = run_cli({"--channel", "-h"}, {});
    CHECK(short_help.exit_code == 0u);
    CHECK_THAT(short_help.err, ContainsSubstring("Usage: openwhisper-cpp-cli --channel"));

    const CliResult unexpected = run_cli({"--channel", "--port", "1"}, {});
    CHECK(unexpected.exit_code == 2u);
    CHECK_THAT(unexpected.err, ContainsSubstring("Unexpected arguments"));
}

TEST_CASE("test_frozen_builds_start_their_own_executable", "[test_automation_channel_protocol]") {
    const fs::path relative = fv::path_from_utf8(std::string(fv::identity().product) + ".exe");
    const std::vector<std::string> command = fv::agentapi::channel_command_line(relative);
    REQUIRE(command.size() == 2);
    CHECK(command[0] == fv::path_to_utf8(fs::absolute(relative)));
    CHECK(command[1] == "--channel");
    CHECK(fs::path(fv::path_from_utf8(command[0])).is_absolute());
}

TEST_CASE("test_mcp_json_entry_names_the_data_folder_only_when_it_is_unusual",
          "[test_automation_channel_protocol]") {
    fv::testkit::TempDataRoot root;
    const fs::path exe = root.path() / "app.exe";
    Json entry = fv::py::loads(fv::agentapi::claude_channel_config(
        exe, root.path(), root.path() / "standard"))["mcpServers"][server_name()];
    CHECK(entry["args"] == Json::array({"--channel"}));
    CHECK(entry["env"] == Json::object({{std::string(fv::identity().env_prefix) + "DATA_DIR",
                                         fv::path_to_utf8(root.path())}}));

    entry = fv::py::loads(fv::agentapi::claude_channel_config(
        exe, root.path(), root.path()))["mcpServers"][server_name()];
    CHECK_FALSE(entry.contains("env"));
    std::vector<std::string> keys;
    for (const auto& [key, value] : entry.items()) {
        keys.push_back(key);
    }
    std::sort(keys.begin(), keys.end());
    CHECK(keys == std::vector<std::string>{"args", "command"});
}

TEST_CASE("test_snippets_carry_no_token", "[test_automation_channel_protocol]") {
    fv::testkit::TempDataRoot root;
    // The snippets are built from paths alone; no credential store is read.
    const std::string text =
        fv::agentapi::claude_channel_config(root.path() / "app.exe", root.path(), root.path()) +
        fv::agentapi::claude_channel_command();
    CHECK_THAT(text, !ContainsSubstring(kToken));
    CHECK_THAT(text, !ContainsSubstring("Bearer"));
    CHECK(fv::agentapi::claude_channel_command() ==
          "claude --dangerously-load-development-channels server:" + server_name());
}
