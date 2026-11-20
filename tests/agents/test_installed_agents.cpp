// Finding installed coding agents, their sign-in, their config, and models: OpenWhisper's
// tests/test_installed_agents.py, over a temporary home with program files that only the
// scripted launcher "runs".

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <tuple>
#include <vector>

#include "agents/agent_config.h"
#include "agents/installed_agents.h"
#include "agents/testing/fake_installed_agents.h"
#include "agents_test_support.h"
#include "base/executor.h"
#include "base/identity.h"
#include "base/testing/manual_executor.h"
#include "base/utf8.h"
#include "process/process_env.h"
#include "process/testing/map_env.h"
#include "pyport/json.h"

using fv::agents::CLAUDE_CODE;
using fv::agents::CODEX;
using fv::agents::InstalledAgent;
using fv::agents::OPENCODE;
using fv::agents_test::AgentsFixture;
using fv::agents_test::CountingAgents;
using fv::agents_test::exe;
using fv::agents_test::Gate;
using fv::agents_test::make_program;
using fv::agents_test::Reply;
using fv::agents_test::write_file;

namespace {

std::array<std::int64_t, 4> key_of(std::string_view text) {
    return fv::agents::parse_version(text).key.value();
}

InstalledAgent agent(std::string_view id, const std::filesystem::path& path, std::string version,
                     std::optional<bool> signed_in = std::nullopt) {
    return InstalledAgent{
        .id = std::string(id), .path = path, .version = std::move(version), .signed_in = signed_in};
}

std::vector<std::string> values(const std::vector<fv::agents::AgentModel>& models) {
    std::vector<std::string> out;
    for (const auto& model : models) {
        out.push_back(model.value);
    }
    return out;
}

} // namespace

// from tests/test_installed_agents.py::test_parse_version
TEST_CASE("parse version", "[test_installed_agents]") {
    using Key = std::optional<std::array<std::int64_t, 4>>;
    const auto [text, expected] = GENERATE(table<std::string, Key>({
        {"2.1.281 (Claude Code)", std::array<std::int64_t, 4>{2, 1, 281, 1}},
        {"codex-cli 0.158.0-alpha.2.1", std::array<std::int64_t, 4>{0, 158, 0, 0}},
        {"opencode 2.0", std::array<std::int64_t, 4>{2, 0, 0, 1}},
        {"no version here", std::nullopt},
    }));
    CAPTURE(text);
    CHECK(fv::agents::parse_version(text).key == expected);
}

// from
// tests/test_installed_agents.py::test_a_prerelease_sorts_before_its_release_but_after_older_releases
TEST_CASE("a prerelease sorts before its release but after older releases",
          "[test_installed_agents]") {
    const auto alpha = key_of("0.158.0-alpha.2");
    CHECK(key_of("0.146.0") < alpha);
    CHECK(alpha < key_of("0.158.0"));
}

// from tests/test_installed_agents.py::test_find_agent_runs_the_newest_install
TEST_CASE("find agent runs the newest install", "[test_installed_agents]") {
    // The Codex desktop app bundles a newer CLI than the one on PATH.
    AgentsFixture fx;
    const auto on_path = make_program(exe(fx.path_dir, "codex"));
    const auto bundle =
        make_program(fx.local / "OpenAI" / "Codex" / "bin" / "abc123" / "codex.exe");
    fx.launcher->reply(on_path, {"--version"}, {.out = "codex-cli 0.146.0\n"});
    fx.launcher->reply(bundle, {"--version"}, {.out = "codex-cli 0.158.0-alpha.2.1\n"});
    auto agents = fx.make();

    const auto found = agents->find_agent(CODEX);
    REQUIRE(found);
#ifdef _WIN32
    CHECK(found->path == bundle);
    CHECK(found->version == "0.158.0-alpha.2.1");
#else
    CHECK(found->path == on_path);
#endif
    CHECK(found->problem.empty());
    fx.check_launcher_stayed_inside();
}

// from tests/test_installed_agents.py::test_find_agent_flags_a_version_too_old
TEST_CASE("find agent flags a version too old", "[test_installed_agents]") {
    AgentsFixture fx;
    const auto program = make_program(exe(fx.home / ".opencode" / "bin", "opencode"));
    fx.launcher->reply(program, {"--version"}, {.out = "1.4.2"});
    auto agents = fx.make();

    const auto found = agents->find_agent(OPENCODE);
    REQUIRE(found);
    CHECK(found->problem.find("2.0.0 or newer") != std::string::npos);
    CHECK(found->problem == std::string(fv::identity().product) +
                                " needs OpenCode 2.0.0 or newer. Run `opencode upgrade`.");
    CHECK_FALSE(found->usable());
    // An agent with a version problem is not asked for its sign-in.
    CHECK(agents->with_sign_in(*found) == *found);
    CHECK(fx.launcher->specs().size() == 1);
}

// from tests/test_installed_agents.py::test_find_agent_returns_none_when_nothing_answers
TEST_CASE("find agent returns none when nothing answers", "[test_installed_agents]") {
    AgentsFixture fx;
    const auto broken = make_program(exe(fx.home / ".local" / "bin", "claude"));
    const auto silent = make_program(exe(fx.path_dir, "claude"));
    fx.launcher->reply(broken, {"--version"}, {.code = 1, .out = "2.1.0"});
    // `silent` is not scripted: it can't start.
    auto agents = fx.make();

    CHECK_FALSE(agents->find_agent(CLAUDE_CODE));
    CHECK(fx.launcher->count(broken, {"--version"}) == 1);
    CHECK(fx.launcher->count(silent, {"--version"}) == 1);
}

// from tests/test_installed_agents.py::test_claude_sign_in
TEST_CASE("claude sign in", "[test_installed_agents]") {
    const auto [status, signed_in, account] =
        GENERATE(table<std::string, std::optional<bool>, std::string>({
            {R"({"loggedIn": true, "authMethod": "claude.ai", "subscriptionType": "team"})", true,
             "Claude Team"},
            {R"({"loggedIn": true, "apiProvider": "bedrock"})", true, "Amazon Bedrock"},
            {R"({"loggedIn": true, "authMethod": "api_key"})", true, "API key"},
            {R"({"loggedIn": false})", false, ""},
        }));
    AgentsFixture fx;
    const auto program = make_program(exe(fx.path_dir, "claude"));
    fx.launcher->reply(program, {"auth", "status"}, {.out = status});
    auto agents = fx.make();

    const auto result = agents->with_sign_in(agent(CLAUDE_CODE, program, "2.1.281"));
    CHECK(result.signed_in == signed_in);
    CHECK(result.account == account);
}

// from tests/test_installed_agents.py::test_codex_sign_in
TEST_CASE("codex sign in", "[test_installed_agents]") {
    const auto [output, code, signed_in, account] =
        GENERATE(table<std::string, int, std::optional<bool>, std::string>({
            {"Logged in using ChatGPT", 0, true, "ChatGPT"},
            {"Logged in using an API key", 0, true, "API key"},
            {"Not logged in", 1, false, ""},
        }));
    AgentsFixture fx;
    const auto program = make_program(exe(fx.path_dir, "codex"));
    // `codex login status` writes to stderr; stdout and stderr are read together.
    fx.launcher->reply(program, {"login", "status"}, {.code = code, .err = output + "\n"});
    auto agents = fx.make();

    const auto result = agents->with_sign_in(agent(CODEX, program, "0.158.0"));
    CHECK(result.signed_in == signed_in);
    CHECK(result.account == account);
}

// from tests/test_installed_agents.py::test_opencode_sign_in_stays_unknown
TEST_CASE("opencode sign in stays unknown", "[test_installed_agents]") {
    // Reading it would start OpenCode's background service.
    AgentsFixture fx;
    auto agents = fx.make();
    const auto result = agents->with_sign_in(agent(OPENCODE, "oc", "2.0.18"));
    CHECK_FALSE(result.signed_in.has_value());
    CHECK(result.usable());
    CHECK(fx.launcher->specs().empty());
}

// from tests/test_installed_agents.py::test_configured_default_models
TEST_CASE("configured default models", "[test_installed_agents]") {
    AgentsFixture fx;
    const auto root = fx.temp.path();
    write_file(root / "claude" / "settings.json", R"({"model": "opus"})");
    write_file(root / "codex" / "config.toml", "model = \"gpt-6-sol\"\n");
    write_file(
        root / "xdg" / "opencode" / "opencode.jsonc",
        "{\n  // their favourite\n  \"model\": \"openrouter/deepseek/deepseek-v4.1-flash\",\n"
        "  \"provider\": {\"x\": {\"baseURL\": \"https://example.com/v1\"}}, /* trailing */\n}\n");
    (*fx.env)["CLAUDE_CONFIG_DIR"] = fv::path_to_utf8(root / "claude");
    (*fx.env)["CODEX_HOME"] = fv::path_to_utf8(root / "codex");
    (*fx.env)["XDG_CONFIG_HOME"] = fv::path_to_utf8(root / "xdg");
    auto agents = fx.make();
    const auto& config = agents->config();

    CHECK(config.configured_default_model(CLAUDE_CODE) == "opus");
    CHECK(config.configured_default_model(CODEX) == "gpt-6-sol");
    CHECK(config.configured_default_model(OPENCODE) == "openrouter/deepseek/deepseek-v4.1-flash");
    CHECK(config.default_model_label(CODEX) == "Codex default (gpt-6-sol)");
}

// from tests/test_installed_agents.py::test_jsonc_keeps_slashes_inside_strings
TEST_CASE("jsonc keeps slashes inside strings", "[test_installed_agents]") {
    const auto text = R"({"url": "https://a//b", /* c */ "x": [1, 2,],})";
    CHECK(fv::py::loads(fv::agents::strip_jsonc(text)) ==
          fv::Json{{"url", "https://a//b"}, {"x", {1, 2}}});
}

// from tests/test_installed_agents.py::test_codex_models_come_from_its_listed_cache
TEST_CASE("codex models come from its listed cache", "[test_installed_agents]") {
    AgentsFixture fx;
    write_file(
        fx.temp.path() / "codex-home" / "models_cache.json",
        R"({"models": [{"slug": "gpt-6-sol", "display_name": "GPT-6-Sol", "visibility": "list"},
                              {"slug": "internal", "display_name": "Hidden", "visibility": "hide"}]})");
    auto agents = fx.make();
    const auto models = agents->list_models(agent(CODEX, "codex", "0.158.0"));
    CHECK(values(models) == std::vector<std::string>{"", "gpt-6-sol"});
    CHECK(fx.launcher->specs().empty());
}

// from tests/test_installed_agents.py::test_claude_models_are_aliases_after_its_default
TEST_CASE("claude models are aliases after its default", "[test_installed_agents]") {
    AgentsFixture fx;
    auto agents = fx.make();
    const auto models = agents->list_models(agent(CLAUDE_CODE, "claude", "2.1.281"));
    REQUIRE_FALSE(models.empty());
    CHECK(models[0].value.empty());
    CHECK(models[0].label == "Claude Code default");
    const auto listed = values(models);
    CHECK(std::find(listed.begin(), listed.end(), "haiku") != listed.end());
}

// from tests/test_installed_agents.py::test_describe_choice
TEST_CASE("describe choice", "[test_installed_agents]") {
    AgentsFixture fx;
    auto agents = fx.make();
    const auto& config = agents->config();
    CHECK(fv::agents::describe_choice(config, CLAUDE_CODE, "haiku") == "Claude Code · Haiku");
    CHECK(fv::agents::describe_choice(config, OPENCODE, "openrouter/deepseek/v4") ==
          "OpenCode · v4");
    CHECK(fv::agents::describe_choice(config, CLAUDE_CODE, "") == "Claude Code · default model");

    // The free functions read the set instance's configuration.
    fv::agents::set_installed_agents(agents);
    CHECK(fv::agents::describe_choice(CLAUDE_CODE, "") == "Claude Code · default model");
    write_file(fx.temp.path() / "claude-config" / "settings.json", R"({"model": "opus"})");
    CHECK(fv::agents::describe_choice(CLAUDE_CODE, "") == "Claude Code · opus");
    fv::agents::set_installed_agents(nullptr);
}

// from tests/test_installed_agents.py::test_agent_env_drops_keys_from_openwhispers_own_env_file
TEST_CASE("agent env drops keys from openwhispers own env file", "[test_installed_agents]") {
    fv::MapEnv system(fv::ProcessEnv::kWindows,
                      {{"ANTHROPIC_API_KEY", "exported-by-user"}, {"PATH", "C:\\Users\\u\\bin"}});
    fv::ProcessEnv process_env(system);
    // What loading the app's .env does: OPENROUTER_API_KEY is the file's, ANTHROPIC_API_KEY
    // stays the user's.
    const std::vector<std::pair<std::string, std::string>> dotenv{
        {"OPENROUTER_API_KEY", "from-dotenv"}, {"ANTHROPIC_API_KEY", "different"}};
    process_env.load_for_app(dotenv);
    REQUIRE(system.get("OPENROUTER_API_KEY") == "from-dotenv");
    system.set(std::string(fv::identity().env_prefix) + "SIDECAR_TOKEN", "internal");

    AgentsFixture fx;
    fv::agents::InstalledAgents agents(
        fx.launcher, [&] { return fv::to_spec_env(process_env.external_env()); }, fx.home, fx.tmp);
    const auto env = agents.agent_child_env({{"EXTRA", "1"}});
    const fv::EnvMap matched(env.begin(), env.end());
    CHECK_FALSE(matched.contains("OPENROUTER_API_KEY"));
    CHECK(matched.at("ANTHROPIC_API_KEY") == "exported-by-user");
    CHECK(matched.at("EXTRA") == "1");
    CHECK_FALSE(matched.contains(std::string(fv::identity().env_prefix) + "SIDECAR_TOKEN"));
}

// from tests/test_installed_agents.py::test_scan_is_cached_until_refresh
TEST_CASE("scan is cached until refresh", "[test_installed_agents]") {
    AgentsFixture fx;
    auto agents = fx.make<CountingAgents>();
    agents->scan_agents();
    agents->scan_agents();
    auto calls = agents->calls();
    std::sort(calls.begin(), calls.end());
    CHECK(calls == std::vector<std::string>{"claude_code", "codex", "opencode_cli"});
    agents->scan_agents(true);
    CHECK(agents->calls().size() == 6);
    CHECK(agents->cached_scan() == fv::agents::ScanResult{{"claude_code", std::nullopt},
                                                          {"codex", std::nullopt},
                                                          {"opencode_cli", std::nullopt}});
}

// from tests/test_installed_agents.py::test_resolve_keeps_a_usable_lookup_until_the_next_scan
TEST_CASE("resolve keeps a usable lookup until the next scan", "[test_installed_agents]") {
    AgentsFixture fx;
    const auto program = make_program(exe(fx.path_dir, "claude"));
    fx.launcher->reply(program, {"--version"}, {.out = "2.1.0 (Claude Code)\n"});
    fx.launcher->reply(program, {"auth", "status"}, {.out = R"({"loggedIn": false})"});
    auto agents = fx.make<CountingAgents>();

    CHECK_FALSE(agents->resolve_agent(CLAUDE_CODE)->usable());
    CHECK_FALSE(agents->resolve_agent(CLAUDE_CODE)->usable());
    CHECK(agents->calls().size() == 2); // signed out: asked again each time
    fx.launcher->reply(program, {"auth", "status"},
                       {.out = R"({"loggedIn": true, "subscriptionType": "pro"})"});
    CHECK(agents->resolve_agent(CLAUDE_CODE)->usable());
    CHECK(agents->resolve_agent(CLAUDE_CODE)->usable());
    CHECK(agents->calls().size() == 3);
    std::filesystem::remove(program);
    CHECK_FALSE(agents->resolve_agent(CLAUDE_CODE));
    CHECK(agents->calls().size() == 4);

    // A scan forgets the lookups: the scan's answer is what is known afterwards.
    make_program(program);
    agents->scan_agents(true);
    const auto [known, after] = agents->known_agent(CLAUDE_CODE);
    CHECK(known);
    REQUIRE(after);
    CHECK(after->account == "Claude Pro");
    fx.check_launcher_stayed_inside();
}

// from tests/test_installed_agents.py::test_known_agent_never_blocks_and_looks_up_once
TEST_CASE("known agent never blocks and looks up once", "[test_installed_agents]") {
    AgentsFixture fx;
    const auto program = make_program(exe(fx.path_dir, "claude"));
    fx.launcher->reply(program, {"--version"}, {.out = "2.1.0"});
    fx.launcher->reply(program, {"auth", "status"}, {.out = R"({"loggedIn": true})"});
    auto agents = fx.make<CountingAgents>();
    agents->gate = std::make_shared<Gate>();
    fv::ManualExecutor executor;
    std::vector<std::string> heard;
    auto subscription =
        agents->add_lookup_listener(executor, [&](std::string id) { heard.push_back(id); });

    const auto started = std::chrono::steady_clock::now();
    CHECK(agents->known_agent(CLAUDE_CODE) == std::pair{false, std::optional<InstalledAgent>{}});
    CHECK(agents->known_agent(CLAUDE_CODE) == std::pair{false, std::optional<InstalledAgent>{}});
    CHECK(std::chrono::steady_clock::now() - started < std::chrono::seconds(1));
    agents->gate->open();
    agents->wait_for_lookups();
    executor.run_all();

    const auto [known, found] = agents->known_agent(CLAUDE_CODE);
    CHECK(known);
    REQUIRE(found);
    CHECK(found->usable());
    CHECK(agents->calls() == std::vector<std::string>{"claude_code"});
    CHECK(heard == std::vector<std::string>{"claude_code"});
}

// from
// tests/test_installed_agents.py::test_known_agent_rechecks_an_unusable_answer_in_the_background
TEST_CASE("known agent rechecks an unusable answer in the background", "[test_installed_agents]") {
    AgentsFixture fx;
    const auto program = make_program(exe(fx.path_dir, "claude"));
    fx.launcher->reply(program, {"--version"}, {.out = "2.1.0"});
    fx.launcher->reply(program, {"auth", "status"}, {.out = R"({"loggedIn": false})"});
    auto agents = fx.make();
    const auto signed_out = agents->resolve_agent(CLAUDE_CODE);
    REQUIRE(signed_out);
    REQUIRE(signed_out->signed_in == false);

    fv::ManualExecutor executor;
    std::vector<std::string> heard;
    auto subscription =
        agents->add_lookup_listener(executor, [&](std::string id) { heard.push_back(id); });
    fx.launcher->reply(program, {"auth", "status"}, {.out = R"({"loggedIn": true})"});

    CHECK(agents->known_agent(CLAUDE_CODE) == std::pair{true, signed_out});
    agents->wait_for_lookups();
    executor.run_all();
    CHECK(heard == std::vector<std::string>{"claude_code"});
    CHECK(agents->known_agent(CLAUDE_CODE).second->usable());
}

// from tests/test_installed_agents.py::test_candidate_paths_find_a_desktop_bundle
TEST_CASE("candidate paths find a desktop bundle", "[test_installed_agents]") {
#ifndef _WIN32
    SKIP("the versioned bundle folder is the Windows Codex app's layout");
#endif
    AgentsFixture fx;
    const auto bundle =
        make_program(fx.local / "OpenAI" / "Codex" / "bin" / "abc123" / "codex.exe");
    auto agents = fx.make();
    const auto paths = agents->candidate_paths(CODEX);
    CHECK(std::find(paths.begin(), paths.end(), bundle) != paths.end());
}

// ---- beyond the Python tests ----

TEST_CASE("candidates come from PATH first, then the installer folders, each file once",
          "[installed_agents]") {
    AgentsFixture fx;
    make_program(exe(fx.path_dir, "claude"));
    const auto local_bin = make_program(exe(fx.home / ".local" / "bin", "claude"));
    const auto claude_local = make_program(exe(fx.home / ".claude" / "local", "claude"));
    // PATH also names a folder the installer list has: the file comes once.
    (*fx.env)["PATH"] = fv::path_to_utf8(fx.path_dir) + (fv::ProcessEnv::kWindows ? ";" : ":") +
                        fv::path_to_utf8(fx.home / ".local" / "bin");
#ifdef _WIN32
    const auto shim = make_program(fx.appdata / "npm" / "claude.cmd");
    // shutil.which names the file with PATHEXT's spelling of the extension.
    const std::vector<std::filesystem::path> expected{fx.path_dir / "claude.EXE", local_bin,
                                                      claude_local, shim};
#else
    const std::vector<std::filesystem::path> expected{exe(fx.path_dir, "claude"), local_bin,
                                                      claude_local};
#endif
    auto agents = fx.make();
    CHECK(agents->candidate_paths(CLAUDE_CODE) == expected);
}

TEST_CASE("a scan notifies every agent once and logs what it found", "[installed_agents]") {
    AgentsFixture fx;
    const auto program = make_program(exe(fx.path_dir, "codex"));
    fx.launcher->reply(program, {"--version"}, {.out = "codex-cli 0.158.0"});
    fx.launcher->reply(program, {"login", "status"}, {.out = "Logged in using ChatGPT"});
    auto agents = fx.make();
    fv::ManualExecutor executor;
    std::vector<std::string> heard;
    auto subscription =
        agents->add_lookup_listener(executor, [&](std::string id) { heard.push_back(id); });

    const auto result = agents->scan_agents();
    executor.run_all();
    CHECK(heard == std::vector<std::string>{"claude_code", "codex", "opencode_cli"});
    REQUIRE(result.at("codex"));
    CHECK(result.at("codex")->account == "ChatGPT");
    CHECK(result.at("codex")->signed_in == true);
    CHECK_FALSE(result.at("claude_code"));

    // The cached scan answers without notifying again.
    agents->scan_agents();
    executor.run_all();
    CHECK(heard.size() == 3);
    CHECK(agents->known_agent(CODEX) == std::pair{true, result.at("codex")});
}

TEST_CASE("probes run in the agent workspace with the agent environment and no stdin",
          "[installed_agents]") {
    AgentsFixture fx;
    (*fx.env)[std::string(fv::identity().env_prefix) + "LLM_API_KEY"] = "internal";
    const auto program = make_program(exe(fx.path_dir, "claude"));
    fx.launcher->reply(program, {"--version"}, {.out = "2.1.0"});
    auto agents = fx.make();
    REQUIRE(agents->find_agent(CLAUDE_CODE));

    const auto specs = fx.launcher->specs();
    REQUIRE(specs.size() == 1);
    const auto workspace =
        fx.tmp / fv::path_from_utf8(fv::py::lower(fv::identity().product) + "-agent-workspace");
    CHECK(specs[0].cwd == workspace);
    CHECK(std::filesystem::is_directory(workspace));
    CHECK(specs[0].stdin_null);
    CHECK(specs[0].pipes);
    CHECK(specs[0].own_tree);
    CHECK_FALSE(specs[0].new_console);
    CHECK(specs[0].env.at("PATH") == fv::path_to_utf8(fx.path_dir));
    CHECK_FALSE(specs[0].env.contains(std::string(fv::identity().env_prefix) + "LLM_API_KEY"));
    fx.check_launcher_stayed_inside();
}

TEST_CASE("a probe that doesn't answer in time counts as no answer", "[installed_agents]") {
    AgentsFixture fx;
    const auto program = make_program(exe(fx.path_dir, "claude"));
    // Never exits: the 6 s version probe gives up and the tree is killed.
    fx.launcher->reply(program, {"--version"}, {.code = std::nullopt, .out = "2.1.0"});
    auto agents = fx.make();
    const auto started = std::chrono::steady_clock::now();
    CHECK_FALSE(agents->find_agent(CLAUDE_CODE));
    const auto elapsed = std::chrono::steady_clock::now() - started;
    CHECK(elapsed >= std::chrono::milliseconds(5900));
    CHECK(elapsed < std::chrono::seconds(10));
}

namespace {

// Runs each task at once, on the posting thread.
class InlineExecutor final : public fv::Executor {
public:
    void post(fv::Task task) override { task(); }
    fv::TimerHandle post_after(std::chrono::milliseconds, fv::Task) override { return {}; }
    bool is_current() const override { return true; }
};

// Codex's lookup takes a while, so it is still running when Claude Code's has ended.
class SlowCodexAgents final : public fv::agents::InstalledAgents {
public:
    using fv::agents::InstalledAgents::InstalledAgents;
    ~SlowCodexAgents() override { wait_for_lookups(); }
    std::optional<InstalledAgent> find_agent(std::string_view id) override {
        if (id == CODEX) {
            std::this_thread::sleep_for(std::chrono::milliseconds(300));
        }
        return fv::agents::InstalledAgents::find_agent(id);
    }
};

} // namespace

TEST_CASE("waiting for lookups also waits for one a listener started meanwhile",
          "[installed_agents]") {
    AgentsFixture fx;
    auto agents = fx.make<SlowCodexAgents>();
    InlineExecutor inline_executor;
    std::atomic<bool> chained{false};
    // On Claude Code's lookup thread, the listener asks about Codex, which starts its lookup.
    auto subscription = agents->add_lookup_listener(inline_executor, [&](std::string id) {
        if (id == CLAUDE_CODE && !chained.exchange(true)) {
            agents->known_agent(CODEX);
        }
    });
    CHECK_FALSE(agents->known_agent(CLAUDE_CODE).first);
    agents->wait_for_lookups();
    CHECK(chained.load());
    // Codex's lookup has ended too: its answer (none found) is known.
    CHECK(agents->known_agent(CODEX).first);
    subscription.disconnect();
    agents->wait_for_lookups();
}

TEST_CASE("installed_agents() needs an instance", "[installed_agents]") {
    fv::agents::set_installed_agents(nullptr);
    CHECK_THROWS_AS(fv::agents::installed_agents(), std::logic_error);
    auto fake = std::make_shared<fv::agents::FakeInstalledAgents>(
        fv::agents::ScanResult{{"codex", agent(CODEX, "codex", "0.158.0", true)}});
    fv::agents::set_installed_agents(fake);
    CHECK(&fv::agents::installed_agents() == fake.get());
    const auto scan = fv::agents::installed_agents().scan_agents();
    CHECK(scan.at("codex")->version == "0.158.0");
    CHECK_FALSE(scan.at("claude_code"));
    CHECK(fake->lookups() == 3);
    fv::agents::set_installed_agents(nullptr);

    // A fake's workspace can live in the test's own folder.
    AgentsFixture fx;
    const fv::agents::FakeInstalledAgents in_temp({}, fx.home, fx.tmp);
    CHECK(in_temp.agent_workspace_dir().parent_path() == fx.tmp);
}
