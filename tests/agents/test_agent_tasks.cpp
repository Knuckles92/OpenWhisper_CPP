// One-off tasks through an installed agent (meeting/agent/installed/core.py:323-439): the
// reading, viewing and agent tasks of tests/test_installed_agent_core.py over FakeDriver, where
// Python patches make_driver, and a reading task end to end with fv_fake_agent as claude.exe.

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <filesystem>
#include <map>
#include <memory>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include "agents/agent_tasks.h"
#include "agents/drivers.h"
#include "agents/testing/fake_driver.h"
#include "golden/golden.h"
#include "net/reactor.h"
#include "pass_support.h"

using fv::Json;
using fv::agents::AgentUnavailable;
using fv::agents::FakeDriver;
using fv::agents::InstalledAgent;
using fv::agents::PassOutcome;
using namespace fv::agents_test;

namespace {

bool contains(std::string_view text, std::string_view needle) {
    return text.find(needle) != std::string_view::npos;
}

// A driver the task owns that forwards to a FakeDriver the test keeps.
class Shared final : public fv::agents::Driver {
public:
    explicit Shared(std::shared_ptr<FakeDriver> fake) : fake_(std::move(fake)) {}
    std::string_view name() const override { return fake_->name(); }
    void start(fv::agents::LoopbackMcpServer& server, std::map<std::string, std::string> personas,
               const Json& tools) override {
        fake_->start(server, std::move(personas), tools);
    }
    bool healthy() const override { return fake_->healthy(); }
    void close() override { fake_->close(); }
    PassOutcome run_pass(const fv::agents::PassRequest& request) override {
        return fake_->run_pass(request);
    }
    std::string tool_prefix() const override { return fake_->tool_prefix(); }
    bool reads_folders() const override { return fake_->reads_folders(); }
    bool supports_read_folder() const override { return fake_->supports_read_folder(); }

private:
    std::shared_ptr<FakeDriver> fake_;
};

// The task services for one test: a reactor of its own, and the drivers `make` gives (the real
// ones when empty).
struct Services {
    fv::net::Reactor reactor{2};
    Services(std::shared_ptr<fv::ProcessLauncher> launcher, fv::agents::DriverFactory make = {}) {
        fv::agents::set_agent_task_services(reactor, std::move(launcher), std::move(make));
    }
    ~Services() { fv::agents::clear_agent_task_services(); }
    Services(const Services&) = delete;
    Services& operator=(const Services&) = delete;
};

// The `agent` fixture of the Python tests: Claude Code, signed in to Claude Team.
struct Tasks : AgentsFixture {
    std::shared_ptr<TableAgents> table = make<TableAgents>();
    ScopedAgents scoped{table};
    InstalledAgent found{"claude_code", "claude", "2.1.281"};
    std::shared_ptr<FakeDriver> fake;
    std::unique_ptr<Services> services;

    explicit Tasks(FakeDriver::Options options = {}) {
        found.account = "Claude Team";
        found.signed_in = true;
        // resolve_agent gives the same agent whatever is asked, as the Python fixture does.
        for (const auto* id : {"claude_code", "codex", "opencode_cli"}) {
            auto agent = found;
            agent.id = id;
            table->set(agent);
        }
        fake = std::make_shared<FakeDriver>(std::move(options));
        services = std::make_unique<Services>(launcher, [fake = fake](const InstalledAgent&) {
            return std::make_unique<Shared>(fake);
        });
    }
};

PassOutcome reading_task(const std::filesystem::path& folder = ".") {
    return fv::agents::run_reading_task("claude_code", "", "charter", "the pdf", folder, 60, 30);
}

PassOutcome viewing_task(std::string_view agent_id = "codex") {
    return fv::agents::run_viewing_task(agent_id, "", "charter", "the words",
                                        {fv::path_from_utf8("a.png"), fv::path_from_utf8("b.png")},
                                        60, 30);
}

template <class Body> std::string unavailable(Body body) {
    try {
        body();
    } catch (const AgentUnavailable& error) {
        return error.what();
    }
    return "<no AgentUnavailable>";
}

#ifdef _WIN32
// The 8.3 form of `path`, or `path` when the volume makes none.
std::filesystem::path short_form(const std::filesystem::path& path) {
    const auto size = GetShortPathNameW(path.c_str(), nullptr, 0);
    if (size == 0) {
        return path;
    }
    std::wstring out(size, L'\0');
    const auto written = GetShortPathNameW(path.c_str(), out.data(), size);
    out.resize(written);
    return written == 0 ? path : std::filesystem::path(out);
}
#else
std::filesystem::path short_form(const std::filesystem::path& path) {
    return path;
}
#endif

} // namespace

TEST_CASE("an unusable agent says why", "[test_installed_agent_core]") {
    // from tests/test_installed_agent_core.py::test_an_unusable_agent_says_why
    const auto row = GENERATE(0, 1, 2);
    AgentsFixture fixture;
    const auto table = fixture.make<TableAgents>();
    ScopedAgents scoped(table);
    std::string expected;
    if (row == 0) {
        expected = "not installed on this computer";
    } else if (row == 1) {
        InstalledAgent agent{"claude_code", "c", "1.0"};
        agent.problem = std::string(fv::identity().product) + " needs Claude Code 2.0.0";
        table->set(agent);
        expected = "needs Claude Code";
    } else {
        InstalledAgent agent{"claude_code", "c", "2.1"};
        agent.signed_in = false;
        table->set(agent);
        expected = "not signed in";
    }
    CHECK(contains(unavailable([] { fv::agents::installed_agent("claude_code"); }), expected));
}

TEST_CASE("a reading task hands claude code the folders long path", "[test_installed_agent_core]") {
    // from
    // tests/test_installed_agent_core.py::test_a_reading_task_hands_claude_code_the_folders_long_path
    // Python patches os.path.realpath; here the folder is given in its 8.3 form where the volume
    // makes one, and the request must carry the long one.
    Tasks tasks({.reads_folders = true,
                 .supports_read_folder = true,
                 .outcome = PassOutcome{.ok = true, .text = "# Write-up"}});
    const auto folder = tasks.temp.path() / "a walkthrough folder with a long name";
    std::filesystem::create_directories(folder);
    const auto outcome = reading_task(short_form(folder));
    CHECK(outcome.text == "# Write-up");
    const auto requests = tasks.fake->requests();
    REQUIRE(requests.size() == 1);
    const auto& request = requests[0];
    CHECK(request.read_folder == std::filesystem::canonical(folder));
    CHECK(fv::path_to_utf8(request.read_folder).find('~') == std::string::npos);
    CHECK(request.tools == Json::array());
    CHECK(request.user_prompt == "the pdf");
    CHECK(request.system_prompt == "charter");
    CHECK(request.effort == "medium");
    CHECK(request.stall_s == 30);
    CHECK(tasks.fake->closed());
}

TEST_CASE("a claude code that cannot be fenced is refused", "[test_installed_agent_core]") {
    // from tests/test_installed_agent_core.py::test_a_claude_code_that_cannot_be_fenced_is_refused
    Tasks tasks({.reads_folders = true, .supports_read_folder = false});
    const auto message = unavailable([] { reading_task(); });
    CHECK(contains(message, "claude update"));
    CHECK(message ==
          "Claude Code 2.1.281 is too old to read the walkthrough. Run `claude update`.");
    CHECK(tasks.fake->closed());
}

TEST_CASE("other agents cannot run reading tasks", "[test_installed_agent_core]") {
    // from tests/test_installed_agent_core.py::test_other_agents_cannot_run_reading_tasks
    Tasks tasks;
    CHECK(contains(unavailable([] { reading_task(); }), "Use Claude Code"));
    CHECK(tasks.fake->requests().empty());
}

TEST_CASE("a viewing task sends the pictures and offers no tools", "[test_installed_agent_core]") {
    // from
    // tests/test_installed_agent_core.py::test_a_viewing_task_sends_the_pictures_and_offers_no_tools
    Tasks tasks({.outcome = PassOutcome{.ok = true, .text = "# Write-up"}});
    const auto outcome = viewing_task();
    CHECK(outcome.text == "# Write-up");
    CHECK(tasks.fake->closed());
    // OpenCode fixes its system prompt per process, so it is the build persona.
    CHECK(tasks.fake->personas() == std::map<std::string, std::string>{{"build", "charter"}});
    CHECK(tasks.fake->start_tools() == Json::array());
    const auto requests = tasks.fake->requests();
    REQUIRE(requests.size() == 1);
    CHECK(requests[0].images == std::vector<std::filesystem::path>{fv::path_from_utf8("a.png"),
                                                                   fv::path_from_utf8("b.png")});
    CHECK(requests[0].tools == Json::array());
    CHECK(requests[0].read_folder.empty());
    CHECK(requests[0].user_prompt == "the words");
}

TEST_CASE("claude code reads the folder rather than viewing", "[test_installed_agent_core]") {
    // from
    // tests/test_installed_agent_core.py::test_claude_code_reads_the_folder_rather_than_viewing
    Tasks tasks({.reads_folders = true, .supports_read_folder = true});
    CHECK(contains(unavailable([] { viewing_task("claude_code"); }), "run_reading_task"));
}

TEST_CASE("the real drivers refuse the task they cannot run", "[agent_tasks]") {
    AgentsFixture fixture;
    const auto table = fixture.make<TableAgents>();
    ScopedAgents scoped(table);
    for (const auto* id : {"claude_code", "codex", "opencode_cli"}) {
        table->set({id, fixture.temp.path() / "agents" / id, "1.0"});
    }
    Services services(fixture.launcher);
    CHECK(unavailable([] { fv::agents::run_reading_task("codex", "", "s", "u", ".", 60, 30); }) ==
          "Codex can't be limited to one folder, so it can't read the walkthrough. Use "
          "Claude Code.");
    CHECK(unavailable([] {
              fv::agents::run_reading_task("opencode_cli", "", "s", "u", ".", 60, 30);
          }) == "OpenCode can't be limited to one folder, so it can't read the walkthrough. Use "
                "Claude Code.");
    CHECK(unavailable([] {
              fv::agents::run_viewing_task("claude_code", "", "s", "u", {}, 60, 30);
          }) == "Claude Code reads the folder instead: use run_reading_task.");
    CHECK(unavailable([] { fv::agents::installed_agent("pi"); }) == "Unknown meeting agent 'pi'.");
    // Nothing was started.
    CHECK(fixture.launcher->specs().empty());
}

TEST_CASE("an agent task answers through the task's own tools", "[agent_tasks]") {
    Tasks tasks({.tool_prefix = "mcp__x__",
                 .calls = {{"search_transcript", Json{{"query", "beta"}}}},
                 .outcome = PassOutcome{.ok = true, .text = "# Exec brief"}});
    std::vector<std::tuple<std::string, Json, Json>> seen;
    const Json tools =
        Json::array({Json{{"name", "search_transcript"},
                          {"inputSchema", {{"properties", {{"query", {{"type", "string"}}}}}}}}});
    const auto outcome = fv::agents::run_agent_task(
        "claude_code", "haiku", "A brief", "the meeting", tools,
        [&](std::string_view name, const Json& args, const Json& meta) {
            seen.emplace_back(std::string(name), args, meta);
            return std::pair<std::string, bool>("beta found", false);
        },
        120, "Reply with the finished report.");
    CHECK(outcome.text == "# Exec brief");
    REQUIRE(seen.size() == 1);
    CHECK(std::get<0>(seen[0]) == "search_transcript");
    CHECK(std::get<1>(seen[0]) == Json{{"query", "beta"}});
    CHECK(std::get<2>(seen[0]) == Json::object());
    const auto requests = tasks.fake->requests();
    REQUIRE(requests.size() == 1);
    const auto charter =
        "A brief\n\n" + fv::agents::tool_contract(tools, "Claude Code", "mcp__x__",
                                                  "Reply with the finished report.");
    CHECK(requests[0].system_prompt == charter);
    CHECK(tasks.fake->personas() == std::map<std::string, std::string>{{"build", charter}});
    CHECK(tasks.fake->start_tools() == tools);
    CHECK(requests[0].effort == "medium");
    CHECK(requests[0].model == "haiku");
    CHECK(requests[0].stall_s == fv::agents::CONSOLIDATION_STALL_S);
    CHECK(requests[0].timeout_s == 120);
    CHECK(tasks.fake->closed());
}

TEST_CASE("a task without its services set says so", "[agent_tasks]") {
    AgentsFixture fixture;
    const auto table = fixture.make<TableAgents>();
    ScopedAgents scoped(table);
    table->set({"claude_code", "claude", "2.1.306"});
    fv::agents::clear_agent_task_services();
    CHECK_THROWS_AS(fv::agents::run_reading_task("claude_code", "", "s", "u", ".", 60, 30),
                    std::logic_error);
}

TEST_CASE("a reading task through claude code runs the golden command line",
          "[agent_tasks][platform]") {
    // The acceptance: run_reading_task("claude_code", ...) against the fake agent returns its
    // reply with ok, and the fake's argv is the command line OpenWhisper builds for the same
    // request (claude_command's fenced case in the vectors).
    FV_PASS_PROCESSES();
    PassAgents fake;
    const auto& vector = fv::golden::load("meeting.agent.installed.drivers")
                             .at("functions")
                             .at("claude_command")
                             .at(2);
    const auto help = vector.at("args").at(0).get<std::string>();
    const auto& values = vector.at("args").at(1);
    fake.set("FV_FAKE_AGENT_HELP", help);
    fake.set("FV_FAKE_AGENT_RESULT", "# Write-up");
    Services services(fake.launcher);
    const auto folder = fake.temp.path() / "capture";
    std::filesystem::create_directories(folder);
    const auto outcome = fv::agents::run_reading_task(
        "claude_code", values.at("model").get<std::string>(),
        values.at("system_prompt").get<std::string>(), values.at("user_prompt").get<std::string>(),
        folder, 60, 30, values.at("effort").get<std::string>());
    INFO(outcome.error);
    CHECK(outcome.ok);
    CHECK(outcome.text == "# Write-up");
    const auto seen = fake.recorded();
    auto argv = seen.at("argv").get<std::vector<std::string>>();
    argv.insert(argv.begin(), "claude.exe");
    const auto workdir = fv::path_from_utf8(after_flag(argv, "--mcp-config")).parent_path();
    for (auto& value : argv) {
        if (value.starts_with(fv::path_to_utf8(workdir))) {
            value = "<workdir>" + value.substr(fv::path_to_utf8(workdir).size());
        }
        if (fv::path_from_utf8(value) == std::filesystem::canonical(folder)) {
            value = values.at("read_folder").get<std::string>();
        }
    }
    CHECK(Json(argv) == rebranded(vector.at("result").at("argv")));
    CHECK(seen.at("files").at("--system-prompt-file") == values.at("system_prompt"));
    CHECK(seen.at("prompt") == values.at("user_prompt"));
    CHECK(fake.pass_folders_removed());
    CHECK_FALSE(std::filesystem::exists(workdir));
}

TEST_CASE("a viewing task through codex sends its pictures", "[agent_tasks][platform]") {
    FV_PASS_PROCESSES();
    PassAgents fake;
    fake.set("FV_FAKE_AGENT_RESULT", "# Write-up");
    const auto shot = fake.temp.path() / "shot-1.png";
    write_file(shot, "\x89PNG fake");
    Services services(fake.launcher);
    const auto outcome =
        fv::agents::run_viewing_task("codex", "", "Write it up.", "the words", {shot}, 60, 30);
    INFO(outcome.error);
    CHECK(outcome.ok);
    CHECK(outcome.text == "# Write-up");
    const auto seen = fake.recorded();
    CHECK(seen.at("images") == Json::array({fv::path_to_utf8(shot)}));
    const auto argv = seen.at("argv").get<std::vector<std::string>>();
    CHECK(has_arg(argv, "--disable"));
    CHECK(after_flag(argv, "-s") == "read-only");
    CHECK(fake.pass_folders_removed());
}
