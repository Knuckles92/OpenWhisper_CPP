// openwhisper-cpp-cli --api, the headless History API, run as a child.
// from tests/test_agent_api.py (the two CLI tests; the API itself is tests/agentapi)
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <chrono>
#include <filesystem>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "base/data_lease.h"
#include "base/identity.h"
#include "base/json.h"
#include "base/utf8.h"
#include "channel_support.h"
#include "cli_process.h"
#include "engine/runtime.h"
#include "http/client.h"
#include "pyport/json.h"
#include "pyport/text.h"
#include "testkit/testkit.h"

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <psapi.h>
#endif

using Catch::Matchers::ContainsSubstring;
using Catch::Matchers::StartsWith;
using fv::Json;
using namespace fv::clitest;
namespace fs = std::filesystem;
using namespace std::chrono_literals;

namespace {

const std::string kApiToken = "test-history-token-" + std::string(32, 'a');

std::string prefix() {
    return std::string(fv::identity().env_prefix);
}

std::string legacy_prefix() {
    return std::string(fv::legacy_identity().env_prefix);
}

// Neither token variable reaches the child unless a test sets one.
std::vector<EnvOverride> api_env(const fs::path& data, std::vector<EnvOverride> extra = {}) {
    std::vector<EnvOverride> env{{prefix() + "DATA_DIR", fv::path_to_utf8(data)},
                                 {prefix() + "API_TOKEN", std::nullopt},
                                 {legacy_prefix() + "API_TOKEN", std::nullopt}};
    env.insert(env.end(), extra.begin(), extra.end());
    return env;
}

std::optional<fv::http::Response> get(std::uint16_t port, const std::string& path) {
    fv::http::Request request;
    request.url = "http://127.0.0.1:" + std::to_string(port) + path;
    request.bypass_proxy = true;
    request.connect_timeout = 2s;
    request.idle_timeout = 10s;
    request.log_request = false;
    request.headers.emplace_back("Authorization", "Bearer " + kApiToken);
    auto answer = fv::http::make_curl_client()->send(request, {});
    if (!answer) {
        return std::nullopt;
    }
    return *answer;
}

// The file names of the modules loaded in process `pid`, lower case; empty off Windows.
std::vector<std::string> loaded_modules(std::uint32_t pid) {
    std::vector<std::string> names;
#if defined(_WIN32)
    HANDLE process = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE, pid);
    REQUIRE(process != nullptr);
    std::vector<HMODULE> modules(1024);
    DWORD needed = 0;
    if (EnumProcessModulesEx(process, modules.data(),
                             static_cast<DWORD>(modules.size() * sizeof(HMODULE)), &needed,
                             LIST_MODULES_ALL)) {
        modules.resize(std::min<std::size_t>(modules.size(), needed / sizeof(HMODULE)));
        for (HMODULE module : modules) {
            wchar_t name[MAX_PATH];
            if (GetModuleBaseNameW(process, module, name, MAX_PATH) > 0) {
                names.push_back(fv::py::lower(fv::path_to_utf8(fs::path(name))));
            }
        }
    }
    CloseHandle(process);
#elif defined(__linux__)
    names = mapped_modules(pid);
#else
    (void)pid;
#endif
    return names;
}

} // namespace

TEST_CASE("test_cli_is_headless_and_requires_opt_in", "[platform][test_agent_api]") {
    fv::testkit::TempDataRoot root;
    const fs::path data = root.path() / "data";

    const CliResult help = run_cli({"--api", "--help"}, api_env(data));
    CHECK(help.exit_code == 0u);
    CHECK_THAT(help.out, StartsWith("usage: openwhisper-cpp-cli --api [-h] [--database DATABASE] "
                                    "[--port PORT]"));
    CHECK_THAT(help.out,
               ContainsSubstring("Serve the read-only " + std::string(fv::identity().product) +
                                 " History API on 127.0.0.1."));

    const CliResult refused = run_cli({"--api"}, api_env(data));
    CHECK(refused.exit_code == 2u);
    CHECK_THAT(refused.err, ContainsSubstring(prefix() + "API_TOKEN"));
    CHECK_THAT(refused.err,
               ContainsSubstring("to a randomly generated token of at least 32 characters."));
    CHECK_FALSE(fs::exists(data / fv::path_from_utf8(fv::identity().database_file)));

    // The legacy product's variable is read when the new one is unset: past the token check, the
    // missing database is what stops it.
    const fs::path missing = root.path() / "missing.db";
    const CliResult legacy = run_cli({"--api", "--database", fv::path_to_utf8(missing)},
                                     api_env(data, {{legacy_prefix() + "API_TOKEN", kApiToken}}));
    CHECK(legacy.exit_code == 1u);
    CHECK_THAT(legacy.err, StartsWith("History API: Database does not exist."));
    CHECK_FALSE(fs::exists(missing));

    // A token that is too short is the History API's own refusal.
    const CliResult short_token = run_cli({"--api", "--database", fv::path_to_utf8(missing)},
                                          api_env(data, {{prefix() + "API_TOKEN", "short"}}));
    CHECK(short_token.exit_code == 1u);
    CHECK_THAT(short_token.err, StartsWith("History API: " + prefix() + "API_TOKEN must be"));

    const CliResult port =
        run_cli({"--api", "--port", "0"}, api_env(data, {{prefix() + "API_TOKEN", kApiToken}}));
    CHECK(port.exit_code == 2u);
    CHECK_THAT(port.err, ContainsSubstring("error: --port must be between 1 and 65535"));

    const CliResult not_a_number = run_cli({"--api", "--port", "x"}, api_env(data));
    CHECK(not_a_number.exit_code == 2u);
    CHECK_THAT(not_a_number.err, ContainsSubstring("argument --port: invalid int value: 'x'"));

    const CliResult extra = run_cli({"--api", "surplus"}, api_env(data));
    CHECK(extra.exit_code == 2u);
    CHECK_THAT(extra.err, ContainsSubstring("unrecognized arguments: surplus"));
}

TEST_CASE("test_cli_launch_uses_read_only_service_without_desktop_imports",
          "[platform][test_agent_api]") {
    fv::testkit::TempDataRoot root;
    const fs::path data = root.path() / "data";
    const fs::path database = root.path() / fv::path_from_utf8("history # café.db");
    fs::copy_file(fv::testkit::source_root() / "tests" / "fixtures" / "agentapi" / "history.db",
                  database);
    const std::uint16_t port = closed_port();
    ChannelProcess server(
        cli_executable(),
        {"--api", "--database", fv::path_to_utf8(database), "--port", std::to_string(port)},
        child_env({{prefix() + "API_TOKEN", kApiToken},
                   {prefix() + "DATA_DIR", fv::path_to_utf8(data)}}));

    std::optional<fv::http::Response> status;
    const auto deadline = std::chrono::steady_clock::now() + kWait;
    while (!(status = get(port, "/v1/status")) && !server.exit_code() &&
           std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(50ms);
    }
    INFO(server.stderr_text());
    REQUIRE(status.has_value());
    CHECK(status->status == 200);

    const std::optional<fv::http::Response> openapi = get(port, "/openapi.json");
    REQUIRE(openapi.has_value());
    CHECK(fv::py::loads(openapi->body)["paths"].contains("/v1/search"));

    // The child serves the History API with no window toolkit and no speech runtime loaded.
    const std::vector<std::string> modules = loaded_modules(server.pid());
#if defined(_WIN32) || defined(__linux__)
    CHECK_FALSE(modules.empty());
#endif
    const std::string runtime =
        fv::py::lower(fv::path_to_utf8(fs::path(fv::runtime_library_name())));
    for (const std::string& name : modules) {
        INFO(name);
        CHECK_FALSE(name.starts_with("qt"));
        CHECK_FALSE(name.starts_with("libqt"));
        CHECK(name != runtime);
    }
    // A database given by path is read in place: no data folder lease, no database created.
    CHECK_FALSE(fs::exists(data / fv::path_from_utf8(fv::identity().database_file)));
    CHECK_NOTHROW((void)fv::DataLease::acquire(data, fv::identity(), fv::LeaseMode::Exclusive));
}

// cli.py: the data folder's own database is served under its shared startup lease, so a restore
// cannot replace it meanwhile; `--database ""` names it too (args.database or DATABASE_FILE).
TEST_CASE("the primary database is served under the data folder's lease",
          "[platform][test_agent_api]") {
    fv::testkit::TempDataRoot root;
    const fs::path data = root.path() / "data";
    fs::create_directories(data);
    fs::copy_file(fv::testkit::source_root() / "tests" / "fixtures" / "agentapi" / "history.db",
                  data / fv::path_from_utf8(fv::identity().database_file));
    const std::uint16_t port = closed_port();
    ChannelProcess server(cli_executable(),
                          {"--api", "--database", "", "--port", std::to_string(port)},
                          child_env({{prefix() + "API_TOKEN", kApiToken},
                                     {prefix() + "DATA_DIR", fv::path_to_utf8(data)}}));

    std::optional<fv::http::Response> status;
    const auto deadline = std::chrono::steady_clock::now() + kWait;
    while (!(status = get(port, "/v1/status")) && !server.exit_code() &&
           std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(50ms);
    }
    INFO(server.stderr_text());
    REQUIRE(status.has_value());
    CHECK(status->status == 200);
    CHECK_THROWS_AS((void)fv::DataLease::acquire(data, fv::identity(), fv::LeaseMode::Exclusive),
                    fv::DataLeaseBusy);
}
