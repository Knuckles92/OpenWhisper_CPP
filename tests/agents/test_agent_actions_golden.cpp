// Replays tests/golden/services.actions.agents.json (tools/parity/cases/
// services.actions.agents.py): app links, terminal command lines, npm shims, and how the agents'
// output and errors read, as OpenWhisper computes them.

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include "agents/acp.h"
#include "agents/agent_actions.h"
#include "agents/detail/headless.h"
#include "agents/detail/shim.h"
#include "agents_test_support.h"
#include "base/utf8.h"
#include "golden/golden.h"
#include "pyport/errors.h"

namespace golden = fv::golden;
namespace detail = fv::agents::detail;
using fv::Json;
using fv::agents_test::TempDir;
using fv::agents_test::write_file;

namespace {

constexpr std::string_view kModule = "services.actions.agents";

Json result_json(const fv::agents::AskResult& result) {
    return Json::array({result.ok, result.message, result.ok ? Json(result.answer) : Json(nullptr),
                        result.stopped});
}

detail::HeadlessRun run_of(const std::string& stdout_text, const std::string& stderr_text) {
    return detail::HeadlessRun{
        .status = "done", .returncode = 0, .stdout_text = stdout_text, .stderr_text = stderr_text};
}

// What `body` gives for the case, or the Python exception it raises.
template <class Body> void check(const golden::Case& c, Body body) {
    if (c.raises()) {
        try {
            body();
            FAIL_CHECK(c.label() << ": expected " << c.raises_type());
        } catch (const fv::py::PyError& error) {
            CHECK(error.type_name() == c.raises_type());
        } catch (const fv::agents::AcpError&) {
            CHECK(c.raises_type() == "AcpError");
        }
        return;
    }
    const Json actual = body();
    INFO(c.label());
    CHECK(golden::equal(c.raw().at("result"), actual));
}

std::string slashes(std::string text) {
    std::replace(text.begin(), text.end(), '\\', '/');
    return text;
}

} // namespace

TEST_CASE("app_links replays OpenWhisper's vectors", "[agent_actions][golden]") {
    golden::for_each(kModule, "app_links", [](const golden::Case& c) {
        check(c, [&] {
            Json out = Json::array();
            for (const auto& uri :
                 fv::agents::app_links(c.arg<std::string>(0), c.arg<std::string>(1),
                                       fv::path_from_utf8(c.arg<std::string>(2)),
                                       fv::path_from_utf8(c.arg<std::string>(3)))) {
                out.push_back(Json::array({uri.substr(0, uri.find(':')), uri}));
            }
            return out;
        });
    });
}

TEST_CASE("terminal_argv replays OpenWhisper's vectors", "[agent_actions][golden]") {
    golden::for_each(kModule, "terminal_argv", [](const golden::Case& c) {
        check(c, [&] {
            const auto agent_id = c.arg<std::string>(0);
            const auto text = c.arg<std::string>(2);
            if (c.args().at(1).is_array()) {
                return Json(
                    fv::agents::terminal_argv(agent_id, c.arg<std::vector<std::string>>(1), text));
            }
            return Json(fv::agents::terminal_argv(agent_id,
                                                  fv::path_from_utf8(c.arg<std::string>(1)), text));
        });
    });
}

TEST_CASE("npm_shim_command replays OpenWhisper's vectors", "[agent_actions][golden]") {
    golden::for_each(kModule, "npm_shim_command", [](const golden::Case& c) {
        check(c, [&] {
            // The case's layout under a fresh folder, as the case file's _npm_shim_command.
            TempDir temp;
            const auto root = temp.path();
            const auto shim = root / "npm" / fv::path_from_utf8(c.arg<std::string>(3));
            write_file(shim, c.arg<std::string>(0));
            for (const auto& relative : c.arg<std::vector<std::string>>(1)) {
                write_file(root / fv::path_from_utf8(relative), "");
            }
            std::optional<std::string> path_env;
            if (c.arg<bool>(2)) {
                write_file(root / "nodejs" / "node.exe", "");
                path_env = fv::path_to_utf8(root / "nodejs");
            }
            const auto command = detail::npm_shim_command(shim, path_env);
            if (!command) {
                return Json(nullptr);
            }
            Json out = Json::array();
            for (const auto& part : *command) {
                out.push_back(
                    slashes(fv::path_to_utf8(fv::path_from_utf8(part).lexically_relative(root))));
            }
            return out;
        });
    });
}

TEST_CASE("_claude_result replays OpenWhisper's vectors", "[agent_actions][golden]") {
    golden::for_each(kModule, "_claude_result", [](const golden::Case& c) {
        check(c, [&] {
            const auto result = detail::claude_result(c.arg<std::string>(0));
            return result ? *result : Json(nullptr);
        });
    });
}

TEST_CASE("_claude_answer replays OpenWhisper's vectors", "[agent_actions][golden]") {
    golden::for_each(kModule, "_claude_answer", [](const golden::Case& c) {
        check(c, [&] {
            return result_json(
                detail::claude_answer(run_of(c.arg<std::string>(0), c.arg<std::string>(1))));
        });
    });
}

TEST_CASE("_codex_events replays OpenWhisper's vectors", "[agent_actions][golden]") {
    golden::for_each(kModule, "_codex_events", [](const golden::Case& c) {
        check(c, [&] { return Json(detail::codex_events(c.arg<std::string>(0))); });
    });
}

TEST_CASE("_codex_answer replays OpenWhisper's vectors", "[agent_actions][golden]") {
    golden::for_each(kModule, "_codex_answer", [](const golden::Case& c) {
        check(c, [&] {
            return result_json(
                detail::codex_answer(run_of(c.arg<std::string>(0), c.arg<std::string>(1))));
        });
    });
}

TEST_CASE("_codex_message replays OpenWhisper's vectors", "[agent_actions][golden]") {
    golden::for_each(kModule, "_codex_message", [](const golden::Case& c) {
        check(c, [&] { return Json(detail::codex_message(c.arg<std::string>(0))); });
    });
}

TEST_CASE("_failure, _signed_out and _stopped replay OpenWhisper's vectors",
          "[agent_actions][golden]") {
    golden::for_each(kModule, "_failure", [](const golden::Case& c) {
        check(c,
              [&] { return Json(detail::failure(c.arg<std::string>(0), c.arg<std::string>(1))); });
    });
    golden::for_each(kModule, "_signed_out", [](const golden::Case& c) {
        check(c, [&] { return Json(detail::signed_out(c.arg<std::string>(0))); });
    });
    golden::for_each(kModule, "_stopped", [](const golden::Case& c) {
        check(c, [&] {
            return Json(detail::stopped(c.arg<std::string>(0), run_of("", c.arg<std::string>(1))));
        });
    });
}

TEST_CASE("_opencode_result replays OpenWhisper's vectors", "[agent_actions][golden]") {
    golden::for_each(kModule, "_opencode_result", [](const golden::Case& c) {
        check(c, [&] {
            detail::AcpAnswer answer;
            for (const auto& notification : c.args().at(1)) {
                answer.on_notification(notification.at(0).get<std::string>(), notification.at(1));
            }
            return result_json(detail::opencode_result(c.args().at(0), answer.text()));
        });
    });
}

TEST_CASE("_refuse_acp_request replays OpenWhisper's vectors", "[agent_actions][golden]") {
    golden::for_each(kModule, "_refuse_acp_request", [](const golden::Case& c) {
        check(c, [&] { return detail::refuse_acp_request(c.arg<std::string>(0), c.args().at(1)); });
    });
}

TEST_CASE("_opencode_model replays OpenWhisper's vectors", "[agent_actions][golden]") {
    golden::for_each(kModule, "_opencode_model", [](const golden::Case& c) {
        check(c, [&] {
            const auto model = detail::opencode_model(c.args().at(0), c.arg<std::string>(1),
                                                      c.arg<std::string>(2));
            return model ? Json(*model) : Json(nullptr);
        });
    });
}

TEST_CASE("_codex_tool_features replays OpenWhisper's vectors", "[agent_actions][golden]") {
    golden::for_each(kModule, "_codex_tool_features", [](const golden::Case& c) {
        check(c, [&] {
            // The listing is what `codex features list` prints to a scripted launcher.
            fv::agents_test::AgentsFixture fixture;
            const auto agents = fixture.make();
            const fv::agents::InstalledAgent codex{"codex", fixture.temp.path() / "codex.exe",
                                                   "0.162.1"};
            detail::forget_help();
            fixture.launcher->reply(codex.path, {"features", "list"},
                                    fv::agents_test::Reply{0, c.arg<std::string>(0)});
            return Json(detail::codex_tool_features(*agents, codex));
        });
    });
}
