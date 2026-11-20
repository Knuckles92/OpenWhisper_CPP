// Opening links and terminals: OpenWhisper's tests/test_automation_agents_launch.py,
// tests/test_automation_fix_actions_launch.py and tests/test_automation_r2c_launch.py, the
// Windows parts. Nothing real opens: the association lookup, Explorer and the console start are
// FakeLaunchBackend; the [platform] tests only read the real association table.

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <algorithm>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "base/identity.h"
#include "credentials/dotenv.h"
#include "process/launch.h"
#include "process/process_env.h"
#include "process/testing/fake_launch_backend.h"
#include "process_test_support.h"
#include "testkit/testkit.h"

using Catch::Matchers::ContainsSubstring;
using fv::AssocString;
using fv::FakeLaunchBackend;
using fv::LaunchError;
using fv::process_test::ScopedLaunchBackend;

namespace {

constexpr std::uint32_t kNoAssociation = 0x80070483;
constexpr std::uint32_t kNotFound = 0x80070490;
constexpr std::uint32_t kFailed = 0x80004005;
constexpr std::uint32_t kAppNotFound = 0x800401F5;

// Shell metacharacters, quotes, a newline and option-like words.
const std::string kWords = "It's $(rm -rf ~) `id` \"quoted\" & | ; %PATH% ^\nsecond line -- --help";

struct Fixture {
    std::shared_ptr<FakeLaunchBackend> backend = std::make_shared<FakeLaunchBackend>();
    ScopedLaunchBackend installed{backend};
    fv::testkit::TempDataRoot root;
};

std::string message_of(const std::function<void()>& call) {
    try {
        call();
    } catch (const LaunchError& error) {
        return error.what();
    }
    FAIL("no LaunchError");
    return {};
}

std::string home_variable() {
    return fv::ProcessEnv::kWindows ? "USERPROFILE" : "HOME";
}

} // namespace

// ---- handler detection: Windows -------------------------------------------------------------

// from
// tests/test_automation_agents_launch.py::test_windows_classic_handler_counts_only_while_its_program_exists
TEST_CASE("windows classic handler counts only while its program exists",
          "[test_automation_agents_launch]") {
    Fixture f;
    const auto exe = f.root.path() / "claude.exe";
    fv::process_test::write_file(exe, "");
    const auto exe_text = fv::path_to_utf8(exe);
    f.backend->set_assoc_function([&](std::string_view, AssocString what) -> fv::AssocAnswer {
        // AssocQueryStringW checks the program: CO_E_APPNOTFOUND once it's gone.
        if (what == AssocString::command) {
            return {0, "\"" + exe_text + "\" --handle-uri \"%1\""};
        }
        if (what == AssocString::executable) {
            return std::filesystem::exists(exe) ? fv::AssocAnswer{0, exe_text}
                                                : fv::AssocAnswer{kAppNotFound, ""};
        }
        return {kNoAssociation, ""};
    });
    CHECK(fv::handler_available("claude-cli") == true);
    std::filesystem::remove(exe); // an uninstall that left its registration behind
    fv::forget_handlers();
    CHECK(fv::handler_available("claude-cli") == false);
}

// from tests/test_automation_agents_launch.py::test_windows_packaged_app_protocol_counts
TEST_CASE("windows packaged app protocol counts", "[test_automation_agents_launch]") {
    // MSIX apps (the ChatGPT app's codex://) leave only a bare "URL Protocol" key; the
    // association resolves to the AppX protocol handler, a COM object.
    Fixture f;
    f.backend->set_assoc("codex", AssocString::delegate_execute, 0,
                         "{A56A841F-E974-45C1-8001-7E3F8A085917}");
    CHECK(fv::handler_available("codex") == true);
}

// from tests/test_automation_agents_launch.py::test_windows_without_a_handler
TEST_CASE("windows without a handler", "[test_automation_agents_launch]") {
    Fixture f;
    SECTION("unknown scheme") {
        CHECK(fv::handler_available("stale") == false);
    }
    SECTION("key, no handler") {
        f.backend->set_assoc("stale", AssocString::command, kNoAssociation);
        f.backend->set_assoc("stale", AssocString::delegate_execute, kNotFound);
        CHECK(fv::handler_available("stale") == false);
    }
    SECTION("lookup failed") {
        f.backend->set_assoc("stale", AssocString::command, kFailed);
        f.backend->set_assoc("stale", AssocString::delegate_execute, kFailed);
        CHECK(fv::handler_available("stale") == std::nullopt);
    }
}

#ifdef _WIN32
// from tests/test_automation_agents_launch.py::test_windows_lookup_answers_for_an_unknown_scheme
TEST_CASE("windows lookup answers for an unknown scheme",
          "[test_automation_agents_launch][platform]") {
    fv::set_launch_backend(nullptr);
    const std::string scheme = "fiorivoce-no-such-scheme";
    CHECK(fv::launch_backend()->assoc_query(scheme, AssocString::command).result == kNoAssociation);
    CHECK(fv::handler_available(scheme) == false);
}
#endif

// ---- handler cache --------------------------------------------------------------------------

// from tests/test_automation_agents_launch.py::test_handler_answers_are_cached_for_a_minute
TEST_CASE("handler answers are cached for a minute", "[test_automation_agents_launch]") {
    Fixture f;
    f.backend->set_assoc("claude", AssocString::delegate_execute, 0, "{handler}");
    const auto platform_lookups = [&] {
        std::size_t count = 0;
        for (const auto& [scheme, what] : f.backend->lookups()) {
            count += what == AssocString::delegate_execute ? 1 : 0;
        }
        return count;
    };
    CHECK(fv::handler_available("claude") == true);
    CHECK(fv::handler_available("Claude:") == true); // the same scheme
    CHECK(platform_lookups() == 1);
    f.backend->advance(std::chrono::seconds(61));
    f.backend->clear_assoc();
    CHECK(fv::handler_available("claude") == false);
    CHECK(platform_lookups() == 2);
}

// from tests/test_automation_agents_launch.py::test_handler_lookup_never_raises
TEST_CASE("handler lookup never raises", "[test_automation_agents_launch]") {
    Fixture f;
    f.backend->set_assoc_function(
        [](std::string_view, AssocString) -> fv::AssocAnswer { throw std::runtime_error("boom"); });
    CHECK(fv::handler_available("claude") == std::nullopt);
}

// from tests/test_automation_agents_launch.py::test_malformed_schemes_have_no_handler
TEST_CASE("malformed schemes have no handler", "[test_automation_agents_launch]") {
    Fixture f;
    const auto scheme = GENERATE(as<std::string>{}, "", "has space", "1claude", "claude://x");
    CAPTURE(scheme);
    CHECK(fv::handler_available(scheme) == false);
    CHECK(f.backend->lookups().empty());
}

// ---- terminals: common checks ---------------------------------------------------------------

// from tests/test_automation_agents_launch.py::test_terminal_refuses_bad_input
TEST_CASE("terminal refuses bad input", "[test_automation_agents_launch]") {
    Fixture f;
    CHECK_THROWS_AS(fv::open_in_terminal({}), LaunchError);
    CHECK_THAT(
        message_of([&] { fv::open_in_terminal({"agent.exe", "hi"}, f.root.path() / "missing"); }),
        ContainsSubstring("folder"));
    CHECK_THROWS_AS(fv::open_in_terminal({"agent.exe", std::string("a\0b", 3)}, f.root.path()),
                    LaunchError);
    CHECK(f.backend->consoles().empty());
}

// A19-57 and A19-29: the refusals' exact copy (launch.py:open_in_terminal, open_uri).
TEST_CASE("terminal and link refusals say why in OpenWhisper's words",
          "[test_automation_agents_launch]") {
    Fixture f;
    CHECK(message_of([&] { fv::open_in_terminal({}); }) == "There's no program to open.");
    CHECK(message_of([&] { fv::open_in_terminal({""}); }) == "There's no program to open.");
    CHECK(message_of([&] {
              fv::open_in_terminal({"agent.exe", std::string("a\0b", 3)}, f.root.path());
          }) == "Those words can't be passed to a program.");
    CHECK(message_of([&] {
              fv::open_in_terminal({"agent.exe", "hi"}, f.root.path() / "missing");
          }) == "The working folder doesn't exist.");
    CHECK(message_of([&] { fv::open_uri("https://example.com/?q=" + std::string(30'000, 'a')); }) ==
          "That's too long to send in a link. Try fewer words.");
    CHECK(message_of([&] { fv::open_uri("https://example.com/\r\nx"); }) ==
          "That link can't be opened.");
    CHECK(f.backend->consoles().empty());
    CHECK(f.backend->explorer_opened().empty());
}

// from
// tests/test_automation_agents_launch.py::test_terminal_dispatches_with_home_as_the_default_folder
TEST_CASE("terminal dispatches with home as the default folder",
          "[test_automation_agents_launch]") {
    Fixture f;
    fv::process_test::ScopedEnv home(home_variable(), fv::path_to_utf8(f.root.path()));
    fv::open_in_terminal({"agent.exe", kWords});
    const auto consoles = f.backend->consoles();
    REQUIRE(consoles.size() == 1);
    CHECK(consoles[0].program == fv::path_from_utf8("agent.exe"));
    CHECK(consoles[0].args == std::vector<std::string>{kWords});
    CHECK(consoles[0].cwd == f.root.path());
}

// ---- terminals: Windows -------------------------------------------------------------------

// from
// tests/test_automation_agents_launch.py::test_windows_refuses_scripts_that_would_parse_the_words
TEST_CASE("windows refuses scripts that would parse the words", "[test_automation_agents_launch]") {
    Fixture f;
    const auto program = GENERATE(as<std::string>{}, "claude.cmd", "codex.BAT", "opencode.ps1");
    CAPTURE(program);
    const auto path = fv::path_to_utf8(f.root.path() / program);
    const auto message = message_of([&] { fv::open_in_terminal({path, kWords}, f.root.path()); });
    CHECK_THAT(message, ContainsSubstring("script"));
    CHECK(message.starts_with(program + " is a script, and Windows would read your words as "
                                        "commands on the way in. Install the program's own .exe "
                                        "build, or open it through its app instead."));
    CHECK(f.backend->consoles().empty());
}

// from tests/test_automation_agents_launch.py::test_windows_refuses_non_programs
TEST_CASE("windows refuses non programs", "[test_automation_agents_launch]") {
    Fixture f;
    const auto message = message_of([&] {
        fv::open_in_terminal({fv::path_to_utf8(f.root.path() / "claude"), "hi"}, f.root.path());
    });
    CHECK(message == "claude isn't a program " + std::string(fv::identity().product) +
                         " can open in a terminal.");
    CHECK(f.backend->consoles().empty());
}

// from tests/test_automation_agents_launch.py::test_windows_starts_the_exe_in_a_new_console
TEST_CASE("windows starts the exe in a new console", "[test_automation_agents_launch]") {
    Fixture f;
    const auto program = fv::path_to_utf8(f.root.path() / "claude.exe");
    fv::open_in_terminal({program, "--", kWords}, f.root.path(),
                         std::map<std::string, std::string>{{"A", "1"}});
    const auto consoles = f.backend->consoles();
    REQUIRE(consoles.size() == 1);
    const auto& spec = consoles[0];
    CHECK(fv::path_to_utf8(spec.program) == program);
    CHECK(spec.args == std::vector<std::string>{"--", kWords});
    CHECK(spec.cwd == f.root.path());
    CHECK(spec.env == std::map<std::string, std::string>{{"A", "1"}});
    CHECK(spec.new_console);
    // The console's own handles: no pipes, no NUL, no job.
    CHECK_FALSE(spec.pipes);
    CHECK_FALSE(spec.stdin_null);
    CHECK_FALSE(spec.own_tree);
}

// from tests/test_automation_agents_launch.py::test_windows_refuses_a_command_line_over_the_limit
TEST_CASE("windows refuses a command line over the limit", "[test_automation_agents_launch]") {
    Fixture f;
    const auto message = message_of([&] {
        fv::open_in_terminal(
            {fv::path_to_utf8(f.root.path() / "claude.exe"), std::string(40'000, 'x')},
            f.root.path());
    });
    CHECK(message == "That's too long to open in a terminal. Try a shorter request.");
    CHECK(f.backend->consoles().empty());
}

TEST_CASE("a terminal that couldn't start says which program", "[process]") {
    Fixture f;
    f.backend->set_console_result(false);
    const auto message = message_of([&] {
        fv::open_in_terminal({fv::path_to_utf8(f.root.path() / "claude.exe")}, f.root.path());
    });
    CHECK(message == "claude.exe couldn't be started in a terminal.");
}

// ---- tests/test_automation_fix_actions_launch.py ---------------------------------------------

// from
// tests/test_automation_fix_actions_launch.py::test_a_terminal_without_an_env_gets_the_users_without_openwhispers_keys
TEST_CASE("a terminal without an env gets the user's without the app's keys",
          "[test_automation_fix_actions_launch]") {
    Fixture f;
    // A key the app's .env holds, which the editor that started it also set; no child may
    // inherit it.
    fv::process_test::ScopedEnv key("OPENAI_API_KEY", "sk-from-the-apps-dotenv");
    fv::process_test::ScopedEnv own("USER_OWN_VAR", "kept");
    const auto dotenv_file = f.root.path() / ".env";
    fv::process_test::write_file(dotenv_file, "OPENAI_API_KEY=sk-from-the-apps-dotenv\n");
    auto& process_env = fv::ProcessEnv::instance();
    process_env.forget();
    process_env.adopt_dotenv(fv::DotEnv(dotenv_file));

    fv::open_in_terminal({"agent.exe", "hi"}, f.root.path());
    process_env.forget();
    const auto consoles = f.backend->consoles();
    REQUIRE(consoles.size() == 1);
    const auto& env = consoles[0].env;
    const auto named = [&](std::string_view name) {
        return std::any_of(env.begin(), env.end(), [&](const auto& item) {
            return fv::EnvNameLess{}(item.first, name) == fv::EnvNameLess{}(name, item.first);
        });
    };
    CHECK_FALSE(named("OPENAI_API_KEY"));
    CHECK(named("USER_OWN_VAR"));
}

// from tests/test_automation_fix_actions_launch.py::test_a_link_too_long_for_any_handler_says_so
TEST_CASE("a link too long for any handler says so", "[test_automation_fix_actions_launch]") {
    Fixture f;
    f.backend->set_assoc("claude", AssocString::delegate_execute, 0, "{handler}");
    const auto uri = "claude://claude.ai/new?q=" + std::string(fv::LINK_MAX_CHARS, 'a');
    const auto message = message_of([&] { fv::open_uri(uri); });
    CHECK_THAT(message, ContainsSubstring("too long"));
    CHECK_FALSE(message.find("No app") != std::string::npos);
    CHECK(f.backend->explorer_opened().empty());
}

// from tests/test_automation_fix_actions_launch.py::test_a_missing_handler_names_the_app_to_install
TEST_CASE("a missing handler names the app to install", "[test_automation_fix_actions_launch]") {
    Fixture f;
    const auto [scheme, app] = GENERATE(table<std::string, std::string>({
        {"claude", "the Claude app"},
        {"codex", "the ChatGPT app"},
        {"claude-cli", "Claude Code"},
    }));
    CAPTURE(scheme);
    const auto message = message_of([&] { fv::open_uri(scheme + "://new?q=hello"); });
    CHECK(message == "No app on this computer opens " + scheme + ": links. Install " + app + ".");
    CHECK(f.backend->explorer_opened().empty());
}

// from
// tests/test_automation_fix_actions_launch.py::test_an_unknown_scheme_without_a_handler_is_not_opened
TEST_CASE("an unknown scheme without a handler is not opened",
          "[test_automation_fix_actions_launch]") {
    Fixture f;
    const auto message = message_of([&] { fv::open_uri("x-notes://add?text=hi"); });
    CHECK(message == "No app on this computer opens x-notes: links.");
    CHECK(f.backend->explorer_opened().empty());
}

// from tests/test_automation_fix_actions_launch.py::test_web_links_and_unknown_handlers_still_open
TEST_CASE("web links and unknown handlers still open", "[test_automation_fix_actions_launch]") {
    Fixture f;
    // The claude lookup can't tell: let the system try.
    f.backend->set_assoc("claude", AssocString::command, kFailed);
    f.backend->set_assoc("claude", AssocString::delegate_execute, kFailed);
    fv::open_uri("https://chatgpt.com/?q=hi");
    fv::open_uri("claude://claude.ai/new?q=hi");
    const auto opened = f.backend->explorer_opened();
    REQUIRE(opened.size() == 2);
    CHECK(opened[0].first == "https://chatgpt.com/?q=hi");
    CHECK(opened[1].first == "claude://claude.ai/new?q=hi");
    CHECK(f.backend->opened_here().empty());
}

// from tests/test_automation_fix_actions_launch.py::test_the_limits_are_the_ones_agent_links_use
// (the agent side, services/actions/agents.py, is M5.7's and checks its constants against these)
TEST_CASE("the limits are the ones agent links use", "[test_automation_fix_actions_launch]") {
    CHECK(fv::LINK_MAX_CHARS == 30'000);
    CHECK(fv::CLAUDE_APP_TEXT_MAX == 14'000);
}

// from
// tests/test_automation_fix_actions_launch.py::test_the_handler_lookup_asks_for_the_delegate_execute_handler
TEST_CASE("the handler lookup asks for the delegate execute handler",
          "[test_automation_fix_actions_launch]") {
    // ASSOCSTR_SUPPORTED_URI_PROTOCOLS is 19.
    CHECK(static_cast<int>(AssocString::delegate_execute) == 18);
    Fixture f;
    fv::handler_available("anything");
    const auto lookups = f.backend->lookups();
    REQUIRE_FALSE(lookups.empty());
    CHECK(lookups.front().second == AssocString::delegate_execute);
}

// ---- tests/test_automation_r2c_launch.py -----------------------------------------------------

namespace {

constexpr const char* kAppxProtocol = "{A56A841F-E974-45C1-8001-7E3F8A085917}";
constexpr const char* kWindowsPages = "{4ed3a719-cea8-4bd9-910d-e252f997afc2}";
constexpr const char* kClaudeCode = R"(C:\Users\u\.local\bin\claude.exe)";

// How schemes are registered: a classic command, a packaged app, Windows' own pages; what
// AssocQueryStringW answers on a real Windows 11.
void registered(FakeLaunchBackend& backend) {
    backend.set_assoc("claude-cli", AssocString::command, 0,
                      std::string("\"") + kClaudeCode + "\" --handle-uri \"%1\"");
    backend.set_assoc("claude-cli", AssocString::executable, 0, kClaudeCode);
    backend.set_assoc("codex", AssocString::delegate_execute, 0, kAppxProtocol);
    backend.set_assoc("ms-settings", AssocString::delegate_execute, 0, kWindowsPages);
    // Both registered: ShellExecute uses the COM object.
    backend.set_assoc("tel", AssocString::command, 0, R"(C:\WINDOWS\system32\OpenWith.exe "%1")");
    backend.set_assoc("tel", AssocString::delegate_execute, 0,
                      "{e44e9428-bdbc-4987-a099-40dc8fd255e7}");
}

} // namespace

// from tests/test_automation_r2c_launch.py::test_a_com_handler_counts
TEST_CASE("a com handler counts", "[test_automation_r2c_launch]") {
    Fixture f;
    registered(*f.backend);
    const auto scheme = GENERATE(as<std::string>{}, "ms-settings", "codex", "tel");
    CAPTURE(scheme);
    fv::forget_handlers();
    CHECK(fv::handler_available(scheme) == true);
}

// from tests/test_automation_r2c_launch.py::test_only_no_handler_at_all_is_refused
TEST_CASE("only no handler at all is refused", "[test_automation_r2c_launch]") {
    Fixture f;
    SECTION("nothing at all") {
        CHECK(fv::handler_available("bingmaps") == false);
    }
    SECTION("a bare key") {
        f.backend->set_assoc("bingmaps", static_cast<AssocString>(20), 0, "bingmaps");
        CHECK(fv::handler_available("bingmaps") == false);
    }
    SECTION("lookup failed") {
        f.backend->set_assoc("bingmaps", AssocString::command, kFailed);
        f.backend->set_assoc("bingmaps", AssocString::delegate_execute, kFailed);
        CHECK(fv::handler_available("bingmaps") == std::nullopt);
    }
}

#ifdef _WIN32
// from
// tests/test_automation_r2c_launch.py::test_a_worker_thread_sees_windows_own_packaged_protocols
TEST_CASE("a worker thread sees windows own packaged protocols",
          "[test_automation_r2c_launch][platform]") {
    // Windows reads some packaged apps' protocols (ms-actioncenter:, the notification centre)
    // only through COM; a worker thread without COM was told they had no handler. Read-only:
    // nothing is opened.
    fv::set_launch_backend(nullptr);
    const std::string scheme = "ms-actioncenter";
    std::uint32_t with_com = 1;
    std::thread([&] {
        with_com = fv::launch_backend()->assoc_query(scheme, AssocString::delegate_execute).result;
    }).join();
    if (with_com != 0) {
        SKIP(scheme + ": isn't registered on this computer");
    }
    std::optional<bool> found;
    std::thread([&] { found = fv::handler_available(scheme); }).join();
    CHECK(found == true);
}
#endif

// from tests/test_automation_r2c_launch.py::test_open_uri_on_windows_opens_the_link_as_it_is
TEST_CASE("open_uri on windows opens the link as it is", "[test_automation_r2c_launch]") {
    Fixture f;
    f.backend->set_assoc("claude-cli", AssocString::delegate_execute, 0, "{handler}");
    fv::open_uri("Claude-CLI://open?q=hi");
    const auto opened = f.backend->explorer_opened();
    REQUIRE(opened.size() == 1);
    CHECK(opened[0].first == "Claude-CLI://open?q=hi");
    CHECK(f.backend->lookups().front().first == "claude-cli");
}

TEST_CASE("a link that can't be a link is refused", "[process]") {
    Fixture f;
    for (const std::string& uri :
         {std::string(), std::string("https://x/\r\n"), std::string("https://x/\0", 11)}) {
        CHECK(message_of([&] { fv::open_uri(uri); }) == "That link can't be opened.");
    }
    CHECK(f.backend->explorer_opened().empty());
}
