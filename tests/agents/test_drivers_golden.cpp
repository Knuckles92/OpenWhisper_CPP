// Replays tests/golden/meeting.agent.installed.drivers.json and .core.json
// (tools/parity/cases/meeting.agent.installed.drivers.py and .core.py): the tool contract, the
// Claude Code and Codex command lines and event folding, the error texts, OpenCode's overlay,
// permission answers and outcomes, and what installed_agent says, as OpenWhisper computes them.
// OpenWhisper's names in the vectors are read as OpenWhisper_CPP's (rebranded()), inputs and
// results alike.

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include "agents/agent_tasks.h"
#include "agents/detail/driver_impl.h"
#include "agents/drivers.h"
#include "golden/golden.h"
#include "pass_support.h"
#include "pyport/errors.h"

namespace golden = fv::golden;
namespace detail = fv::agents::detail;
using fv::Json;
using fv::agents::AgentUnavailable;
using fv::agents::InstalledAgent;
using fv::agents::PassOutcome;
using fv::agents::PassRequest;
using namespace fv::agents_test;

namespace {

constexpr std::string_view kDrivers = "meeting.agent.installed.drivers";
constexpr std::string_view kCore = "meeting.agent.installed.core";

Json outcome_json(const PassOutcome& outcome) {
    return Json::array({outcome.ok, outcome.text, outcome.error, outcome.usage, outcome.canceled});
}

// What `body` gives for the case (compared with the rebranded result), or the Python exception
// it raises.
template <class Body> void check(const golden::Case& c, Body body) {
    INFO(c.label());
    if (c.raises()) {
        try {
            body();
            FAIL_CHECK("expected " << c.raises_type());
        } catch (const fv::py::PyError& error) {
            CHECK(error.type_name() == c.raises_type());
        }
        return;
    }
    const Json actual = body();
    const Json expected = rebranded(c.raw().at("result"));
    CHECK(golden::equal(expected, actual));
    if (!golden::equal(expected, actual)) {
        UNSCOPED_INFO("expected " << expected.dump());
        UNSCOPED_INFO("actual   " << actual.dump());
    }
}

Json arg(const golden::Case& c, std::size_t index) {
    return rebranded(c.args().at(index));
}

PassRequest request_from(const Json& values) {
    PassRequest request;
    request.system_prompt = values.value("system_prompt", std::string());
    request.user_prompt = values.value("user_prompt", std::string());
    request.tools = values.value("tools", Json::array());
    request.handler = [](std::string_view, const Json&, const Json&) {
        return std::pair<std::string, bool>("", false);
    };
    request.model = values.value("model", std::string());
    request.effort = values.value("effort", std::string());
    if (values.contains("read_folder")) {
        request.read_folder = fv::path_from_utf8(values.at("read_folder").get<std::string>());
    }
    for (const auto& image : values.value("images", Json::array())) {
        request.images.push_back(fv::path_from_utf8(image.get<std::string>()));
    }
    return request;
}

std::string replace_all(std::string text, const std::string& from, const std::string& to) {
    if (from.empty()) {
        return text;
    }
    for (auto at = text.find(from); at != std::string::npos; at = text.find(from, at + to.size())) {
        text.replace(at, from.size(), to);
    }
    return text;
}

// The working folder and the agent workspace as the vectors name them.
Json normalized(const Json& value, const std::filesystem::path& workdir,
                const std::filesystem::path& workspace) {
    if (value.is_string()) {
        auto text = value.get<std::string>();
        auto forward = fv::path_to_utf8(workdir);
        std::replace(forward.begin(), forward.end(), '\\', '/');
        text = replace_all(text, fv::path_to_utf8(workdir), "<workdir>");
        text = replace_all(text, forward, "<workdir>");
        return replace_all(text, fv::path_to_utf8(workspace), "<workspace>");
    }
    if (value.is_array()) {
        Json out = Json::array();
        for (const auto& item : value) {
            out.push_back(normalized(item, workdir, workspace));
        }
        return out;
    }
    if (value.is_object()) {
        Json out = Json::object();
        for (const auto& [key, item] : value.items()) {
            out[key] = normalized(item, workdir, workspace);
        }
        return out;
    }
    return value;
}

struct Offline : AgentsFixture {
    std::shared_ptr<fv::agents::InstalledAgents> agents = make();
    ScopedAgents scoped{agents};
    TempDir workdir;
};

Json built(detail::HeadlessDriver& driver, const Json& values, const Offline& offline,
           std::initializer_list<const char*> names) {
    try {
        driver.check_ready();
        const auto [argv, env] = driver.build(request_from(values), "http://127.0.0.1:1/mcp/t", "t",
                                              offline.workdir.path());
        Json files = Json::object();
        for (const auto* name : names) {
            const auto file = offline.workdir.path() / name;
            if (std::filesystem::exists(file)) {
                files[name] = read_all(file);
            }
        }
        return normalized(Json{{"argv", argv}, {"env", env}, {"files", files}},
                          offline.workdir.path(), offline.agents->agent_workspace_dir());
    } catch (const AgentUnavailable& error) {
        return Json{{"unavailable", error.what()}};
    }
}

#ifdef _WIN32
#define FV_WINDOWS_PATHS()
#else
#define FV_WINDOWS_PATHS() SKIP("the vectors were recorded with Windows paths")
#endif

Json events_json(detail::HeadlessDriver& driver, const Json& events, const Json& tools) {
    Json emitted = Json::array();
    auto request =
        request_from(Json{{"system_prompt", "s"}, {"user_prompt", "u"}, {"tools", tools}});
    request.on_event = [&](std::string_view kind, std::string_view tool) {
        emitted.push_back(Json::array({kind, tool}));
    };
    detail::RunState state;
    for (const auto& event : events) {
        try {
            driver.handle_event(event, state, request);
        } catch (const fv::py::PyError& error) {
            emitted.push_back(Json::array({"raised", error.type_name()}));
        }
    }
    return Json{{"state",
                 {{"text", state.text},
                  {"error", state.error},
                  {"fatal", state.fatal},
                  {"done", state.done},
                  {"usage", state.usage}}},
                {"emitted", emitted}};
}

detail::RunState state_from(const Json& values) {
    detail::RunState state;
    state.text = values.value("text", std::string());
    state.error = values.value("error", std::string());
    state.fatal = values.value("fatal", std::string());
    state.done = values.value("done", false);
    state.usage = values.value("usage", Json::object());
    return state;
}

std::optional<int> returncode(const Json& value) {
    return value.is_null() ? std::nullopt : std::optional<int>(value.get<int>());
}

} // namespace

TEST_CASE("tool_contract replays OpenWhisper's vectors", "[drivers][golden]") {
    golden::for_each(kDrivers, "tool_contract", [](const golden::Case& c) {
        check(c, [&] {
            return Json(fv::agents::tool_contract(arg(c, 0), arg(c, 1).get<std::string>(),
                                                  arg(c, 2).get<std::string>(),
                                                  arg(c, 3).get<std::string>()));
        });
    });
}

TEST_CASE("the error texts replay OpenWhisper's vectors", "[drivers][golden]") {
    golden::for_each(kDrivers, "_claude_error", [](const golden::Case& c) {
        check(c, [&] { return Json(detail::claude_error(arg(c, 0).get<std::string>())); });
    });
    golden::for_each(kDrivers, "_codex_error", [](const golden::Case& c) {
        check(c, [&] { return Json(detail::codex_error(arg(c, 0).get<std::string>())); });
    });
    golden::for_each(kDrivers, "tail_detail", [](const golden::Case& c) {
        check(c, [&] { return Json(detail::tail_detail(arg(c, 0).get<std::string>())); });
    });
}

TEST_CASE("the Claude Code command line replays OpenWhisper's vectors", "[drivers][golden]") {
    FV_WINDOWS_PATHS();
    golden::for_each(kDrivers, "claude_command", [](const golden::Case& c) {
        check(c, [&] {
            Offline offline;
            detail::ClaudeCodeDriver driver({"claude_code", "claude.exe", "2.1.306"},
                                            offline.launcher,
                                            canned_help(arg(c, 0).get<std::string>()));
            return built(driver, arg(c, 1), offline, {"system.md", "mcp.json", "settings.json"});
        });
    });
}

TEST_CASE("the Codex command line replays OpenWhisper's vectors", "[drivers][golden]") {
    FV_WINDOWS_PATHS();
    golden::for_each(kDrivers, "codex_command", [](const golden::Case& c) {
        check(c, [&] {
            Offline offline;
            const auto exec_help = arg(c, 0).get<std::string>();
            const auto features = arg(c, 1).get<std::string>();
            const auto listing = arg(c, 2).get<std::string>();
            detail::CodexDriver driver(
                {"codex", "codex.exe", "0.163.0"}, offline.launcher,
                [=](const InstalledAgent&, const std::vector<std::string>& args) {
                    if (ends_mcp_list(args)) {
                        return listing;
                    }
                    return !args.empty() && args[0] == "features" ? features : exec_help;
                });
            return built(driver, arg(c, 3), offline, {"instructions.md"});
        });
    });
}

TEST_CASE("Claude Code's events fold as OpenWhisper's did", "[drivers][golden]") {
    golden::for_each(kDrivers, "claude_events", [](const golden::Case& c) {
        check(c, [&] {
            Offline offline;
            detail::ClaudeCodeDriver driver({"claude_code", "claude", "2.1.306"}, offline.launcher,
                                            canned_help(""));
            return events_json(driver, arg(c, 0), arg(c, 1));
        });
    });
    golden::for_each(kDrivers, "claude_finish", [](const golden::Case& c) {
        check(c, [&] {
            Offline offline;
            detail::ClaudeCodeDriver driver({"claude_code", "claude", "2.1.306"}, offline.launcher,
                                            canned_help(""));
            return outcome_json(driver.finish(state_from(arg(c, 0)), returncode(c.args().at(1)),
                                              arg(c, 2).get<std::string>()));
        });
    });
}

TEST_CASE("Codex's events fold as OpenWhisper's did", "[drivers][golden]") {
    golden::for_each(kDrivers, "codex_events", [](const golden::Case& c) {
        check(c, [&] {
            Offline offline;
            detail::CodexDriver driver({"codex", "codex", "0.163.0"}, offline.launcher,
                                       canned_help(""));
            return events_json(driver, arg(c, 0), Json::array());
        });
    });
    golden::for_each(kDrivers, "codex_finish", [](const golden::Case& c) {
        check(c, [&] {
            Offline offline;
            detail::CodexDriver driver({"codex", "codex", "0.163.0"}, offline.launcher,
                                       canned_help(""));
            return outcome_json(driver.finish(state_from(arg(c, 0)), returncode(c.args().at(1)),
                                              arg(c, 2).get<std::string>()));
        });
    });
}

TEST_CASE("Codex's lockdown lists replay OpenWhisper's vectors", "[drivers][golden]") {
    golden::for_each(kDrivers, "codex_features", [](const golden::Case& c) {
        check(c, [&] {
            return Json(detail::codex_features_to_disable(arg(c, 0).get<std::string>()));
        });
    });
    golden::for_each(kDrivers, "codex_servers", [](const golden::Case& c) {
        check(c, [&] {
            try {
                return Json(detail::codex_servers_from(arg(c, 0).get<std::string>()));
            } catch (const AgentUnavailable& error) {
                return Json{{"unavailable", error.what()}};
            }
        });
    });
}

TEST_CASE("OpenCode's overlay and answers replay OpenWhisper's vectors", "[drivers][golden]") {
    golden::for_each(kDrivers, "opencode_overlay", [](const golden::Case& c) {
        check(c, [&] {
            std::map<std::string, std::string> personas;
            const auto files = arg(c, 1);
            for (const auto& [name, text] : files.items()) {
                personas.emplace(name, text.get<std::string>());
            }
            return detail::opencode_overlay(arg(c, 0).get<bool>(), personas,
                                            arg(c, 2).get<std::string>(),
                                            arg(c, 3).get<std::string>());
        });
    });
    golden::for_each(kDrivers, "opencode_request", [](const golden::Case& c) {
        check(c, [&] {
            try {
                return detail::opencode_request(arg(c, 0).get<std::string>(), arg(c, 1));
            } catch (const fv::agents::AcpError& error) {
                return Json{{"acp_error", error.what()}, {"code", *error.code()}};
            }
        });
    });
    golden::for_each(kDrivers, "opencode_pass_model", [](const golden::Case& c) {
        check(c, [&] {
            const auto [model, error] = detail::opencode_pass_model(
                arg(c, 0), arg(c, 1).get<std::string>(), arg(c, 2).get<std::string>());
            return Json::array({model, error});
        });
    });
    golden::for_each(kDrivers, "opencode_failed", [](const golden::Case& c) {
        check(c, [&] {
            return outcome_json(detail::opencode_failed(
                fv::agents::AcpError(arg(c, 0).get<std::string>()), arg(c, 1).get<bool>()));
        });
    });
    golden::for_each(kDrivers, "opencode_outcome", [](const golden::Case& c) {
        check(c, [&] { return outcome_json(detail::opencode_outcome(arg(c, 0))); });
    });
}

TEST_CASE("installed_agent replays OpenWhisper's vectors", "[agent_tasks][golden]") {
    golden::for_each(kCore, "installed_agent", [](const golden::Case& c) {
        check(c, [&] {
            AgentsFixture fixture;
            const auto table = fixture.make<TableAgents>();
            ScopedAgents scoped(table);
            const auto found = arg(c, 1);
            if (found.is_object()) {
                InstalledAgent agent{found.at("id").get<std::string>(),
                                     fv::path_from_utf8(found.at("path").get<std::string>()),
                                     found.at("version").get<std::string>()};
                agent.account = found.value("account", std::string());
                if (found.contains("signed_in") && !found.at("signed_in").is_null()) {
                    agent.signed_in = found.at("signed_in").get<bool>();
                }
                agent.problem = found.value("problem", std::string());
                table->set(agent);
            }
            try {
                const auto agent = fv::agents::installed_agent(arg(c, 0).get<std::string>());
                return Json{{"agent",
                             Json::array({agent.id, fv::path_to_utf8(agent.path), agent.version})}};
            } catch (const AgentUnavailable& error) {
                return Json{{"unavailable", error.what()}};
            }
        });
    });
    golden::for_each(kCore, "constants", [](const golden::Case& c) {
        check(c, [&] {
            return Json{{"MEETING_AGENT_SETTING", fv::agents::meeting_agent_setting()},
                        {"FINAL_EFFORT", fv::agents::FINAL_EFFORT},
                        {"CONSOLIDATION_STALL_S", fv::agents::CONSOLIDATION_STALL_S}};
        });
    });
}
