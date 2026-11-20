// Asking an installed agent with no tools (services/actions/agents.py ask), from
// tests/test_automation_agents_ask.py, and the review fixes of
// tests/test_automation_fix_actions_agents.py (Claude's verbose JSON, npm shims, the .exe for
// terminals). No real agent runs: the command lines are built against help listings a scripted
// launcher gives, and the [platform] tests run fv_fake_agent copied as claude.exe, codex.exe and
// opencode.exe.

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <thread>
#include <vector>

#include "agent_actions_support.h"
#include "agents/agent_actions.h"
#include "agents/detail/headless.h"
#include "agents/detail/shim.h"
#include "base/cancel.h"
#include "base/identity.h"
#include "base/json.h"
#include "base/utf8.h"
#include "process/launcher.h"
#include "pyport/json.h"
#include "pyport/text.h"

#ifdef _WIN32
#include <windows.h>
#endif

using namespace fv;
using namespace fv::agents_test;
using agents::detail::HeadlessRun;

namespace {

const std::string WORDS = "What's 2+2? \xC3\x9Cn\xC3\xAF"
                          "c\xC3\xB6"
                          "d\xC3\xA9 \xE6\x97\xA5\xE6\x9C\xAC \"quoted\" & | ; --help\nsecond line";

constexpr std::string_view CLAUDE_HELP =
    "--tools <tools...> --no-session-persistence --disable-slash-commands --model";
constexpr std::string_view CODEX_EXEC_HELP =
    "--json --ephemeral --ignore-user-config --skip-git-repo-check";
constexpr std::string_view CODEX_FEATURES = "shell_tool    stable   true\n"
                                            "apps          stable   false\n"
                                            "code_mode_host stable  true\n"
                                            "js_repl       removed  false\n"
                                            "fast_mode     stable   true\n";

std::string read_text(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

std::ptrdiff_t index_of(const std::vector<std::string>& argv, std::string_view arg) {
    const auto it = std::find(argv.begin(), argv.end(), arg);
    REQUIRE(it != argv.end());
    return it - argv.begin();
}

const std::string& after(const std::vector<std::string>& argv, std::string_view arg) {
    return argv.at(static_cast<std::size_t>(index_of(argv, arg)) + 1);
}

bool has(const std::vector<std::string>& argv, std::string_view arg) {
    return std::find(argv.begin(), argv.end(), arg) != argv.end();
}

std::vector<std::string> values_after(const std::vector<std::string>& argv, std::string_view flag) {
    std::vector<std::string> out;
    for (std::size_t i = 0; i + 1 < argv.size(); ++i) {
        if (argv[i] == flag) {
            out.push_back(argv[i + 1]);
        }
    }
    return out;
}

bool mentions_question(const std::vector<std::string>& argv) {
    return std::any_of(argv.begin(), argv.end(),
                       [](const std::string& arg) { return arg.find("2+2") != std::string::npos; });
}

// A folder of its own for one command line's files.
std::filesystem::path workdir(const ActionsFixture& fixture) {
    const auto path = fixture.temp.path() / "work";
    std::filesystem::create_directories(path);
    return path;
}

// Help listings of the scripted agents, as the Python tests patch _help.
void script_help(ActionsFixture& fixture, std::string_view claude_help = CLAUDE_HELP,
                 std::string_view exec_help = CODEX_EXEC_HELP,
                 std::string_view features = CODEX_FEATURES) {
    agents::detail::forget_help();
    fixture.launcher->reply(fixture.claude.path, {"--help"}, Reply{0, std::string(claude_help)});
    fixture.launcher->reply(fixture.codex.path, {"exec", "--help"},
                            Reply{0, std::string(exec_help)});
    fixture.launcher->reply(fixture.codex.path, {"features", "list"},
                            Reply{0, std::string(features)});
}

// The user's Codex default model, in config.toml under the fixture's CODEX_HOME.
void codex_default(ActionsFixture& fixture, std::string_view model) {
    write_file(fixture.temp.path() / "codex-home" / "config.toml",
               "model = \"" + std::string(model) + "\"\n");
}

bool started_ask(const ActionsFixture& fixture) {
    const auto specs = fixture.launcher->specs();
    return std::any_of(specs.begin(), specs.end(), [](const ProcessSpec& spec) {
        return !spec.args.empty() &&
               (spec.args[0] == "-p" || (spec.args.size() > 1 && spec.args[1] == "--json"));
    });
}

HeadlessRun done(std::string stdout_text, std::string stderr_text = {}, int code = 0) {
    return HeadlessRun{.status = "done",
                       .returncode = code,
                       .stdout_text = std::move(stdout_text),
                       .stderr_text = std::move(stderr_text)};
}

} // namespace

// ---- command lines ----

TEST_CASE("claude command line has no tools and keeps the words off it",
          "[test_automation_agents_ask]") {
    // from
    // tests/test_automation_agents_ask.py::test_claude_command_line_has_no_tools_and_keeps_the_words_off_it
    ActionsFixture fixture;
    script_help(fixture);
    const auto argv = agents::detail::claude_ask_command(*fixture.table, fixture.claude, "haiku",
                                                         workdir(fixture));
    CHECK(std::vector<std::string>(argv.begin(), argv.begin() + 4) ==
          std::vector<std::string>{path_to_utf8(fixture.claude.path), "-p", "--output-format",
                                   "json"});
    CHECK(after(argv, "--system-prompt") == agents::ASK_INSTRUCTION);
    CHECK(after(argv, "--tools").empty());
    CHECK(has(argv, "--strict-mcp-config"));
    CHECK(py::loads(read_text(path_from_utf8(after(argv, "--mcp-config")))) ==
          Json{{"mcpServers", Json::object()}});
    CHECK(py::loads(read_text(path_from_utf8(after(argv, "--settings")))) ==
          Json{{"disableAllHooks", true}, {"alwaysThinkingEnabled", false}});
    CHECK(has(argv, "--no-session-persistence"));
    CHECK(has(argv, "--disable-slash-commands"));
    CHECK(after(argv, "--model") == "haiku");
    CHECK_FALSE(has(argv, "--bare"));
    CHECK_FALSE(has(argv, "--safe-mode"));
    CHECK_FALSE(mentions_question(argv));
}

TEST_CASE("claude command line follows its help", "[test_automation_agents_ask]") {
    // from tests/test_automation_agents_ask.py::test_claude_command_line_follows_its_help
    ActionsFixture fixture;
    script_help(fixture, "--tools --model");
    const auto argv =
        agents::detail::claude_ask_command(*fixture.table, fixture.claude, "", workdir(fixture));
    CHECK_FALSE(has(argv, "--no-session-persistence"));
    CHECK_FALSE(has(argv, "--model"));
    script_help(fixture, "--print --model");
    try {
        agents::detail::claude_ask_command(*fixture.table, fixture.claude, "", workdir(fixture));
        FAIL_CHECK("an old Claude Code was not refused");
    } catch (const agents::detail::TooOld& error) {
        CHECK(std::string(error.what()).find("claude update") != std::string::npos);
    }
}

TEST_CASE("codex command line ignores user config and switches tools off",
          "[test_automation_agents_ask]") {
    // from
    // tests/test_automation_agents_ask.py::test_codex_command_line_ignores_user_config_and_switches_tools_off
    ActionsFixture fixture;
    script_help(fixture);
    codex_default(fixture, "gpt-6-sol");
    const auto argv =
        agents::detail::codex_ask_command(*fixture.table, fixture.codex, "", workdir(fixture));
    CHECK(std::vector<std::string>(argv.begin(), argv.begin() + 3) ==
          std::vector<std::string>{path_to_utf8(fixture.codex.path), "exec", "--json"});
    CHECK(argv.back() == "-");
    for (const auto* flag : {"--ephemeral", "--skip-git-repo-check", "--ignore-user-config"}) {
        CHECK(has(argv, flag));
    }
    CHECK(after(argv, "-s") == "read-only");
    CHECK(after(argv, "-C") == path_to_utf8(fixture.table->agent_workspace_dir()));
    const auto overrides = values_after(argv, "-c");
    for (const auto* expected :
         {"approval_policy=never", "web_search=disabled", "project_doc_max_bytes=0"}) {
        CHECK(std::find(overrides.begin(), overrides.end(), expected) != overrides.end());
    }
    const auto instructions = std::find_if(overrides.begin(), overrides.end(), [](const auto& o) {
        return o.starts_with("model_instructions_file=");
    });
    REQUIRE(instructions != overrides.end());
    const auto path = instructions->substr(instructions->find('=') + 1);
    CHECK(read_text(path_from_utf8(path)) == agents::ASK_INSTRUCTION);
    CHECK(path.find('\\') == std::string::npos);
    // Every tool feature this Codex knows is off, enabled or not; unknown names would stop the
    // run.
    CHECK(values_after(argv, "--disable") ==
          std::vector<std::string>{"shell_tool", "code_mode_host", "apps", "js_repl"});
    CHECK(after(argv, "-m") == "gpt-6-sol"); // the user's default, since config is ignored
}

TEST_CASE("codex without a feature list still drops the shell", "[test_automation_agents_ask]") {
    // from
    // tests/test_automation_agents_ask.py::test_codex_without_a_feature_list_still_drops_the_shell
    const auto listing =
        GENERATE(std::string(""), std::string("Error: config.toml line 3: expected a value"));
    ActionsFixture fixture;
    script_help(fixture, CLAUDE_HELP, CODEX_EXEC_HELP, listing);
    const auto argv = agents::detail::codex_ask_command(*fixture.table, fixture.codex, "gpt-6-luna",
                                                        workdir(fixture));
    CHECK(values_after(argv, "--disable") == std::vector<std::string>{"shell_tool"});
    CHECK(after(argv, "-m") == "gpt-6-luna");
}

TEST_CASE("an old codex is refused before it runs", "[test_automation_agents_ask]") {
    // from tests/test_automation_agents_ask.py::test_an_old_codex_is_refused_before_it_runs
    ActionsFixture fixture;
    script_help(fixture, CLAUDE_HELP, "--json --ephemeral");
    const auto result = agents::ask("codex", WORDS);
    CHECK_FALSE(result.ok);
    CHECK(result.message.find("too old") != std::string::npos);
    CHECK_FALSE(started_ask(fixture));
}

TEST_CASE("refusals run nothing", "[test_automation_agents_ask]") {
    // from tests/test_automation_agents_ask.py::test_refusals_run_nothing
    struct Row {
        std::string agent_id, text, model, message;
    };
    const auto row = GENERATE(Catch::Generators::values<Row>({
        {"pi", WORDS, "", "Choose Claude Code, Codex or OpenCode"},
        {"claude_code", " ", "", "nothing to ask Claude Code"},
        {"claude_code", WORDS, "haiku&calc", "model name can't be used"},
    }));
    ActionsFixture fixture;
    const auto result = agents::ask(row.agent_id, row.text, row.model);
    CHECK_FALSE(result.ok);
    CHECK(result.message.find(row.message) != std::string::npos);
    CHECK(fixture.launcher->specs().empty());
}

TEST_CASE("ask names a missing agent", "[test_automation_agents_ask]") {
    // from tests/test_automation_agents_ask.py::test_a_missing_agent_is_named
    ActionsFixture fixture;
    fixture.table->clear();
    CHECK(agents::ask("codex", WORDS).message == "Codex isn't installed.");
}

TEST_CASE("a missing program reads as not started", "[test_automation_agents_ask]") {
    // from tests/test_automation_agents_ask.py::test_a_missing_program_reads_as_not_started
    // The scripted launcher starts nothing it has no script for, as for a program that is gone.
    ActionsFixture fixture;
    fixture.table->set({"claude_code", fixture.temp.path() / "gone.exe", "2.1.296"});
    CHECK(agents::ask("claude_code", WORDS).message == "Claude Code couldn't be started.");
}

TEST_CASE("opencode that cannot start says so", "[test_automation_agents_ask]") {
    // from tests/test_automation_agents_ask.py::test_opencode_that_cannot_start_says_so
    ActionsFixture fixture;
    fixture.table->set({"opencode_cli", fixture.temp.path() / "gone.exe", "2.0.18"});
    CHECK(agents::ask("opencode_cli", WORDS).message == "OpenCode couldn't be started.");
}

// ---- what the agents print (test_automation_fix_actions_agents.py) ----

namespace {

const Json RESULT{{"type", "result"},
                  {"subtype", "success"},
                  {"is_error", false},
                  {"result", "Lima."},
                  {"session_id", "x"}};

} // namespace

TEST_CASE("claude answers from either json shape", "[test_automation_fix_actions_agents]") {
    // from tests/test_automation_fix_actions_agents.py::test_claude_answers_from_either_json_shape
    const auto stdout_text = GENERATE(
        py::dumps(RESULT),
        // What Claude Code prints when verbose is on in the user's config: every message of the
        // turn as one JSON array.
        py::dumps(Json::array(
            {Json{{"type", "system"}, {"subtype", "init"}, {"session_id", "x"}},
             Json{{"type", "assistant"},
                  {"message",
                   {{"content", Json::array({Json{{"type", "text"}, {"text", "Lima."}}})}}}},
             RESULT})),
        // Pretty-printed across lines.
        py::dumps(Json::array({Json{{"type", "system"}, {"subtype", "init"}}, RESULT}),
                  {.indent = 2}));
    const auto result = agents::detail::claude_answer(done(stdout_text + "\n"));
    CHECK(result.ok);
    CHECK(result.answer == "Lima.");
}

TEST_CASE("claude verbose errors are reported", "[test_automation_fix_actions_agents]") {
    // from tests/test_automation_fix_actions_agents.py::test_claude_verbose_errors_are_reported
    auto failed = RESULT;
    failed["subtype"] = "error_max_turns";
    failed["is_error"] = true;
    failed["result"] = "";
    const auto stdout_text =
        py::dumps(Json::array({Json{{"type", "system"}, {"subtype", "init"}}, failed}));
    const auto result = agents::detail::claude_answer(done(stdout_text, "", 1));
    CHECK_FALSE(result.ok);
    CHECK(result.message == "Claude Code couldn't answer: error_max_turns");
}

TEST_CASE("claude output without a result says it stopped",
          "[test_automation_fix_actions_agents]") {
    // from
    // tests/test_automation_fix_actions_agents.py::test_claude_output_without_a_result_says_it_stopped
    const auto stdout_text =
        py::dumps(Json::array({Json{{"type", "system"}, {"subtype", "init"}}}));
    const auto result = agents::detail::claude_answer(done(stdout_text, "boom", 1));
    CHECK_FALSE(result.ok);
    CHECK(result.message == "Claude Code stopped before answering: boom");
}

// ---- npm shims (test_automation_fix_actions_agents.py) ----

namespace {

// What `npm install -g @openai/codex` writes as codex.cmd (npm's cmd-shim).
const std::string CODEX_SHIM =
    "@ECHO off\r\nGOTO start\r\n:find_dp0\r\nSET dp0=%~dp0\r\nEXIT /b\r\n:start\r\nSETLOCAL\r\n"
    "CALL :find_dp0\r\n\r\nIF EXIST \"%dp0%\\node.exe\" (\r\n  SET \"_prog=%dp0%\\node.exe\"\r\n"
    ") ELSE (\r\n  SET \"_prog=node\"\r\n)\r\n\r\n"
    "endLocal & goto #_undefined_# 2>NUL || title %COMSPEC% & set PATHEXT=%PATHEXT:;.JS;=;% & "
    "\"%_prog%\"  \"%dp0%\\node_modules\\@openai\\codex\\bin\\codex.js\" %*\r\n";

const std::filesystem::path CODEX_JS =
    std::filesystem::path("node_modules") / "@openai" / "codex" / "bin" / "codex.js";
const std::filesystem::path CODEX_EXE = std::filesystem::path("node_modules") / "@openai" /
                                        "codex" / "node_modules" / "@openai" / "codex-win32-x64" /
                                        "vendor" / "x86_64-pc-windows-msvc" / "bin" / "codex.exe";

struct NpmLayout {
    bool node = true;
    bool script = true;
    bool exe = false;
};

// An npm global folder holding Codex the way npm installs it on Windows.
std::filesystem::path npm(const std::filesystem::path& root, NpmLayout layout = {}) {
    const auto folder = root / "npm";
    std::filesystem::create_directories(folder);
    write_file(folder / "codex.cmd", CODEX_SHIM);
    const auto package =
        std::filesystem::path("node_modules") / "@openai" / "codex" / "package.json";
    for (const auto& [wanted, relative] :
         {std::pair{layout.node, std::filesystem::path("node.exe")},
          std::pair{layout.script, CODEX_JS}, std::pair{layout.exe, CODEX_EXE},
          std::pair{true, package}}) {
        if (wanted) {
            write_file(folder / relative, "");
        }
    }
    return folder;
}

std::string text(const std::filesystem::path& path) {
    return path_to_utf8(path.lexically_normal().make_preferred());
}

std::string replace_all(std::string text, std::string_view from, std::string_view to) {
    for (auto at = text.find(from); at != std::string::npos; at = text.find(from, at + to.size())) {
        text.replace(at, from.size(), to);
    }
    return text;
}

} // namespace

TEST_CASE("a shim runs its script with the node beside it",
          "[test_automation_fix_actions_agents]") {
    // from
    // tests/test_automation_fix_actions_agents.py::test_a_shim_runs_its_script_with_the_node_beside_it
    TempDir temp;
    const auto folder = npm(temp.path());
    CHECK(agents::detail::npm_shim_command(folder / "codex.cmd", std::nullopt) ==
          std::vector<std::string>{text(folder / "node.exe"), text(folder / CODEX_JS)});
}

TEST_CASE("a shim runs its script with node from path", "[test_automation_fix_actions_agents]") {
    // from
    // tests/test_automation_fix_actions_agents.py::test_a_shim_runs_its_script_with_node_from_path
    TempDir temp;
    const auto folder = npm(temp.path(), {.node = false});
    const auto node = temp.path() / "nodejs" / "node.exe";
    write_file(node, "");
    CHECK(
        agents::detail::npm_shim_command(folder / "codex.cmd", path_to_utf8(node.parent_path())) ==
        std::vector<std::string>{path_to_utf8(node), text(folder / CODEX_JS)});
}

TEST_CASE("without node the packages own exe runs", "[test_automation_fix_actions_agents]") {
    // from tests/test_automation_fix_actions_agents.py::test_without_node_the_packages_own_exe_runs
    TempDir temp;
    const auto folder = npm(temp.path(), {.node = false, .exe = true});
    CHECK(agents::detail::npm_shim_command(folder / "codex.cmd", std::nullopt) ==
          std::vector<std::string>{text(folder / CODEX_EXE)});
}

TEST_CASE("a shim that cannot be resolved is left alone", "[test_automation_fix_actions_agents]") {
    // from
    // tests/test_automation_fix_actions_agents.py::test_a_shim_that_cannot_be_resolved_is_left_alone
    struct Row {
        NpmLayout layout;
        std::string source;
    };
    const auto row = GENERATE(Catch::Generators::values<Row>({
        {{.node = false}, CODEX_SHIM},                         // no-node
        {{.script = false}, CODEX_SHIM},                       // no-script
        {{}, "@ECHO off\r\npowershell -File evil.ps1 %*\r\n"}, // not-npm
        {{},
         replace_all(replace_all(CODEX_SHIM, "node.exe", "sh.exe"), "\"_prog=node\"",
                     "\"_prog=sh\"")}, // not-node
    }));
    TempDir temp;
    const auto folder = npm(temp.path(), row.layout);
    write_file(folder / "codex.cmd", row.source);
    CHECK_FALSE(agents::detail::npm_shim_command(folder / "codex.cmd", std::nullopt).has_value());
}

TEST_CASE("a handoff through an npm shim starts node with the script",
          "[test_automation_fix_actions_agents]") {
    // from
    // tests/test_automation_fix_actions_agents.py::test_a_handoff_through_an_npm_shim_starts_node_with_the_script
    ActionsFixture fixture;
    const auto folder = npm(fixture.temp.path());
    const auto shim = folder / "codex.cmd";
    fixture.table->set({"codex", shim, "0.162.1"});
    // The shim is the only candidate (the user's PATH holds its folder alone), and no node.exe
    // is on that PATH.
    fixture.env->insert_or_assign("PATH", path_to_utf8(folder));
    Desk desk;
    CHECK(agents::handoff(
              {.agent_id = "codex", .text = "fix the build & ship", .surface = "terminal"}) ==
          "Opened Codex in a terminal");
    const auto terminals = desk.terminals();
    REQUIRE(terminals.size() == 1);
    CHECK(terminals[0].argv == std::vector<std::string>{text(folder / "node.exe"),
                                                        text(folder / CODEX_JS), "--",
                                                        "fix the build & ship"});
}

TEST_CASE("a terminal prefers an installed exe over a newer npm shim",
          "[test_automation_fix_actions_agents]") {
    // from
    // tests/test_automation_fix_actions_agents.py::test_a_terminal_prefers_an_installed_exe_over_a_newer_npm_shim
#ifndef _WIN32
    SKIP("Codex's .exe install folders are looked for on Windows only");
#else
    ActionsFixture fixture;
    const auto folder = npm(fixture.temp.path());
    const auto shim = folder / "codex.cmd";
    const auto exe =
        make_program(fixture.local / "Programs" / "OpenAI" / "Codex" / "bin" / "codex.exe");
    fixture.table->set({"codex", shim, "0.162.1"}); // the newest one
    fixture.env->insert_or_assign("PATH", path_to_utf8(folder));
    REQUIRE(fixture.table->candidate_paths("codex").size() == 2);
    Desk desk;
    agents::handoff({.agent_id = "codex", .text = "hello there", .surface = "terminal"});
    CHECK(desk.terminals().at(0).argv == std::vector<std::string>{text(exe), "--", "hello there"});
#endif
}

TEST_CASE("an unresolvable shim reaches the terminal refusal",
          "[test_automation_fix_actions_agents]") {
    // from
    // tests/test_automation_fix_actions_agents.py::test_an_unresolvable_shim_reaches_the_terminal_refusal
    // The terminal backend records what it was asked to run; open_in_terminal itself refuses a
    // .cmd, so the refusal is what reaches the user.
    ActionsFixture fixture;
    const auto folder = npm(fixture.temp.path(), {.node = false});
    const auto shim = folder / "codex.cmd";
    fixture.table->set({"codex", shim, "0.162.1"});
    fixture.env->insert_or_assign("PATH", path_to_utf8(folder));
    CHECK(agents::terminal_command(fixture.table->find_agent("codex").value()) ==
          std::vector<std::string>{path_to_utf8(shim)});
    Desk desk;
    try {
        agents::handoff({.agent_id = "codex", .text = "hello there", .surface = "terminal"});
        FAIL_CHECK("the shim was opened");
    } catch (const agents::HandoffError& error) {
        CHECK(std::string(error.what()).starts_with("codex.cmd is a script"));
    }
    CHECK(desk.terminals().empty());
}

// ---- headless runs and OpenCode, with fv_fake_agent ([platform]) ----

namespace {

#ifdef _WIN32
bool alive(unsigned long pid) {
    HANDLE handle = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (handle == nullptr) {
        return false;
    }
    DWORD code = 0;
    const bool running = GetExitCodeProcess(handle, &code) && code == STILL_ACTIVE;
    CloseHandle(handle);
    return running;
}

void kill(unsigned long pid) {
    if (HANDLE handle = OpenProcess(PROCESS_TERMINATE, FALSE, pid)) {
        TerminateProcess(handle, 1);
        CloseHandle(handle);
    }
}
#else
bool alive(unsigned long) {
    return false;
}
void kill(unsigned long) {}
#endif

bool wait_dead(unsigned long pid, std::chrono::seconds limit = std::chrono::seconds(5)) {
    const auto until = std::chrono::steady_clock::now() + limit;
    while (alive(pid) && std::chrono::steady_clock::now() < until) {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    return !alive(pid);
}

// The three agents as fv_fake_agent copies, run by the real launcher, with the files they write
// in the fixture's folder.
struct FakeAgents {
    TempDir temp;
    std::filesystem::path bin = temp.path() / "bin";
    std::filesystem::path tmp = temp.path() / "tmp";
    std::filesystem::path record = temp.path() / "record.json";
    std::filesystem::path helper_pid = temp.path() / "helper.pid";
    std::filesystem::path acp_log = temp.path() / "acp.jsonl";
    std::shared_ptr<std::map<std::string, std::string>> env =
        std::make_shared<std::map<std::string, std::string>>();
    std::shared_ptr<TableAgents> table;
    std::unique_ptr<ScopedAgents> scoped;

    FakeAgents() {
        std::filesystem::create_directories(bin);
        std::filesystem::create_directories(tmp);
        for (const auto* name : {"claude", "codex", "opencode"}) {
            std::filesystem::copy_file(FV_FAKE_AGENT, exe(bin, name));
        }
        const auto home = temp.path() / "home";
        std::filesystem::create_directories(home);
        *env = {
            {"USERPROFILE", path_to_utf8(home)},
            {"HOME", path_to_utf8(home)},
            {"LOCALAPPDATA", path_to_utf8(temp.path() / "local")},
            {"APPDATA", path_to_utf8(temp.path() / "roaming")},
            {"CODEX_HOME", path_to_utf8(temp.path() / "codex-home")},
            {"CLAUDE_CONFIG_DIR", path_to_utf8(temp.path() / "claude-config")},
            {"XDG_CONFIG_HOME", path_to_utf8(temp.path() / "xdg")},
            {"XDG_DATA_HOME", path_to_utf8(temp.path() / "xdg-data")},
            {"PATH", path_to_utf8(bin)},
            {"FV_FAKE_AGENT_RECORD", path_to_utf8(record)},
            {"FV_FAKE_AGENT_HELPER_PID", path_to_utf8(helper_pid)},
            {"FV_FAKE_AGENT_ACP_LOG", path_to_utf8(acp_log)},
        };
        // Windows programs expect these two.
        for (const auto* name : {"SystemRoot", "SYSTEMDRIVE"}) {
            if (const auto value = env_utf8(name)) {
                env->insert_or_assign(name, *value);
            }
        }
        table = std::make_shared<TableAgents>(
            make_process_launcher(), [env = env] { return *env; }, home, tmp);
        table->set({"claude_code", exe(bin, "claude"), "2.1.296"});
        table->set({"codex", exe(bin, "codex"), "0.162.1"});
        table->set({"opencode_cli", exe(bin, "opencode"), "2.0.18"});
        scoped = std::make_unique<ScopedAgents>(table);
        agents::detail::forget_help();
    }
    ~FakeAgents() {
        if (const auto pid = helper(); pid != 0 && alive(pid)) {
            kill(pid); // a test that failed may leave the helper; never leave it running
        }
    }

    void mode(const std::string& value) { env->insert_or_assign("FV_FAKE_AGENT_MODE", value); }

    Json recorded() const { return py::loads(read_text(record)); }
    std::vector<Json> acp() const {
        std::vector<Json> entries;
        for (const auto& line : py::split_on(read_text(acp_log), "\n")) {
            if (!line.empty()) {
                entries.push_back(py::loads(line));
            }
        }
        return entries;
    }
    std::vector<std::string> acp_methods(std::size_t from = 0) const {
        std::vector<std::string> methods;
        const auto entries = acp();
        for (std::size_t i = from; i < entries.size(); ++i) {
            const auto& method = entries[i].value("method", Json());
            methods.push_back(method.is_string() ? method.get<std::string>() : "<none>");
        }
        return methods;
    }
    unsigned long helper() const {
        const auto text = read_text(helper_pid);
        return text.empty() ? 0 : std::stoul(text);
    }
    // No openwhisper-cpp-ask-* folder is left in the temporary folder.
    bool ask_folders_removed() const {
        const auto prefix = py::lower(identity().product) + "-ask-";
        for (const auto& entry : std::filesystem::directory_iterator(tmp)) {
            if (path_to_utf8(entry.path().filename()).starts_with(prefix)) {
                return false;
            }
        }
        return true;
    }
};

bool same_folder(const Json& recorded, const std::filesystem::path& expected) {
    std::error_code error;
    return std::filesystem::equivalent(path_from_utf8(recorded.get<std::string>()), expected,
                                       error);
}

#ifdef _WIN32
#define FV_REQUIRE_PROCESSES()
#else
#define FV_REQUIRE_PROCESSES() SKIP("programs are started on Windows only")
#endif

} // namespace

TEST_CASE("claude answers from its json result", "[test_automation_agents_ask][platform]") {
    // from tests/test_automation_agents_ask.py::test_claude_answers_from_its_json_result
    FV_REQUIRE_PROCESSES();
    FakeAgents fake;
    fake.mode("ok");
    const auto result = agents::ask("claude_code", WORDS, "haiku");
    CHECK(result.ok);
    CHECK(result.message == "Claude Code answered");
    CHECK(result.answer == py::strip("Answer: " + WORDS));
    const auto seen = fake.recorded();
    CHECK(seen.at("prompt") == WORDS); // on stdin, intact
    const auto argv = seen.at("argv").get<std::vector<std::string>>();
    CHECK_FALSE(mentions_question(argv));
    CHECK(same_folder(seen.at("cwd"), fake.table->agent_workspace_dir()));
    // No tools, no MCP servers, no hooks: what the agent was given.
    CHECK(after(argv, "--tools").empty());
    CHECK(has(argv, "--strict-mcp-config"));
    CHECK(py::loads(seen.at("files").at("--mcp-config").get<std::string>()) ==
          Json{{"mcpServers", Json::object()}});
    CHECK(py::loads(seen.at("files").at("--settings").get<std::string>()) ==
          Json{{"disableAllHooks", true}, {"alwaysThinkingEnabled", false}});
    CHECK(after(argv, "--model") == "haiku");
    // The ask's own folder is gone; the workspace stays for the next ask.
    CHECK_FALSE(std::filesystem::exists(path_from_utf8(after(argv, "--settings")).parent_path()));
    CHECK(fake.ask_folders_removed());
}

TEST_CASE("a signed out claude says how to sign in", "[test_automation_agents_ask][platform]") {
    // from tests/test_automation_agents_ask.py::test_a_signed_out_claude_says_how_to_sign_in
    FV_REQUIRE_PROCESSES();
    FakeAgents fake;
    fake.mode("signed-out");
    const auto result = agents::ask("claude_code", WORDS);
    CHECK_FALSE(result.ok);
    CHECK(result.message == "Claude Code is signed out. " + agents::sign_in_hint("claude_code"));
}

TEST_CASE("an empty answer is not success", "[test_automation_agents_ask][platform]") {
    // from tests/test_automation_agents_ask.py::test_an_empty_answer_is_not_success
    FV_REQUIRE_PROCESSES();
    FakeAgents fake;
    fake.mode("empty");
    const auto result = agents::ask("claude_code", WORDS);
    CHECK_FALSE(result.ok);
    CHECK(result.message == "Claude Code didn't answer.");
}

TEST_CASE("an ask keeps whole lines of stdout until 4 MiB is reached",
          "[agent_actions][platform]") {
    // agents.py _read_stdout: a line is kept while less than _STDOUT_MAX_BYTES came before it, so
    // the 1 MiB result after 4 MiB less 10 bytes of noise is read whole, in many pipe chunks, and
    // the result line after that is dropped.
    FV_REQUIRE_PROCESSES();
    FakeAgents fake;
    fake.mode("long");
    const auto started = std::chrono::steady_clock::now();
    const auto result = agents::ask("claude_code", WORDS, "", 60.0);
    CHECK(std::chrono::steady_clock::now() - started < std::chrono::seconds(30));
    CHECK(result.ok);
    CHECK(result.message == "Claude Code answered");
    CHECK(result.answer == std::string(1024 * 1024, 'a'));
}

TEST_CASE("codex answers despite a retried hiccup", "[test_automation_agents_ask][platform]") {
    // from tests/test_automation_agents_ask.py::test_codex_answers_despite_a_retried_hiccup
    FV_REQUIRE_PROCESSES();
    FakeAgents fake;
    fake.mode("ok");
    const auto result = agents::ask("codex", WORDS);
    CHECK(result.ok);
    CHECK(result.answer == py::strip("Answer: " + WORDS));
    const auto seen = fake.recorded();
    CHECK(seen.at("prompt") == WORDS);
    const auto argv = seen.at("argv").get<std::vector<std::string>>();
    CHECK(values_after(argv, "--disable") ==
          std::vector<std::string>{"shell_tool", "code_mode_host", "apps", "js_repl"});
    CHECK(seen.at("files").at("model_instructions_file") == std::string(agents::ASK_INSTRUCTION));
    CHECK(argv.back() == "-");
    CHECK(fake.ask_folders_removed());
}

TEST_CASE("codex errors show the providers message", "[test_automation_agents_ask][platform]") {
    // from tests/test_automation_agents_ask.py::test_codex_errors_show_the_providers_message
    FV_REQUIRE_PROCESSES();
    FakeAgents fake;
    fake.mode("fail");
    const auto result = agents::ask("codex", WORDS);
    CHECK_FALSE(result.ok);
    CHECK(result.message == "Codex couldn't answer: The 'x' model is not supported.");
}

TEST_CASE("a crash shows the last error line", "[test_automation_agents_ask][platform]") {
    // from tests/test_automation_agents_ask.py::test_a_crash_shows_the_last_error_line
    FV_REQUIRE_PROCESSES();
    FakeAgents fake;
    fake.mode("crash");
    CHECK(agents::ask("claude_code", WORDS).message ==
          "Claude Code stopped before answering: error: unknown option '--frobnicate'");
}

TEST_CASE("a timeout kills the agent and its helpers", "[test_automation_agents_ask][platform]") {
    // from tests/test_automation_agents_ask.py::test_a_timeout_kills_the_agent_and_its_helpers
    // Python's 5 s limit: the hanging agent must have started its helper before the deadline,
    // which a loaded machine (or a Debug build) may not manage within 1 s.
    FV_REQUIRE_PROCESSES();
    FakeAgents fake;
    fake.mode("hang");
    const auto started = std::chrono::steady_clock::now();
    const auto result = agents::ask("claude_code", WORDS, "", 5.0);
    const auto elapsed = std::chrono::steady_clock::now() - started;
    const auto pid = fake.helper();
    CHECK_FALSE(result.ok);
    CHECK(result.message == "Claude Code didn't answer in time.");
    CHECK(elapsed < std::chrono::seconds(15));
    CHECK(pid != 0);
    CHECK(wait_dead(pid));
    CHECK(fake.ask_folders_removed());
}

TEST_CASE("a hanging agent asked with a 1 s limit says so and leaves nothing running",
          "[agent_actions][platform]") {
    // The spec's acceptance: the shortest limit an ask takes. Whether the helper started before
    // the deadline depends on the machine's load; whatever did start is gone afterwards.
    FV_REQUIRE_PROCESSES();
    FakeAgents fake;
    fake.mode("hang");
    const auto started = std::chrono::steady_clock::now();
    const auto result = agents::ask("claude_code", WORDS, "", 1.0);
    CHECK(std::chrono::steady_clock::now() - started < std::chrono::seconds(15));
    CHECK_FALSE(result.ok);
    CHECK(result.message == "Claude Code didn't answer in time.");
    if (const auto pid = fake.helper(); pid != 0) {
        CHECK(wait_dead(pid));
    }
    CHECK(fake.ask_folders_removed());
}

TEST_CASE("cancel kills the agent and its helpers", "[test_automation_agents_ask][platform]") {
    // from tests/test_automation_agents_ask.py::test_cancel_kills_the_agent_and_its_helpers
    FV_REQUIRE_PROCESSES();
    FakeAgents fake;
    fake.mode("hang");
    CancelSource cancel;
    std::thread canceller([&] {
        const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(20);
        while (fake.helper() == 0 && std::chrono::steady_clock::now() < until) {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
        cancel.cancel();
    });
    const auto result = agents::ask("codex", WORDS, "", 60.0, cancel.token());
    canceller.join();
    const auto pid = fake.helper();
    CHECK_FALSE(result.ok);
    CHECK(result.message == "Stopped asking Codex.");
    CHECK(result.stopped); // the action runtime reports it as canceled
    CHECK(pid != 0);
    CHECK(wait_dead(pid));
}

TEST_CASE("opencode answers in a locked down session it then deletes",
          "[test_automation_agents_ask][platform]") {
    // from
    // tests/test_automation_agents_ask.py::test_opencode_answers_in_a_locked_down_session_it_then_deletes
    FV_REQUIRE_PROCESSES();
    FakeAgents fake;
    fake.mode("ok");
    const auto result = agents::ask("opencode_cli", WORDS);
    CHECK(result.ok);
    CHECK(result.message == "OpenCode answered");
    CHECK(result.answer == py::strip("Answer: " + WORDS));
    const auto log = fake.acp();
    REQUIRE(log.size() >= 5);
    const auto& overlay = log[0].at("overlay");
    REQUIRE(overlay.is_object());
    CHECK(overlay.size() == 1); // no MCP servers of ours or anyone's
    for (const auto* persona : {"build", "plan"}) {
        CHECK(overlay.at("agents").at(persona).at("system") ==
              std::string(agents::ASK_INSTRUCTION));
        CHECK(overlay.at("agents").at(persona).at("permissions") ==
              Json::array({Json{{"action", "*"}, {"resource", "*"}, {"effect", "deny"}}}));
    }
    CHECK(log[0].at("project_config_off") == "1");
    CHECK(log[0].at("autoupdate_off") == "1");
    CHECK(log[0].at("filewatcher_off") == "1");
    CHECK(fake.acp_methods(1) == std::vector<std::string>{"initialize", "session/new",
                                                          "session/prompt", "session/delete"});
    const auto& created = log[2].at("params");
    CHECK(created.at("mcpServers") == Json::array());
    CHECK(same_folder(created.at("cwd"), fake.table->agent_workspace_dir()));
    CHECK(log[3].at("params").at("prompt") ==
          Json::array({Json{{"type", "text"}, {"text", WORDS}}}));
}

TEST_CASE("opencode switches to the chosen model", "[test_automation_agents_ask][platform]") {
    // from tests/test_automation_agents_ask.py::test_opencode_switches_to_the_chosen_model
    FV_REQUIRE_PROCESSES();
    FakeAgents fake;
    fake.mode("ok");
    CHECK(agents::ask("opencode_cli", WORDS, "prov/slow").ok);
    std::vector<Json> switched;
    for (const auto& entry : fake.acp()) {
        if (entry.value("method", Json()) == "session/set_config_option") {
            switched.push_back(entry.at("params").at("value"));
        }
    }
    CHECK(switched == std::vector<Json>{"prov/slow"});
}

TEST_CASE("opencode uses the users default only when it is offered",
          "[test_automation_agents_ask][platform]") {
    // from
    // tests/test_automation_agents_ask.py::test_opencode_uses_the_users_default_only_when_it_is_offered
    FV_REQUIRE_PROCESSES();
    FakeAgents fake;
    fake.mode("ok");
    write_file(fake.temp.path() / "xdg" / "opencode" / "opencode.json",
               R"({"model": "gone/model"})");
    REQUIRE(fake.table->config().configured_default_model("opencode_cli") == "gone/model");
    CHECK(agents::ask("opencode_cli", WORDS).ok);
    const auto methods = fake.acp_methods();
    CHECK(std::find(methods.begin(), methods.end(), "session/set_config_option") == methods.end());
}

TEST_CASE("opencode refuses a model it does not offer", "[test_automation_agents_ask][platform]") {
    // from tests/test_automation_agents_ask.py::test_opencode_refuses_a_model_it_does_not_offer
    FV_REQUIRE_PROCESSES();
    FakeAgents fake;
    fake.mode("ok");
    const auto result = agents::ask("opencode_cli", WORDS, "prov/missing");
    CHECK_FALSE(result.ok);
    CHECK(result.message.find("no model prov/missing") != std::string::npos);
    const auto methods = fake.acp_methods(1);
    CHECK(std::find(methods.begin(), methods.end(), "session/prompt") == methods.end());
    CHECK(methods.back() == "session/delete");
}

TEST_CASE("opencode permission requests are rejected", "[test_automation_agents_ask][platform]") {
    // from tests/test_automation_agents_ask.py::test_opencode_permission_requests_are_rejected
    FV_REQUIRE_PROCESSES();
    FakeAgents fake;
    fake.mode("permission");
    CHECK(agents::ask("opencode_cli", WORDS).ok);
    const auto log = fake.acp();
    const auto reply = std::find_if(log.begin(), log.end(), [](const Json& entry) {
        return entry.contains("method") && entry.at("method").is_null() &&
               !entry.at("result").is_null();
    });
    REQUIRE(reply != log.end());
    CHECK(reply->at("result") ==
          Json{{"outcome", Json{{"outcome", "selected"}, {"optionId", "no"}}}});
}

TEST_CASE("opencode auth errors say how to sign in", "[test_automation_agents_ask][platform]") {
    // from tests/test_automation_agents_ask.py::test_opencode_auth_errors_say_how_to_sign_in
    FV_REQUIRE_PROCESSES();
    FakeAgents fake;
    fake.mode("auth");
    CHECK(agents::ask("opencode_cli", WORDS).message ==
          "OpenCode is signed out. " + agents::sign_in_hint("opencode_cli"));
}

TEST_CASE("opencode timeout and cancel end the session", "[test_automation_agents_ask][platform]") {
    // from tests/test_automation_agents_ask.py::test_opencode_timeout_and_cancel_end_the_session
    FV_REQUIRE_PROCESSES();
    FakeAgents fake;
    fake.mode("hang");
    const auto started = std::chrono::steady_clock::now();
    const auto timed_out = agents::ask("opencode_cli", WORDS, "", 2.0);
    CHECK(timed_out.message == "OpenCode didn't answer in time.");
    CHECK(std::chrono::steady_clock::now() - started < std::chrono::seconds(15));
    const auto methods = fake.acp_methods();
    CHECK(std::find(methods.begin(), methods.end(), "session/cancel") != methods.end());
    CHECK(methods.back() == "session/delete");

    CancelSource cancel;
    std::thread canceller([&] {
        std::this_thread::sleep_for(std::chrono::seconds(1));
        cancel.cancel();
    });
    const auto canceled = agents::ask("opencode_cli", WORDS, "", 60.0, cancel.token());
    canceller.join();
    CHECK(canceled.message == "Stopped asking OpenCode.");
    CHECK(canceled.stopped);
}

TEST_CASE("installed cleanup preserves transcript intent in isolated headless calls",
          "[test_automation_agents_ask][platform]") {
    FV_REQUIRE_PROCESSES();
    FakeAgents fake;
    fake.mode("ok");
    const std::string rules = "Edit only typos.";
    std::string provider;
    std::string effort;
    SECTION("Claude Code") {
        provider = "claude_code";
        effort = "max";
    }
    SECTION("Codex cached minimal") {
        provider = "codex";
        effort = "minimal";
    }
    SECTION("Codex cached ultra") {
        provider = "codex";
        effort = "ultra";
    }
    SECTION("Default effort") {
        provider = "codex";
        effort = "off";
    }
    const auto result =
        agents::ask_for_cleanup(provider, WORDS, rules, "test-model", effort, 10, {});
    REQUIRE(result.ok);
    const auto seen = fake.recorded();
    CHECK(seen.at("prompt") == WORDS);
    const auto argv = seen.at("argv").get<std::vector<std::string>>();
    CHECK_FALSE(mentions_question(argv));
    if (provider == "claude_code") {
        CHECK(after(argv, "--model") == "test-model");
        CHECK(after(argv, "--effort") == effort);
        CHECK(has(argv, "--setting-sources"));
        CHECK(after(argv, "--setting-sources").empty());
        CHECK(has(argv, "--no-session-persistence"));
        CHECK(after(argv, "--tools").empty());
        CHECK(has(argv, "--strict-mcp-config"));
        CHECK(py::loads(seen.at("files").at("--mcp-config").get<std::string>()) ==
              Json{{"mcpServers", Json::object()}});
        CHECK(seen.at("files")
                  .at("--system-prompt-file")
                  .get<std::string>()
                  .starts_with(rules + "\n\n"));
    } else {
        CHECK(after(argv, "-m") == "test-model");
        if (effort == "off") {
            const auto overrides = values_after(argv, "-c");
            CHECK(std::none_of(overrides.begin(), overrides.end(), [](const std::string& value) {
                return value.starts_with("model_reasoning_effort=");
            }));
        } else {
            CHECK(has(argv, "model_reasoning_effort=" + effort));
        }
        CHECK(has(argv, "--ignore-user-config"));
        CHECK(has(argv, "--ephemeral"));
        CHECK(after(argv, "-s") == "read-only");
        CHECK(has(argv, "shell_tool"));
        CHECK(seen.at("files")
                  .at("model_instructions_file")
                  .get<std::string>()
                  .starts_with(rules + "\n\n"));
    }
    CHECK(fake.ask_folders_removed());
}

TEST_CASE("installed cleanup neither launches a pre-canceled agent nor accepts missing isolation",
          "[test_automation_agents_ask][platform]") {
    FV_REQUIRE_PROCESSES();
    FakeAgents fake;
    SECTION("pre-canceled") {
        CancelSource cancel;
        cancel.cancel();
        const auto result =
            agents::ask_for_cleanup("codex", WORDS, "rules", "", "low", 10, cancel.token());
        CHECK_FALSE(result.ok);
        CHECK(result.stopped);
    }
    SECTION("older Claude CLI") {
        fake.env->insert_or_assign("FV_FAKE_AGENT_HELP", "--tools --settings");
        const auto result =
            agents::ask_for_cleanup("claude_code", WORDS, "rules", "", "low", 10, {});
        CHECK_FALSE(result.ok);
    }
    SECTION("older Codex CLI") {
        fake.env->insert_or_assign("FV_FAKE_AGENT_EXEC_HELP", "--ephemeral");
        const auto result = agents::ask_for_cleanup("codex", WORDS, "rules", "", "low", 10, {});
        CHECK_FALSE(result.ok);
    }
    CHECK_FALSE(std::filesystem::exists(fake.record));
    CHECK(fake.ask_folders_removed());
}

TEST_CASE("headless cleanup refuses discarded provider and sign-in settings before launching",
          "[test_automation_agents_ask]") {
    ActionsFixture fixture;
    script_help(fixture);
    std::string provider = "codex";
    const auto codex = fixture.table->config().codex_home() / "config.toml";
    const auto claude = fixture.table->config().claude_config_dir() / "settings.json";
    SECTION("Codex uses a custom provider") {
        write_file(codex, "'model_provider' = 'internal'\n"
                          "[model_providers.internal]\nbase_url = 'http://127.0.0.1:1234/v1'\n");
    }
    SECTION("Codex overrides the OpenAI endpoint") {
        write_file(codex, "'openai_base_url' = 'http://127.0.0.1:1234/v1'\n");
    }
    SECTION("Codex selects a profile") {
        write_file(codex, "profile = 'private'\n[profiles.private]\nmodel_provider = 'internal'\n");
    }
    SECTION("Codex restricts sign-in") {
        write_file(codex, "forced_login_method = 'api'\n");
    }
    SECTION("Claude routes through an endpoint from settings") {
        provider = "claude_code";
        write_file(claude, R"({"env":{"ANTHROPIC_BASE_URL":"http://127.0.0.1:1234"}})");
    }
    SECTION("Claude uses a cloud provider from settings") {
        provider = "claude_code";
        write_file(claude, R"({"env":{"CLAUDE_CODE_USE_BEDROCK":"1"}})");
    }
    SECTION("Claude gets credentials from the settings environment") {
        provider = "claude_code";
        write_file(claude, R"({"env":{"ANTHROPIC_AUTH_TOKEN":"fixture-token"}})");
    }
    SECTION("Claude needs its configured login method") {
        provider = "claude_code";
        write_file(claude, R"({"forceLoginMethod":"console"})");
    }
    SECTION("Unparseable Codex configuration fails closed") {
        write_file(codex, "model_provider = 'truncated");
    }
    CHECK_FALSE(fixture.table->config().supports_isolated_cleanup(provider));
    const auto result =
        agents::ask_for_cleanup(provider, WORDS, "rules", "named-model", "low", 5, {});
    CHECK_FALSE(result.ok);
    CHECK(result.message == "The agent's configured destination cannot be preserved.");
    CHECK_FALSE(started_ask(fixture));
}

TEST_CASE("headless cleanup preserves a configured model with no active route override",
          "[test_automation_agents_ask][platform]") {
    FV_REQUIRE_PROCESSES();
    FakeAgents fake;
    fake.mode("ok");
    SECTION("Codex does not select an unrelated provider or profile definition") {
        write_file(fake.table->config().codex_home() / "config.toml",
                   "model = 'my-codex-model'\nmodel_provider = 'openai'\n"
                   "[model_providers.local]\nbase_url = 'http://127.0.0.1:1234/v1'\n"
                   "[profiles.offline]\nmodel_provider = 'local'\n");
        REQUIRE(agents::ask_for_cleanup("codex", WORDS, "rules", "", "low", 10, {}).ok);
        const auto argv = fake.recorded().at("argv").get<std::vector<std::string>>();
        CHECK(after(argv, "-m") == "my-codex-model");
    }
    SECTION("Claude keeps the saved default model and permits unrelated preferences") {
        write_file(fake.table->config().claude_config_dir() / "settings.json",
                   R"({"model":"saved-haiku","theme":"dark","env":{"EDITOR":"example"}})");
        REQUIRE(agents::ask_for_cleanup("claude_code", WORDS, "rules", "", "low", 10, {}).ok);
        const auto argv = fake.recorded().at("argv").get<std::vector<std::string>>();
        CHECK(after(argv, "--model") == "saved-haiku");
    }
    CHECK(fake.recorded().at("prompt") == WORDS);
}
