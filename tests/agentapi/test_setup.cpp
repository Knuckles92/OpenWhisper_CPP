// Golden replay of services/agent_mcp/setup.py (tools/parity/cases/services.agent_mcp.setup.py).
#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <string>
#include <string_view>

#include "agentapi/setup.h"
#include "base/identity.h"
#include "base/utf8.h"
#include "golden/golden.h"
#include "pyport/json.h"

namespace {

using fv::Json;
namespace golden = fv::golden;
namespace agentapi = fv::agentapi;

constexpr std::string_view kModule = "services.agent_mcp.setup";

// OpenWhisper's names as OpenWhisper_CPP's: the product, its variables and files, and the MCP
// server name, which setup.py spells in lower case.
std::string rebranded(std::string_view text) {
    std::string out = golden::rebrand(text);
    const std::string legacy = "open" + std::string("whisper");
    const std::string name = agentapi::server_name();
    for (std::size_t at = out.find(legacy); at != std::string::npos;
         at = out.find(legacy, at + name.size())) {
        out.replace(at, legacy.size(), name);
    }
    return out;
}

Json rebranded_json(const Json& value) {
    return fv::py::loads(rebranded(fv::py::dumps(value)));
}

std::string arg(const golden::Case& c, std::size_t index) {
    return rebranded(c.arg<std::string>(index));
}

bool recorded_on_windows() {
    return golden::load(kModule).at("pins").value("platform", "") == "win32";
}

} // namespace

TEST_CASE("the MCP server is named after the product, in lower case", "[golden]") {
    CHECK(agentapi::server_name() == "openwhisper-cpp");
    golden::for_each(kModule, "CHANNEL_SERVER_NAME", [](const golden::Case& c) {
        CHECK(agentapi::server_name() == rebranded(c.result<std::string>()));
    });
    golden::for_each(kModule, "TOKEN_PLACEHOLDER", [](const golden::Case& c) {
        CHECK(agentapi::TOKEN_PLACEHOLDER == c.result<std::string>());
    });
}

TEST_CASE("connection snippets match OpenWhisper's", "[golden]") {
    golden::for_each(kModule, "client_config", [](const golden::Case& c) {
        const std::string url = arg(c, 0);
        const std::string result = c.arg_count() > 1 ? agentapi::client_config(url, arg(c, 1))
                                                     : agentapi::client_config(url);
        CHECK(result == rebranded(c.result<std::string>()));
    });
    golden::for_each(kModule, "claude_command", [](const golden::Case& c) {
        const std::string url = arg(c, 0);
        const std::string result = c.arg_count() > 1 ? agentapi::claude_command(url, arg(c, 1))
                                                     : agentapi::claude_command(url);
        CHECK(result == rebranded(c.result<std::string>()));
    });
    golden::for_each(kModule, "chatgpt_config", [](const golden::Case& c) {
        const std::string url = arg(c, 0);
        const std::string result = c.arg_count() > 1 ? agentapi::chatgpt_config(url, arg(c, 1))
                                                     : agentapi::chatgpt_config(url);
        CHECK(result == rebranded(c.result<std::string>()));
    });
}

TEST_CASE("the agent prompt matches OpenWhisper's", "[golden]") {
    golden::for_each(kModule, "agent_prompt", [](const golden::Case& c) {
        const auto host = c.arg<std::optional<std::string>>(1);
        const std::string token = arg(c, 2);
        std::optional<std::string_view> host_view;
        if (host) {
            host_view = *host;
        }
        CHECK(agentapi::agent_prompt(arg(c, 0), host_view, token) ==
              rebranded(c.result<std::string>()));
    });
}

TEST_CASE("the Claude Code channel entry matches OpenWhisper's", "[golden]") {
    // The paths are Windows paths; os.path's rules on another platform differ.
    if (!recorded_on_windows()) {
        SKIP("the vectors were recorded on another platform");
    }
#ifndef _WIN32
    SKIP("the vectors hold Windows paths");
#endif
    golden::for_each(kModule, "channel_command_line", [](const golden::Case& c) {
        const auto command = agentapi::channel_command_line(fv::path_from_utf8(arg(c, 0)));
        Json expected = rebranded_json(c.result<Json>());
        Json actual = Json::array();
        for (const auto& part : command) {
            actual.push_back(part);
        }
        CHECK(actual == expected);
    });
    golden::for_each(kModule, "_channel_env", [](const golden::Case& c) {
        const Json env =
            agentapi::channel_env(fv::path_from_utf8(arg(c, 0)), fv::path_from_utf8(arg(c, 1)));
        CHECK(env == rebranded_json(c.result<Json>()));
    });
    golden::for_each(kModule, "claude_channel_config", [](const golden::Case& c) {
        CHECK(agentapi::claude_channel_config(
                  fv::path_from_utf8(arg(c, 0)), fv::path_from_utf8(arg(c, 1)),
                  fv::path_from_utf8(arg(c, 2))) == rebranded(c.result<std::string>()));
    });
    golden::for_each(kModule, "claude_channel_command", [](const golden::Case& c) {
        CHECK(agentapi::claude_channel_command() == rebranded(c.result<std::string>()));
    });
}

TEST_CASE("a localhost prompt without a host name names this computer") {
    const std::string prompt = agentapi::agent_prompt("http://127.0.0.1:8767/mcp");
    CHECK(prompt.starts_with("Connect my agent to " + std::string(fv::identity().product) +
                             " running on computer "));
    CHECK(prompt.find("Ask me for the token") != std::string::npos);
    CHECK(prompt == agentapi::agent_prompt("http://127.0.0.1:8767/mcp", std::string_view()));
}
