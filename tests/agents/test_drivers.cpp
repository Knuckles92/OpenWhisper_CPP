// The Claude Code, Codex and OpenCode drivers (meeting/agent/installed/drivers.py): the tests of
// tests/test_installed_agent_drivers.py, with fv_fake_agent copied as claude.exe, codex.exe and
// opencode.exe where Python runs tests/fakes/fake_agent_cli.py and fake_acp_agent.py, and the
// driver's help probes canned where Python patches _help_text.

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <chrono>
#include <filesystem>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "agents/detail/driver_impl.h"
#include "agents/drivers.h"
#include "base/cancel.h"
#include "pass_support.h"
#include "pyport/json.h"

using fv::Json;
using fv::agents::AgentUnavailable;
using fv::agents::InstalledAgent;
using fv::agents::PassOutcome;
using fv::agents::server_name;
using fv::agents::tool_contract;
using fv::agents::detail::ClaudeCodeDriver;
using fv::agents::detail::CodexDriver;
using fv::agents::detail::OpenCodeDriver;
using fv::agents::detail::RunState;
using namespace fv::agents_test;

namespace {

bool contains(std::string_view text, std::string_view needle) {
    return text.find(needle) != std::string_view::npos;
}

Json read_json(const std::string& path) {
    return fv::py::loads(read_all(fv::path_from_utf8(path)));
}

// installed_agents() for the drivers' workspace and environment, with nothing that runs.
struct Offline : AgentsFixture {
    std::shared_ptr<fv::agents::InstalledAgents> agents = make();
    ScopedAgents scoped{agents};
    TempDir workdir;
};

// A help probe for Codex: `mcp list --json` gives `*inventory`, `features list` `features`, and
// anything else `help`.
fv::agents::detail::HelpText codex_help(std::shared_ptr<std::string> inventory,
                                        std::string features, std::string help) {
    return [inventory = std::move(inventory), features = std::move(features),
            help = std::move(help)](const InstalledAgent&, const std::vector<std::string>& args) {
        if (ends_mcp_list(args)) {
            return *inventory;
        }
        if (!args.empty() && args[0] == "features") {
            return features;
        }
        return help;
    };
}

std::string text_of_request(const std::vector<std::string>& argv) {
    std::string out;
    for (const auto& arg : argv) {
        out += arg + "\n";
    }
    return out;
}

} // namespace

// ---- tool contract ----

TEST_CASE("tool contract names tools as the agent sees them", "[test_installed_agent_drivers]") {
    // from
    // tests/test_installed_agent_drivers.py::test_tool_contract_names_tools_as_the_agent_sees_them
    const auto text = tool_contract(meeting_tools(), "OpenCode", server_name() + "_");
    CHECK(contains(text, server_name() + "_patch_state("));
    CHECK(contains(text, "Where these instructions name a tool"));
    CHECK(fv::py::rstrip(text).ends_with("reply with one short sentence."));
    const auto report = tool_contract(meeting_tools(), "Codex", "", "Reply with the report.");
    CHECK(contains(report, "- patch_state("));
    CHECK(fv::py::rstrip(report).ends_with("Reply with the report."));
}

// ---- headless command lines ----

TEST_CASE("claude command line", "[test_installed_agent_drivers]") {
    // from tests/test_installed_agent_drivers.py::test_claude_command_line
    Offline offline;
    ClaudeCodeDriver driver({"claude_code", "claude.exe", "2.1.281"}, offline.launcher,
                            canned_help(std::string(CLAUDE_HELP)));
    driver.check_ready();
    Recorder handler;
    const auto [argv, env] = driver.build(pass_request(handler.handler()),
                                          "http://127.0.0.1:1/mcp/t", "t", offline.workdir.path());
    CHECK(argv.at(0) == "claude.exe");
    CHECK(argv.at(1) == "-p");
    CHECK(after_flag(argv, "--tools").empty());
    CHECK(has_arg(argv, "--strict-mcp-config"));
    CHECK(has_arg(argv, "--no-session-persistence"));
    const auto allowed = fv::py::split_on(after_flag(argv, "--allowedTools"), ",");
    const auto prefix = "mcp__" + server_name() + "__";
    CHECK(std::find(allowed.begin(), allowed.end(), prefix + "patch_state") != allowed.end());
    for (const auto& tool : allowed) {
        CHECK(tool.starts_with(prefix));
    }
    CHECK(after_flag(argv, "--model") == "haiku");
    CHECK(read_json(after_flag(argv, "--settings")) ==
          Json{{"disableAllHooks", true}, {"alwaysThinkingEnabled", false}});
    const auto mcp = read_json(after_flag(argv, "--mcp-config"));
    CHECK(mcp.at("mcpServers").at(server_name()).at("headers").at("Authorization") == "Bearer t");
    CHECK(env.empty());
}

TEST_CASE("old claude code is refused", "[test_installed_agent_drivers]") {
    // from tests/test_installed_agent_drivers.py::test_old_claude_code_is_refused
    Offline offline;
    ClaudeCodeDriver driver({"claude_code", "claude", "1.0.0"}, offline.launcher,
                            canned_help("--mcp-config only"));
    try {
        driver.check_ready();
        FAIL("expected AgentUnavailable");
    } catch (const AgentUnavailable& error) {
        CHECK(contains(error.what(), "claude update"));
    }
}

TEST_CASE("codex command line", "[test_installed_agent_drivers]") {
    // from tests/test_installed_agent_drivers.py::test_codex_command_line
    Offline offline;
    const auto inventory = std::make_shared<std::string>(
        Json::array({Json{{"name", "other"}, {"enabled", true}},
                     Json{{"name", server_name()}, {"enabled", true}},
                     Json{{"name", "already_off"}, {"enabled", false}}})
            .dump());
    CodexDriver driver({"codex", "codex.exe", "0.158.0"}, offline.launcher,
                       codex_help(inventory,
                                  "shell_tool   stable   true\napps   stable   true\n"
                                  "sqlite   removed   true\nmemories  stable  false\n",
                                  "--json --ephemeral --disable"));
    driver.check_ready();
    Recorder handler;
    auto request = pass_request(handler.handler());
    request.model.clear();
    const auto [argv, env] =
        driver.build(request, "http://127.0.0.1:1/mcp/tok", "tok", offline.workdir.path());
    CHECK(std::vector<std::string>(argv.begin(), argv.begin() + 3) ==
          std::vector<std::string>{"codex.exe", "exec", "--json"});
    CHECK(argv.back() == "-");
    const auto overrides = values_of(argv, "-c");
    const auto has = [&](const std::string& value) {
        return std::find(overrides.begin(), overrides.end(), value) != overrides.end();
    };
    const auto server = server_name();
    CHECK(has("mcp_servers." + server + ".url=http://127.0.0.1:1/mcp/tok"));
    CHECK(has("mcp_servers." + server + ".default_tools_approval_mode=approve"));
    CHECK(has("approval_policy=never"));
    CHECK(has("project_doc_max_bytes=0"));
    CHECK(has("mcp_servers.other.enabled=false"));
    CHECK_FALSE(has("mcp_servers." + server + ".enabled=false"));
    // Only enabled, non-removed features are switched off; no quoted values.
    CHECK(values_of(argv, "--disable") == std::vector<std::string>{"shell_tool", "apps"});
    for (const auto& value : overrides) {
        CHECK_FALSE(contains(value, "\""));
    }
    CHECK_FALSE(has_arg(argv, "-m"));
    CHECK(env == std::map<std::string, std::string>{
                     {std::string(fv::identity().env_prefix) + "MCP_TOKEN", "tok"}});
}

TEST_CASE("codex refreshes mcp inventory before each pass", "[test_installed_agent_drivers]") {
    // from
    // tests/test_installed_agent_drivers.py::test_codex_refreshes_mcp_inventory_before_each_pass
    Offline offline;
    const auto inventory = std::make_shared<std::string>("[]");
    CodexDriver driver({"codex", "fake", "0.160.0"}, offline.launcher,
                       codex_help(inventory, "--json --disable", "--json --disable"));
    driver.check_ready();
    *inventory = R"([{"name": "added_during_meeting", "enabled": true}])";
    Recorder handler;
    const auto [argv, env] =
        driver.build(pass_request(handler.handler()), "http://127.0.0.1:1/mcp/tok", "tok",
                     offline.workdir.path());
    CHECK(has_arg(argv, "mcp_servers.added_during_meeting.enabled=false"));
}

TEST_CASE("codex refuses a pass when server isolation cannot be verified",
          "[test_installed_agent_drivers]") {
    // from
    // tests/test_installed_agent_drivers.py::test_codex_refuses_a_pass_when_server_isolation_cannot_be_verified
    const auto listing =
        GENERATE(as<std::string>{}, "", "not json", "{}", R"([{"name":"x.y","enabled":true}])");
    Offline offline;
    CodexDriver driver({"codex", "codex.exe", "0.160.0"}, offline.launcher,
                       codex_help(std::make_shared<std::string>(listing), "--json", "--json"));
    try {
        driver.check_ready();
        FAIL("expected AgentUnavailable");
    } catch (const AgentUnavailable& error) {
        CHECK(contains(error.what(), "MCP configuration"));
    }
}

TEST_CASE("codex errors show the providers message", "[test_installed_agent_drivers]") {
    // from tests/test_installed_agent_drivers.py::test_codex_errors_show_the_providers_message
    Offline offline;
    RunState state;
    CodexDriver driver({"codex", "codex", "0.158.0"}, offline.launcher, canned_help(""));
    Recorder handler;
    const Json inner{{"type", "error"},
                     {"status", 400},
                     {"error",
                      {{"message", "The 'x' model is not supported when using Codex with a "
                                   "ChatGPT account."}}}};
    driver.handle_event(
        Json{{"type", "turn.failed"}, {"error", {{"message", fv::py::dumps(inner)}}}}, state,
        pass_request(handler.handler()));
    const auto outcome = driver.finish(state, 1, "");
    CHECK(outcome.error ==
          "Codex: The 'x' model is not supported when using Codex with a ChatGPT account.");
}

TEST_CASE("claude fails fast when our tools did not connect", "[test_installed_agent_drivers]") {
    // from
    // tests/test_installed_agent_drivers.py::test_claude_fails_fast_when_our_tools_did_not_connect
    Offline offline;
    RunState state;
    ClaudeCodeDriver driver({"claude_code", "claude", "2.1.281"}, offline.launcher,
                            canned_help(""));
    Recorder handler;
    driver.handle_event(
        Json{{"type", "system"},
             {"subtype", "init"},
             {"mcp_servers", Json::array({Json{{"name", server_name()}, {"status", "failed"}}})}},
        state, pass_request(handler.handler()));
    CHECK(contains(state.fatal, "could not reach"));
}

TEST_CASE("claude reading a folder gets read fenced to it", "[test_installed_agent_drivers]") {
    // from
    // tests/test_installed_agent_drivers.py::test_claude_reading_a_folder_gets_read_fenced_to_it
    Offline offline;
    ClaudeCodeDriver driver({"claude_code", "claude.exe", "2.1.306"}, offline.launcher,
                            canned_help(std::string(CLAUDE_HELP) + " --restricted --add-dir"));
    driver.check_ready();
    CHECK(driver.supports_read_folder());
    Recorder handler;
    auto request = pass_request(handler.handler());
    request.tools = Json::array();
    request.read_folder = fv::path_from_utf8("C:\\captures\\run");
    const auto [argv, env] =
        driver.build(request, "http://127.0.0.1:1/mcp/t", "t", offline.workdir.path());
    // --restricted is what keeps Read inside the --add-dir folder.
    CHECK(has_arg(argv, "--restricted"));
    CHECK(after_flag(argv, "--tools") == "Read");
    CHECK(after_flag(argv, "--add-dir") == "C:\\captures\\run");
    CHECK_FALSE(has_arg(argv, "--allowedTools"));
    CHECK(has_arg(argv, "--strict-mcp-config"));
}

TEST_CASE("codex gets pictures with the prompt and still no shell",
          "[test_installed_agent_drivers]") {
    // from
    // tests/test_installed_agent_drivers.py::test_codex_gets_pictures_with_the_prompt_and_still_no_shell
    Offline offline;
    CodexDriver driver({"codex", "codex.exe", "0.163.0"}, offline.launcher,
                       codex_help(std::make_shared<std::string>("[]"),
                                  "shell_tool   stable   true\nview_image   stable   true\n",
                                  "--json --ephemeral"));
    driver.check_ready();
    Recorder handler;
    auto request = pass_request(handler.handler());
    request.tools = Json::array();
    request.images = {fv::path_from_utf8("C:\\run\\shot-1.png"),
                      fv::path_from_utf8("C:\\run\\shot-2.png")};
    const auto [argv, env] =
        driver.build(request, "http://127.0.0.1:1/mcp/t", "t", offline.workdir.path());
    // Each --image is followed by its file and then another flag, never by the "-" that reads
    // the prompt: --image takes several values.
    CHECK(std::vector<std::string>(argv.begin() + 3, argv.begin() + 7) ==
          std::vector<std::string>{"--image", "C:\\run\\shot-1.png", "--image",
                                   "C:\\run\\shot-2.png"});
    CHECK(argv.at(7).starts_with("-"));
    CHECK(argv.back() == "-");
    CHECK(argv.at(argv.size() - 2) != "C:\\run\\shot-2.png");
    CHECK(values_of(argv, "--disable") == std::vector<std::string>{"shell_tool", "view_image"});
}

TEST_CASE("a meeting pass reads no files", "[test_installed_agent_drivers]") {
    // from tests/test_installed_agent_drivers.py::test_a_meeting_pass_reads_no_files
    Offline offline;
    ClaudeCodeDriver driver({"claude_code", "claude.exe", "2.1.306"}, offline.launcher,
                            canned_help(std::string(CLAUDE_HELP) + " --restricted --add-dir"));
    driver.check_ready();
    Recorder handler;
    const auto [argv, env] = driver.build(pass_request(handler.handler()),
                                          "http://127.0.0.1:1/mcp/t", "t", offline.workdir.path());
    CHECK_FALSE(has_arg(argv, "--restricted"));
    CHECK_FALSE(has_arg(argv, "--add-dir"));
}

TEST_CASE("claude without restricted cannot read a folder", "[test_installed_agent_drivers]") {
    // from
    // tests/test_installed_agent_drivers.py::test_claude_without_restricted_cannot_read_a_folder
    Offline offline;
    ClaudeCodeDriver driver({"claude_code", "claude", "2.1.200"}, offline.launcher,
                            canned_help(std::string(CLAUDE_HELP) + " --add-dir"));
    driver.check_ready();
    CHECK_FALSE(driver.supports_read_folder());
}

TEST_CASE("a pass without tools does not need our server", "[test_installed_agent_drivers]") {
    // from
    // tests/test_installed_agent_drivers.py::test_a_pass_without_tools_does_not_need_our_server
    Offline offline;
    RunState state;
    ClaudeCodeDriver driver({"claude_code", "claude", "2.1.306"}, offline.launcher,
                            canned_help(""));
    Recorder handler;
    auto request = pass_request(handler.handler());
    request.tools = Json::array();
    driver.handle_event(
        Json{{"type", "system"}, {"subtype", "init"}, {"mcp_servers", Json::array()}}, state,
        request);
    CHECK(state.fatal.empty());
}

TEST_CASE("a reading pass's effort is passed only when --help lists it", "[drivers]") {
    Offline offline;
    Recorder handler;
    auto request = pass_request(handler.handler());
    request.tools = Json::array();
    request.effort = "medium";
    request.read_folder = offline.workdir.path();
    for (const auto& [help, expected] :
         {std::pair<std::string, bool>{std::string(CLAUDE_HELP) + " --restricted --add-dir", true},
          {"--mcp-config --strict-mcp-config --tools --system-prompt --restricted --add-dir",
           false}}) {
        ClaudeCodeDriver driver({"claude_code", "claude.exe", "2.1.306"}, offline.launcher,
                                canned_help(help));
        driver.check_ready();
        const auto [argv, env] =
            driver.build(request, "http://127.0.0.1:1/mcp/t", "t", offline.workdir.path());
        CHECK(has_arg(argv, "--effort") == expected);
        CHECK(after_flag(argv, "--output-format") == "stream-json");
        CHECK(has_arg(argv, "--verbose"));
        // A medium pass keeps the agent's thinking setting.
        CHECK(read_json(after_flag(argv, "--settings")) == Json{{"disableAllHooks", true}});
        if (expected) {
            CHECK(after_flag(argv, "--effort") == "medium");
            CHECK(read_all(fv::path_from_utf8(after_flag(argv, "--system-prompt-file"))) ==
                  "charter");
        } else {
            // Without --system-prompt-file the charter rides the command line.
            CHECK(after_flag(argv, "--system-prompt") == "charter");
        }
    }
}

TEST_CASE("the error texts of the three agents", "[drivers]") {
    using namespace fv::agents::detail;
    CHECK(claude_error("Not logged in \xC2\xB7 Please run /login") ==
          "Claude Code is not signed in. Run `claude` once and sign in.");
    CHECK(claude_error(std::string(400, 'x')) == "Claude Code: " + std::string(300, 'x'));
    CHECK(codex_error("unexpected status 401 Unauthorized") ==
          "Codex is not signed in. Run `codex login`.");
    CHECK(opencode_failed(fv::agents::AcpError("Provider auth required"), false).error ==
          "OpenCode needs a signed-in provider for this model. Run `opencode auth login`.");
    CHECK(opencode_failed(fv::agents::AcpError("boom"), false).error == "OpenCode: boom");
    const auto canceled = opencode_failed(fv::agents::AcpError("boom"), true);
    CHECK(canceled.canceled);
    CHECK(canceled.error == "canceled");
}

// ---- OpenCode, without a process ----

TEST_CASE("opencode overlay locks down both agents", "[test_installed_agent_drivers]") {
    // from tests/test_installed_agent_drivers.py::test_opencode_overlay_locks_down_both_agents
    const auto overlay = fv::agents::detail::opencode_overlay(false, {{"build", "COPILOT"}},
                                                              "http://127.0.0.1:5/mcp/tok", "tok");
    for (const auto* name : {"build", "plan"}) {
        const auto& rules = overlay.at("agents").at(name).at("permissions");
        CHECK(rules.front() == Json{{"action", "*"}, {"resource", "*"}, {"effect", "deny"}});
        CHECK(rules.back().at("action") == server_name() + "_*");
    }
    CHECK(contains(overlay.at("agents").at("build").at("system").get<std::string>(), "COPILOT"));
    const auto& mcp = overlay.at("mcp").at("servers").at(server_name());
    CHECK(mcp.at("codemode") == false);
    CHECK(mcp.at("type") == "remote");
}

TEST_CASE("opencode refuses every other permission", "[test_installed_agent_drivers]") {
    // from tests/test_installed_agent_drivers.py::test_opencode_refuses_every_other_permission
    using fv::agents::detail::opencode_request;
    const Json options = Json::array({Json{{"optionId", "a"}, {"kind", "allow_once"}},
                                      Json{{"optionId", "r"}, {"kind", "reject_once"}}});
    const auto ours = opencode_request(
        "session/request_permission",
        Json{{"toolCall", {{"title", server_name() + "_patch_state"}}}, {"options", options}});
    const auto bash =
        opencode_request("session/request_permission",
                         Json{{"toolCall", {{"title", "bash"}}}, {"options", options}});
    CHECK(ours.at("outcome").at("optionId") == "a");
    CHECK(bash.at("outcome").at("optionId") == "r");
    CHECK_THROWS_AS(opencode_request("fs/read_text_file", Json{{"path", "C:/secrets"}}),
                    fv::agents::AcpError);
}

// ---- headless passes, with fv_fake_agent ([platform]) ----

TEST_CASE("headless pass end to end", "[test_installed_agent_drivers][platform]") {
    // from tests/test_installed_agent_drivers.py::test_headless_pass_end_to_end
    FV_PASS_PROCESSES();
    const auto agent_id = GENERATE(as<std::string>{}, "claude_code", "codex");
    PassAgents fake;
    fake.tool_call(patch_call());
    std::unique_ptr<fv::agents::detail::HeadlessDriver> driver;
    if (agent_id == "claude_code") {
        driver = std::make_unique<ClaudeCodeDriver>(fake.claude, fake.launcher,
                                                    canned_help(std::string(CLAUDE_HELP)));
    } else {
        driver = std::make_unique<CodexDriver>(
            fake.codex, fake.launcher,
            codex_help(std::make_shared<std::string>("[]"), "",
                       std::string(CLAUDE_HELP) + " --json --ephemeral"));
    }
    driver->start(fake.server);
    std::vector<std::string> events;
    std::mutex mutex;
    Recorder handler;
    auto request = pass_request(handler.handler());
    request.on_event = [&](std::string_view kind, std::string_view) {
        std::lock_guard lock(mutex);
        events.emplace_back(kind);
    };
    const auto outcome = driver->run_pass(request);
    INFO(outcome.error);
    CHECK(outcome.ok);
    CHECK(handler.names() == std::vector<std::string>{"patch_state"});
    CHECK(outcome.usage.at("prompt_tokens").get<int>() > 0);
    CHECK(outcome.usage.at("completion_tokens").get<int>() > 0);
    {
        std::lock_guard lock(mutex);
        CHECK(std::find(events.begin(), events.end(), "tool") != events.end());
        REQUIRE_FALSE(events.empty());
        CHECK(events.back() == "settled");
    }
    // The pass's endpoint closed with it, and its folder is gone.
    CHECK(fake.server.endpoint_count() == 0);
    CHECK(fake.pass_folders_removed());
    // The agent ran in the workspace with the prompt on stdin.
    const auto seen = fake.recorded();
    CHECK(seen.at("prompt") == "transcript");
    std::error_code error;
    CHECK(std::filesystem::equivalent(fv::path_from_utf8(seen.at("cwd").get<std::string>()),
                                      fake.table->agent_workspace_dir(), error));
    if (agent_id == "codex") {
        CHECK(outcome.usage.at("cached_tokens") == 200);
        CHECK(seen.at("argv").back() == "-");
    } else {
        CHECK(outcome.usage.at("cost_usd") == 0.001);
        CHECK(seen.at("files").at("--system-prompt-file") == "charter");
    }
}

TEST_CASE("a signed out claude reads as a sign in hint",
          "[test_installed_agent_drivers][platform]") {
    // from tests/test_installed_agent_drivers.py::test_a_signed_out_claude_reads_as_a_sign_in_hint
    FV_PASS_PROCESSES();
    PassAgents fake;
    fake.mode("signed-out");
    ClaudeCodeDriver driver(fake.claude, fake.launcher, canned_help(std::string(CLAUDE_HELP)));
    driver.start(fake.server);
    Recorder handler;
    const auto outcome = driver.run_pass(pass_request(handler.handler()));
    CHECK_FALSE(outcome.ok);
    CHECK(contains(outcome.error, "not signed in"));
}

TEST_CASE("a run that lingers after its result still finishes",
          "[test_installed_agent_drivers][platform]") {
    // from
    // tests/test_installed_agent_drivers.py::test_a_run_that_lingers_after_its_result_still_finishes
    // Python shortens the 5 s grace to 0.5 s; here the grace is the real one.
    FV_PASS_PROCESSES();
    PassAgents fake;
    fake.mode("linger");
    fake.tool_call(patch_call());
    ClaudeCodeDriver driver(fake.claude, fake.launcher, canned_help(std::string(CLAUDE_HELP)));
    driver.start(fake.server);
    Recorder handler;
    const auto started = std::chrono::steady_clock::now();
    const auto outcome = driver.run_pass(pass_request(handler.handler()));
    const auto elapsed = std::chrono::steady_clock::now() - started;
    INFO(outcome.error);
    CHECK(outcome.ok);
    CHECK(elapsed >= fv::agents::detail::DONE_GRACE);
    CHECK(elapsed < std::chrono::seconds(20));
    // The helper that held stdout open is gone with the agent's tree.
    const auto pid = fake.helper();
    CHECK(pid != 0);
    CHECK(gone_soon(pid));
    CHECK(fake.pass_folders_removed());
}

TEST_CASE("a silent agent stalls and a cancel stops it",
          "[test_installed_agent_drivers][platform]") {
    // from tests/test_installed_agent_drivers.py::test_a_silent_agent_stalls_and_a_cancel_stops_it
    FV_PASS_PROCESSES();
    PassAgents fake;
    fake.mode("hang");
    ClaudeCodeDriver driver(fake.claude, fake.launcher, canned_help(std::string(CLAUDE_HELP)));
    driver.start(fake.server);
    Recorder handler;
    auto stalling = pass_request(handler.handler());
    stalling.stall_s = 1.5;
    const auto stalled = driver.run_pass(stalling);
    CHECK_FALSE(stalled.ok);
    CHECK(stalled.error == "Claude Code stalled for 2s without progress");
    if (const auto pid = fake.helper(); pid != 0) {
        CHECK(gone_soon(pid));
    }
    std::filesystem::remove(fake.helper_pid);

    fv::CancelSource cancel;
    auto canceling = pass_request(handler.handler());
    canceling.cancel = cancel.token();
    std::thread canceller([&] {
        std::this_thread::sleep_for(std::chrono::seconds(1));
        cancel.cancel();
    });
    const auto canceled = driver.run_pass(canceling);
    canceller.join();
    CHECK(canceled.canceled);
    CHECK(canceled.error == "canceled");
    if (const auto pid = fake.helper(); pid != 0) {
        CHECK(gone_soon(pid));
    }
    CHECK(fake.pass_folders_removed());
}

TEST_CASE("a pass past its wall is stopped with its helpers", "[drivers][platform]") {
    FV_PASS_PROCESSES();
    PassAgents fake;
    fake.mode("hang");
    ClaudeCodeDriver driver(fake.claude, fake.launcher, canned_help(std::string(CLAUDE_HELP)));
    driver.start(fake.server);
    Recorder handler;
    auto request = pass_request(handler.handler());
    request.timeout_s = 3;
    request.stall_s = std::nullopt;
    const auto started = std::chrono::steady_clock::now();
    const auto outcome = driver.run_pass(request);
    CHECK(std::chrono::steady_clock::now() - started < std::chrono::seconds(15));
    CHECK_FALSE(outcome.ok);
    CHECK(outcome.error == "Claude Code did not finish the pass within 3s");
    if (const auto pid = fake.helper(); pid != 0) {
        CHECK(gone_soon(pid));
    }
    CHECK(fake.pass_folders_removed());
}

TEST_CASE("a pass that ends early shows the last of its stderr", "[drivers][platform]") {
    FV_PASS_PROCESSES();
    PassAgents fake;
    fake.mode("crash");
    CodexDriver driver(fake.codex, fake.launcher,
                       codex_help(std::make_shared<std::string>("[]"), "", "--json"));
    driver.start(fake.server);
    Recorder handler;
    const auto outcome = driver.run_pass(pass_request(handler.handler()));
    CHECK_FALSE(outcome.ok);
    CHECK(outcome.error ==
          "Codex exited (code 2) before finishing the pass: error: unknown option '--frobnicate'");
    CHECK(fake.pass_folders_removed());
}

TEST_CASE("a pass's events and tool calls count as progress", "[drivers]") {
    Recorder handler;
    auto request = pass_request(handler.handler());
    int failed = 0;
    request.on_event = [&](std::string_view, std::string_view) {
        ++failed;
        throw std::runtime_error("callback broke");
    };
    const auto before = request.heartbeat->last();
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    // A callback that throws is logged, never the pass's failure.
    CHECK_NOTHROW(request.emit("thinking"));
    CHECK_NOTHROW(request.emit("writing"));
    CHECK(failed == 2);
    CHECK(request.heartbeat->last() > before);
    // Copies share the heartbeat: the pass's readers, its tool calls and the supervisor see one.
    const auto copy = request;
    CHECK(copy.heartbeat == request.heartbeat);

    std::vector<std::string> events;
    request.on_event = [&](std::string_view kind, std::string_view tool) {
        events.push_back(std::string(kind) + ":" + std::string(tool));
    };
    const auto answer = request.tracked_handler()("patch_state", Json::object(), Json::object());
    CHECK_FALSE(answer.second);
    CHECK(events == std::vector<std::string>{"tool:patch_state"});
    CHECK(handler.names() == std::vector<std::string>{"patch_state"});
}

TEST_CASE("a pass keeps 20 stderr lines of 400 characters", "[drivers]") {
    using fv::agents::detail::tail_detail;
    CHECK(tail_detail("first\n\n  \nlast line\n") == ": last line");
    CHECK(tail_detail("") == "");
    CHECK(tail_detail(std::string(300, 'z')) == ": " + std::string(240, 'z'));
    CHECK(fv::agents::detail::PASS_STDERR_TAIL == 20);
    CHECK(fv::agents::detail::PASS_STDERR_LINE_CHARS == 400);
}

TEST_CASE("codex's pictures reach it before the prompt", "[drivers][platform]") {
    FV_PASS_PROCESSES();
    PassAgents fake;
    const auto shot = fake.temp.path() / "shot 1.png";
    write_file(shot, "\x89PNG fake");
    CodexDriver driver(fake.codex, fake.launcher,
                       codex_help(std::make_shared<std::string>("[]"),
                                  "shell_tool stable true\nview_image stable true\n",
                                  "--json --ephemeral"));
    driver.start(fake.server);
    Recorder handler;
    auto request = pass_request(handler.handler());
    request.tools = Json::array();
    request.images = {shot, shot};
    fake.set("FV_FAKE_AGENT_RESULT", "# Write-up");
    const auto outcome = driver.run_pass(request);
    INFO(outcome.error);
    CHECK(outcome.ok);
    CHECK(outcome.text == "# Write-up");
    const auto seen = fake.recorded();
    CHECK(seen.at("images") == Json::array({fv::path_to_utf8(shot), fv::path_to_utf8(shot)}));
    CHECK(seen.at("image_sizes") == Json::array({9, 9}));
    CHECK(fake.pass_folders_removed());
}

// ---- OpenCode over ACP, with fv_fake_agent ([platform]) ----

TEST_CASE("opencode pass end to end", "[test_installed_agent_drivers][platform]") {
    // from tests/test_installed_agent_drivers.py::test_opencode_pass_end_to_end
    FV_PASS_PROCESSES();
    PassAgents fake;
    fake.tool_call(patch_call());
    Recorder handler;
    {
        OpenCodeDriver driver(fake.opencode, fake.launcher);
        driver.start(fake.server, {{"build", "COPILOT"}, {"plan", "NOTE TAKER"}}, meeting_tools());
        auto request = pass_request(handler.handler());
        request.model = "prov/fast";
        request.persona = "plan";
        request.system_prompt = "NOTE TAKER";
        const auto outcome = driver.run_pass(request);
        INFO(outcome.error);
        CHECK(outcome.ok);
        CHECK(outcome.text == "Done.");
        CHECK(outcome.usage == Json{{"prompt_tokens", 40},
                                    {"completion_tokens", 5},
                                    {"total_tokens", 45},
                                    {"requests", 1}});
        CHECK(handler.names() == std::vector<std::string>{"patch_state"});
        const auto calls = handler.calls();
        REQUIRE(calls.size() == 1);
        CHECK(std::get<2>(calls[0])
                  .at("ai.opencode/sessionID")
                  .get<std::string>()
                  .starts_with("ses_"));
        driver.close();
    }
    std::vector<Json> sets;
    for (const auto& params : fake.acp_requests("session/set_config_option")) {
        sets.push_back(Json{{"configId", params.at("configId")}, {"value", params.at("value")}});
    }
    const auto has = [&](const Json& wanted) {
        return std::find(sets.begin(), sets.end(), wanted) != sets.end();
    };
    CHECK(has(Json{{"configId", "mode"}, {"value", "plan"}}));
    CHECK(has(Json{{"configId", "model"}, {"value", "prov/fast"}}));
    CHECK(has(Json{{"configId", "effort"}, {"value", "low"}}));
    const auto prompts = fake.acp_requests("session/prompt");
    REQUIRE(prompts.size() == 1);
    // The charter lives in the agent's system prompt, never in the turn.
    CHECK(prompts[0].at("prompt") == Json::array({Json{{"type", "text"}, {"text", "transcript"}}}));
    // The warm-up session and the pass's session are both deleted.
    CHECK(fake.acp_requests("session/delete").size() == 2);
    // The process ran with the overlay and the update and file watcher off.
    const auto first = fake.acp().at(0);
    CHECK(first.at("project_config_off") == "1");
    CHECK(first.at("autoupdate_off") == "1");
    CHECK(first.at("filewatcher_off") == "1");
    CHECK(contains(first.at("overlay").at("agents").at("plan").at("system").get<std::string>(),
                   "NOTE TAKER"));
}

TEST_CASE("opencode tool free cleanup returns text without meeting tools",
          "[test_installed_agent_drivers][platform]") {
    // from
    // tests/test_installed_agent_drivers.py::test_opencode_tool_free_cleanup_returns_text_without_meeting_tools
    FV_PASS_PROCESSES();
    PassAgents fake;
    OpenCodeDriver driver(fake.opencode, fake.launcher);
    driver.start(fake.server, {{"build", "Return only the cleaned text."}}, Json::array());
    Recorder handler;
    auto request = pass_request(handler.handler());
    request.tools = Json::array();
    request.model.clear();
    const auto outcome = driver.run_pass(request);
    INFO(outcome.error);
    CHECK(outcome.ok);
    CHECK(outcome.text == "Done.");
    const auto overlay = driver.overlay();
    CHECK(driver.router()->tools == Json::array());
    const auto system = overlay.at("agents").at("build").at("system").get<std::string>();
    CHECK_FALSE(contains(system, "Act only through"));
    CHECK(contains(system, "Return only the cleaned text."));
    driver.close();
}

TEST_CASE("opencode rejects a model its providers lack",
          "[test_installed_agent_drivers][platform]") {
    // from tests/test_installed_agent_drivers.py::test_opencode_rejects_a_model_its_providers_lack
    FV_PASS_PROCESSES();
    PassAgents fake;
    PassOutcome outcome;
    {
        OpenCodeDriver driver(fake.opencode, fake.launcher);
        driver.start(fake.server, {{"build", "COPILOT"}}, meeting_tools());
        Recorder handler;
        auto request = pass_request(handler.handler());
        request.model = "gone/model";
        outcome = driver.run_pass(request);
        driver.close();
    }
    CHECK_FALSE(outcome.ok);
    CHECK(contains(outcome.error, "no model 'gone/model'"));
}

TEST_CASE("opencode gets pictures as image blocks", "[test_installed_agent_drivers][platform]") {
    // from tests/test_installed_agent_drivers.py::test_opencode_gets_pictures_as_image_blocks
    FV_PASS_PROCESSES();
    PassAgents fake;
    const auto shot = fake.temp.path() / "shot-1.png";
    write_file(shot, "\x89PNG fake");
    PassOutcome outcome;
    {
        OpenCodeDriver driver(fake.opencode, fake.launcher);
        driver.start(fake.server, {{"build", "WRITER"}}, Json::array());
        Recorder handler;
        auto request = pass_request(handler.handler());
        request.tools = Json::array();
        request.model.clear();
        request.images = {shot};
        outcome = driver.run_pass(request);
        driver.close();
    }
    INFO(outcome.error);
    CHECK(outcome.ok);
    const auto prompts = fake.acp_requests("session/prompt");
    REQUIRE(prompts.size() == 1);
    CHECK(prompts[0].at("prompt") ==
          Json::array(
              {Json{{"type", "text"}, {"text", "transcript"}},
               Json{{"type", "image"}, {"mimeType", "image/png"}, {"data", "iVBORyBmYWtl"}}}));
}

TEST_CASE("an opencode that takes no pictures refuses them",
          "[test_installed_agent_drivers][platform]") {
    // from
    // tests/test_installed_agent_drivers.py::test_an_opencode_that_takes_no_pictures_refuses_them
    FV_PASS_PROCESSES();
    PassAgents fake;
    fake.set("FV_FAKE_AGENT_NO_IMAGES", "1");
    PassOutcome outcome;
    {
        OpenCodeDriver driver({"opencode_cli", fake.opencode.path, "1.0.0"}, fake.launcher);
        driver.start(fake.server, {{"build", "WRITER"}}, Json::array());
        Recorder handler;
        auto request = pass_request(handler.handler());
        request.tools = Json::array();
        request.images = {fake.temp.path() / "x.png"};
        outcome = driver.run_pass(request);
        driver.close();
    }
    CHECK_FALSE(outcome.ok);
    CHECK(outcome.error == "OpenCode 1.0.0 can't be sent pictures. Update OpenCode.");
    CHECK(fake.acp_requests("session/prompt").empty());
}

TEST_CASE("a closed opencode driver lets go of its tool server", "[drivers][platform]") {
    FV_PASS_PROCESSES();
    PassAgents fake;
    fv::net::Reactor reactor{2};
    auto server = std::make_unique<fv::agents::LoopbackMcpServer>(reactor);
    server->start();
    OpenCodeDriver driver(fake.opencode, fake.launcher);
    driver.start(*server, {{"build", "WRITER"}}, Json::array());
    CHECK(server->endpoint_count() == 1);
    driver.close();
    CHECK(server->endpoint_count() == 0);
    CHECK(driver.router() == nullptr);
    CHECK_FALSE(driver.healthy());
    // The owner may destroy the server once the driver is closed: the driver's destructor closes
    // it again and must not reach the server.
    server.reset();
}
