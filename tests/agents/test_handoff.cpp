// Handing dictated words to an installed agent (services/actions/agents.py handoff), from
// tests/test_automation_agents_handoff.py. Links and terminals are recorded by a fake launch
// backend instead of opened, and the agents come from a table.

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "agent_actions_support.h"
#include "agents/agent_actions.h"
#include "agents/detail/headless.h"
#include "base/identity.h"
#include "base/utf8.h"
#include "pyport/text.h"

using namespace fv;
using namespace fv::agents_test;

namespace {

const std::string& WORDS = HANDOFF_WORDS;

std::string handoff(std::string agent_id, std::string text, std::string surface = "auto",
                    std::filesystem::path folder = {}, bool new_folder = false) {
    return agents::handoff(agents::HandoffRequest{.agent_id = std::move(agent_id),
                                                  .text = std::move(text),
                                                  .surface = std::move(surface),
                                                  .folder = std::move(folder),
                                                  .new_folder = new_folder});
}

// The sentence a refused handoff shows.
std::string refusal(std::string agent_id, std::string text, std::string surface = "auto",
                    std::filesystem::path folder = {}, bool new_folder = false) {
    std::string message;
    try {
        handoff(std::move(agent_id), std::move(text), std::move(surface), std::move(folder),
                new_folder);
    } catch (const agents::HandoffError& error) {
        message = error.what();
    }
    CHECK_FALSE(message.empty()); // the handoff was refused
    return message;
}

std::string str(const std::filesystem::path& path) {
    return path_to_utf8(path);
}

std::filesystem::path tasks_root(const std::filesystem::path& home) {
    return home / path_from_utf8(std::string(identity().product) + " Agent Tasks");
}

bool empty_dir(const std::filesystem::path& path) {
    return std::filesystem::is_directory(path) && std::filesystem::is_empty(path);
}

std::string repeat(std::string_view text, std::size_t times) {
    std::string out;
    for (std::size_t i = 0; i < times; ++i) {
        out += text;
    }
    return out;
}

} // namespace

// ---- Claude Code ----

TEST_CASE("claude prefers the desktop app and leaves the folder to it",
          "[test_automation_agents_handoff]") {
    // from
    // tests/test_automation_agents_handoff.py::test_claude_prefers_the_desktop_app_and_leaves_the_folder_to_it
    ActionsFixture fixture;
    Desk desk;
    desk.handler("claude", true);
    desk.handler("claude-cli", true);
    CHECK(handoff("claude_code", WORDS) == "Opened Claude Code");
    const auto opened = desk.opened();
    REQUIRE(opened.size() == 1);
    const auto [base, values] = query(opened[0]);
    CHECK(base == "claude://code/new");
    CHECK(values == std::map<std::string, std::string>{{"q", WORDS}});
    CHECK(desk.terminals().empty());
}

TEST_CASE("claude desktop link carries a chosen folder", "[test_automation_agents_handoff]") {
    // from
    // tests/test_automation_agents_handoff.py::test_claude_desktop_link_carries_a_chosen_folder
    ActionsFixture fixture;
    Desk desk;
    desk.handler("claude", true);
    handoff("claude_code", WORDS, "auto", fixture.tmp);
    CHECK(query(desk.opened().at(0)).second ==
          std::map<std::string, std::string>{{"q", WORDS}, {"folder", str(fixture.tmp)}});
}

TEST_CASE("every reserved character is encoded", "[test_automation_agents_handoff]") {
    // from tests/test_automation_agents_handoff.py::test_every_reserved_character_is_encoded
    ActionsFixture fixture;
    Desk desk;
    desk.handler("claude", true);
    handoff("claude_code", WORDS);
    const auto uri = desk.opened().at(0);
    const auto q = uri.substr(uri.find('?') + 1);
    const auto first = q.substr(0, q.find('&')).substr(2);
    CHECK(std::all_of(first.begin(), first.end(), [](char c) {
        return std::string_view(
                   "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789%-._~")
                   .find(c) != std::string_view::npos;
    }));
}

TEST_CASE("claude code link is next and always names a folder",
          "[test_automation_agents_handoff]") {
    // from
    // tests/test_automation_agents_handoff.py::test_claude_code_link_is_next_and_always_names_a_folder
    ActionsFixture fixture;
    Desk desk;
    desk.handler("claude-cli", true);
    handoff("claude_code", WORDS);
    handoff("claude_code", WORDS, "auto", fixture.tmp);
    const auto opened = desk.opened();
    REQUIRE(opened.size() == 2);
    CHECK(query(opened[0]) == std::pair<std::string, std::map<std::string, std::string>>{
                                  "claude-cli://open", {{"q", WORDS}, {"cwd", str(fixture.home)}}});
    CHECK(query(opened[1]) == std::pair<std::string, std::map<std::string, std::string>>{
                                  "claude-cli://open", {{"q", WORDS}, {"cwd", str(fixture.tmp)}}});
}

TEST_CASE("claude falls back to a terminal", "[test_automation_agents_handoff]") {
    // from tests/test_automation_agents_handoff.py::test_claude_falls_back_to_a_terminal
    ActionsFixture fixture;
    const auto token = std::string(identity().env_prefix) + "SIDECAR_TOKEN";
    const auto legacy_token = std::string(legacy_identity().env_prefix) + "SIDECAR_TOKEN";
    fixture.env->insert_or_assign(token, "secret");
    fixture.env->insert_or_assign(legacy_token, "secret");
    Desk desk;
    CHECK(handoff("claude_code", WORDS) == "Opened Claude Code in a terminal");
    const auto terminals = desk.terminals();
    REQUIRE(terminals.size() == 1);
    CHECK(terminals[0].argv == std::vector<std::string>{str(fixture.claude.path), "--", WORDS});
    CHECK(terminals[0].cwd == fixture.home);
    CHECK_FALSE(terminals[0].env.empty());
    CHECK_FALSE(terminals[0].env.contains(token));
    CHECK_FALSE(terminals[0].env.contains(legacy_token));
}

TEST_CASE("a lone command word reaches claude as a prompt", "[test_automation_agents_handoff]") {
    // from
    // tests/test_automation_agents_handoff.py::test_a_lone_command_word_reaches_claude_as_a_prompt
    // "claude -- purge" would run Claude Code's purge command.
    ActionsFixture fixture;
    Desk desk;
    handoff("claude_code", "purge", "terminal");
    CHECK(desk.terminals().at(0).argv ==
          std::vector<std::string>{str(fixture.claude.path), "--", "purge "});
}

TEST_CASE("claude code link limit", "[test_automation_agents_handoff]") {
    // from tests/test_automation_agents_handoff.py::test_claude_code_link_limit
    const auto [length, expected] =
        GENERATE(std::pair<std::size_t, std::string>{agents::CLAUDE_CLI_TEXT_MAX, "claude-cli"},
                 std::pair<std::size_t, std::string>{agents::CLAUDE_CLI_TEXT_MAX + 1, "terminal"});
    ActionsFixture fixture;
    Desk desk;
    desk.handler("claude-cli", true);
    handoff("claude_code", repeat("a ", length / 2) + repeat("b", length % 2));
    CHECK((desk.terminals().empty() ? scheme_of(desk.opened().at(0)) : "terminal") == expected);
}

TEST_CASE("claude desktop limit sends long words to the terminal",
          "[test_automation_agents_handoff]") {
    // from
    // tests/test_automation_agents_handoff.py::test_claude_desktop_limit_sends_long_words_to_the_terminal
    ActionsFixture fixture;
    Desk desk;
    desk.handler("claude", true);
    desk.handler("claude-cli", true);
    handoff("claude_code", repeat("word ", 2000)); // 10,000 characters
    CHECK(scheme_of(desk.opened().at(0)) == "claude");
    const auto too_long = repeat("word ", agents::CLAUDE_APP_TEXT_MAX / 5 + 1);
    CHECK(handoff("claude_code", too_long).ends_with("in a terminal"));
    CHECK(refusal("claude_code", too_long, "app").find("too long") != std::string::npos);
    CHECK(desk.terminals().size() == 1);
}

TEST_CASE("links stay inside the command line budget", "[test_automation_agents_handoff]") {
    // from tests/test_automation_agents_handoff.py::test_links_stay_inside_the_command_line_budget
    // 4,000 CJK characters encode to 36,000; no handler could receive that.
    ActionsFixture fixture;
    Desk desk;
    const auto words = repeat("\xE6\x97\xA5\xE6\x9C\xAC\xE8\xAA\x9E", 1334);
    CHECK(agents::app_links("claude_code", words).empty());
    desk.handler("claude", true);
    desk.handler("claude-cli", true);
    CHECK(handoff("claude_code", words).ends_with("in a terminal"));
}

TEST_CASE("a link that fails to open falls through", "[test_automation_agents_handoff]") {
    // from tests/test_automation_agents_handoff.py::test_a_link_that_fails_to_open_falls_through
    ActionsFixture fixture;
    Desk desk;
    desk.handler("claude", true);
    desk.handler("claude-cli", true);
    desk.broken("claude");
    CHECK_NOTHROW(handoff("claude_code", WORDS));
    const auto opened = desk.opened();
    REQUIRE(opened.size() == 1);
    CHECK(scheme_of(opened[0]) == "claude-cli");
}

TEST_CASE("no answer from windows tries nothing else", "[test_automation_agents_handoff]") {
    // from tests/test_automation_agents_handoff.py::test_no_answer_from_windows_tries_nothing_else
    // Windows may still open the first link late; a second link or a terminal now could start
    // the agent twice.
    ActionsFixture fixture;
    Desk desk;
    desk.handler("claude", true);
    desk.handler("claude-cli", true);
    desk.no_answer();
    CHECK(refusal("claude_code", WORDS) == "Windows didn't answer. Try again.");
    const auto opened = desk.opened();
    REQUIRE(opened.size() == 1);
    CHECK(scheme_of(opened[0]) == "claude");
    CHECK(desk.terminals().empty());
}

TEST_CASE("unknown handlers are tried only when the app was asked for",
          "[test_automation_agents_handoff]") {
    // from
    // tests/test_automation_agents_handoff.py::test_unknown_handlers_are_tried_only_when_the_app_was_asked_for
    ActionsFixture fixture;
    Desk desk;
    desk.handler("claude", std::nullopt);
    handoff("claude_code", WORDS);
    CHECK(desk.opened().empty());
    CHECK(desk.terminals().size() == 1);
    CHECK_NOTHROW(handoff("claude_code", WORDS, "app"));
    CHECK(scheme_of(desk.opened().at(0)) == "claude");
}

TEST_CASE("terminal surface ignores app links", "[test_automation_agents_handoff]") {
    // from tests/test_automation_agents_handoff.py::test_terminal_surface_ignores_app_links
    ActionsFixture fixture;
    Desk desk;
    desk.handler("claude", true);
    desk.handler("claude-cli", true);
    CHECK_NOTHROW(handoff("claude_code", WORDS, "terminal"));
    CHECK(desk.opened().empty());
    CHECK(desk.terminals().size() == 1);
}

TEST_CASE("app surface never opens a terminal", "[test_automation_agents_handoff]") {
    // from tests/test_automation_agents_handoff.py::test_app_surface_never_opens_a_terminal
    ActionsFixture fixture;
    Desk desk;
    CHECK(refusal("claude_code", WORDS, "app").find("Claude app") != std::string::npos);
    CHECK(desk.terminals().empty());
}

// A19-58: each agent's no-app copy, word for word (agents.py:_no_app_message).
TEST_CASE("app surface without an app names what to install", "[test_automation_agents_handoff]") {
    ActionsFixture fixture;
    Desk desk;
    CHECK(refusal("claude_code", WORDS, "app") ==
          "Claude Code can't open in an app here. Install the Claude app, or choose Terminal.");
    CHECK(refusal("claude_code", std::string(14001, 'a'), "app") ==
          "That's too long for the Claude app. Choose Terminal for long requests.");
    CHECK(refusal("codex", WORDS, "app") ==
          "Codex can't open in an app here. Install the ChatGPT app, or choose Terminal.");
    CHECK(refusal("opencode_cli", WORDS, "app") ==
          "OpenCode has no app link. Choose Terminal instead.");
    CHECK(desk.opened().empty());
    CHECK(desk.terminals().empty());
}

TEST_CASE("app surface reports the link failure", "[test_automation_agents_handoff]") {
    // from tests/test_automation_agents_handoff.py::test_app_surface_reports_the_link_failure
    ActionsFixture fixture;
    Desk desk;
    desk.handler("claude", true);
    desk.broken("claude");
    CHECK(refusal("claude_code", WORDS, "app") == "That app didn't open.");
}

TEST_CASE("the desktop app works without the cli", "[test_automation_agents_handoff]") {
    // from tests/test_automation_agents_handoff.py::test_the_desktop_app_works_without_the_cli
    ActionsFixture fixture;
    fixture.table->clear();
    Desk desk;
    desk.handler("claude", true);
    CHECK_NOTHROW(handoff("claude_code", WORDS));
}

// ---- Codex and OpenCode ----

TEST_CASE("codex opens the chatgpt app", "[test_automation_agents_handoff]") {
    // from tests/test_automation_agents_handoff.py::test_codex_opens_the_chatgpt_app
    ActionsFixture fixture;
    Desk desk;
    desk.handler("codex", true);
    CHECK(handoff("codex", WORDS) == "Opened Codex");
    handoff("codex", WORDS, "auto", fixture.tmp);
    const auto opened = desk.opened();
    REQUIRE(opened.size() == 2);
    CHECK(query(opened[0]) == std::pair<std::string, std::map<std::string, std::string>>{
                                  "codex://new", {{"prompt", WORDS}}});
    CHECK(query(opened[1]) == std::pair<std::string, std::map<std::string, std::string>>{
                                  "codex://new", {{"prompt", WORDS}, {"path", str(fixture.tmp)}}});
}

TEST_CASE("codex terminal keeps the words out of its commands",
          "[test_automation_agents_handoff]") {
    // from
    // tests/test_automation_agents_handoff.py::test_codex_terminal_keeps_the_words_out_of_its_commands
    ActionsFixture fixture;
    Desk desk;
    CHECK_NOTHROW(handoff("codex", "exec", "auto", fixture.tmp));
    const auto terminals = desk.terminals();
    REQUIRE(terminals.size() == 1);
    CHECK(terminals[0].argv == std::vector<std::string>{str(fixture.codex.path), "--", "exec"});
    CHECK(terminals[0].cwd == fixture.tmp);
}

TEST_CASE("codex new folders are distinct empty and persistent",
          "[test_automation_agents_handoff]") {
    // from
    // tests/test_automation_agents_handoff.py::test_codex_new_folders_are_distinct_empty_and_persistent
    ActionsFixture fixture;
    Desk desk;
    desk.handler("codex", true);
    const auto stale = fixture.temp.path() / "missing-old-project";
    CHECK_NOTHROW(handoff("codex", WORDS, "auto", stale, true));
    const auto first = path_from_utf8(query(desk.opened().at(0)).second.at("path"));
    CHECK(empty_dir(first));
    write_file(first / "notes.txt", "Keep these files for the agent's next turn.");

    CHECK_NOTHROW(handoff("codex", WORDS, "auto", {}, true));
    const auto second = path_from_utf8(query(desk.opened().at(1)).second.at("path"));
    CHECK(empty_dir(second));
    CHECK(first != second);
    CHECK(first.parent_path() == tasks_root(fixture.home));
    CHECK(second.parent_path() == tasks_root(fixture.home));
    std::ifstream notes(first / "notes.txt");
    CHECK(std::string(std::istreambuf_iterator<char>(notes), {}) ==
          "Keep these files for the agent's next turn.");
    CHECK(query(desk.opened().at(0)).second.at("prompt") == WORDS);
    CHECK(desk.opened().at(0).find(str(stale)) == std::string::npos);
    CHECK(desk.terminals().empty());
    // The folder's name holds the time and a random part, never the words.
    const auto name = path_to_utf8(first.filename());
    CHECK(name.starts_with("task-"));
    CHECK(name.size() == std::string_view("task-2026-11-16-123456-").size() + 8);
}

TEST_CASE("new folder is the terminal working directory", "[test_automation_agents_handoff]") {
    // from
    // tests/test_automation_agents_handoff.py::test_new_folder_is_the_terminal_working_directory
    const auto agent_id =
        GENERATE(std::string("claude_code"), std::string("codex"), std::string("opencode_cli"));
    ActionsFixture fixture;
    Desk desk;
    CHECK_NOTHROW(handoff(agent_id, WORDS, "terminal", {}, true));
    const auto terminals = desk.terminals();
    REQUIRE(terminals.size() == 1);
    CHECK(empty_dir(terminals[0].cwd));
    CHECK(terminals[0].argv ==
          agents::terminal_argv(agent_id, fixture.agent(agent_id).path, WORDS));
    CHECK(terminals[0].cwd.parent_path() == tasks_root(fixture.home));
}

TEST_CASE("new folder survives an app failure and terminal fallback",
          "[test_automation_agents_handoff]") {
    // from
    // tests/test_automation_agents_handoff.py::test_new_folder_survives_an_app_failure_and_terminal_fallback
    ActionsFixture fixture;
    Desk desk;
    desk.handler("codex", true);
    desk.all_broken();
    CHECK_NOTHROW(handoff("codex", WORDS, "auto", {}, true));
    CHECK(str(desk.terminals().at(0).cwd) == query(desk.attempted().at(0)).second.at("path"));
    const auto root = tasks_root(fixture.home);
    CHECK(std::distance(std::filesystem::directory_iterator(root),
                        std::filesystem::directory_iterator()) == 1);
}

TEST_CASE("new folder failure does not open an agent in an existing project",
          "[test_automation_agents_handoff]") {
    // from
    // tests/test_automation_agents_handoff.py::test_new_folder_failure_does_not_open_an_agent_in_an_existing_project
    ActionsFixture fixture;
    // A file where the tasks folder goes: the folder can't be made.
    write_file(tasks_root(fixture.home), "not a folder");
    Desk desk;
    desk.handler("codex", true);
    const auto message = refusal("codex", WORDS, "auto", fixture.tmp, true);
    CHECK(message == "Couldn't create a new working folder. Check that ~/" +
                         std::string(identity().product) + " Agent Tasks is writable.");
    CHECK(desk.opened().empty());
    CHECK(desk.terminals().empty());
}

TEST_CASE("opencode opens a terminal session with the prompt", "[test_automation_agents_handoff]") {
    // from
    // tests/test_automation_agents_handoff.py::test_opencode_opens_a_terminal_session_with_the_prompt
    ActionsFixture fixture;
    Desk desk;
    desk.handler("opencode", true); // its desktop app documents no prompt link
    CHECK(handoff("opencode_cli", WORDS) == "Opened OpenCode in a terminal");
    CHECK(desk.terminals().at(0).argv ==
          std::vector<std::string>{str(fixture.opencode.path), "--prompt=" + WORDS});
    CHECK(desk.opened().empty());
    CHECK(refusal("opencode_cli", WORDS, "app").find("Terminal") != std::string::npos);
}

// ---- refusals ----

TEST_CASE("refusals", "[test_automation_agents_handoff]") {
    // from tests/test_automation_agents_handoff.py::test_refusals
    struct Row {
        std::string agent_id, text, surface;
        std::string folder; // "~/..." is under the fixture's home
        std::string message;
    };
    const auto row = GENERATE(Catch::Generators::values<Row>({
        {"pi", WORDS, "auto", "", "Choose Claude Code, Codex or OpenCode"},
        {"codex", "  \n", "auto", "", "nothing to send to Codex"},
        {"codex", WORDS, "window", "", "Choose where"},
        {"codex", WORDS, "auto", "relative/folder", "folder doesn't exist"},
        {"codex", WORDS, "auto", "~/no-such-folder-0w", "folder doesn't exist"},
    }));
    ActionsFixture fixture;
    const auto folder = row.folder.starts_with("~/") ? fixture.home / row.folder.substr(2)
                                                     : path_from_utf8(row.folder);
    Desk desk;
    desk.handler("codex", true);
    CHECK(refusal(row.agent_id, row.text, row.surface, folder).find(row.message) !=
          std::string::npos);
    CHECK(desk.opened().empty());
    CHECK(desk.terminals().empty());
}

TEST_CASE("a missing agent is named", "[test_automation_agents_handoff]") {
    // from tests/test_automation_agents_handoff.py::test_a_missing_agent_is_named
    ActionsFixture fixture;
    fixture.table->clear();
    Desk desk;
    CHECK(refusal("codex", WORDS) == "Codex isn't installed.");
}

TEST_CASE("terminal errors reach the user", "[test_automation_agents_handoff]") {
    // from tests/test_automation_agents_handoff.py::test_terminal_errors_reach_the_user
    // A .cmd that is no npm shim stays a script, which a terminal refuses.
    ActionsFixture fixture;
    const auto script = fixture.temp.path() / "agents" / "codex.cmd";
    write_file(script, "@ECHO off\r\ncodex-real.exe %*\r\n");
    fixture.table->set({"codex", script, "0.162.1"});
    Desk desk;
    const auto message = refusal("codex", WORDS);
    CHECK(message.find("script") != std::string::npos);
    CHECK(message.starts_with("codex.cmd is a script"));
}

// ---- detection ----

TEST_CASE("available reads the last scan without scanning", "[test_automation_agents_handoff]") {
    // from
    // tests/test_automation_agents_handoff.py::test_available_reads_the_last_scan_without_scanning
    {
        CHECK(agents::available().empty()); // no instance
    }
    ActionsFixture fixture;
    fixture.table->clear();
    fixture.table->set(fixture.claude);
    CHECK(agents::available().empty()); // no scan yet
    CHECK(fixture.table->lookups() == 0);
    fixture.table->scan_agents();
    const auto lookups = fixture.table->lookups();
    CHECK(agents::available() == std::map<std::string, bool>{{"claude_code", true},
                                                             {"codex", false},
                                                             {"opencode_cli", false}});
    CHECK(fixture.table->lookups() == lookups);
}

TEST_CASE("find uses the scan while its program exists", "[test_automation_agents_handoff]") {
    // from
    // tests/test_automation_agents_handoff.py::test_find_uses_the_scan_while_its_program_exists
    ActionsFixture fixture;
    const auto program = make_program(fixture.temp.path() / "scan" / "claude.exe");
    const agents::InstalledAgent scanned{"claude_code", program, "2.1.296"};
    const agents::InstalledAgent fresh{"claude_code", fixture.temp.path() / "elsewhere" / "claude",
                                       "2.1.300"};
    fixture.table->set(scanned);
    fixture.table->scan_agents();
    fixture.table->set(fresh);
    CHECK(agents::detail::find_installed("claude_code") == scanned);
    std::filesystem::remove(program);
    CHECK(agents::detail::find_installed("claude_code") == fresh);
}
