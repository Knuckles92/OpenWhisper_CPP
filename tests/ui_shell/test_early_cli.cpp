// openwhisper-cpp.exe's early command line: dispatch on the first argument only, the usage, and the
// executable itself answering --help, --version and --self-test-gui before it takes the data
// lease or writes its log (the GUI half of tests/test_main_entrypoint.py's
// test_help_prints_usage_and_exits_without_opening_the_app; the PARITY keys are M0.12's).
#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

#include "base/identity.h"
#include "base/json.h"
#include "engine/version.h"
#include "testkit/testkit.h"
#include "ui_shell/early_cli.h"
#include "ui_shell/self_test.h"

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

using fv::EarlyCommand;

namespace {

std::vector<std::string> line(std::initializer_list<const char*> args) {
    std::vector<std::string> made{"openwhisper-cpp"};
    for (const char* arg : args) {
        made.emplace_back(arg);
    }
    return made;
}

EarlyCommand parsed(std::initializer_list<const char*> args) {
    return fv::parseEarlyCli(line(args)).command;
}

#if defined(_WIN32) && defined(FV_GUI_EXECUTABLE)

struct Run {
    std::optional<DWORD> exitCode;
    std::string out;
    std::string err;
};

std::string readFile(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

HANDLE inheritable(const std::filesystem::path& path, DWORD access, DWORD disposition) {
    SECURITY_ATTRIBUTES security{sizeof(security), nullptr, TRUE};
    return CreateFileW(path.c_str(), access, FILE_SHARE_READ | FILE_SHARE_WRITE, &security,
                       disposition, FILE_ATTRIBUTE_NORMAL, nullptr);
}

// Runs openwhisper-cpp.exe with its standard streams in files. The child inherits this process's
// environment: the test's own data folder and the offscreen platform, so even a dispatch that
// went wrong could put nothing on the screen.
Run runGui(const std::vector<std::wstring>& args, const std::filesystem::path& scratch) {
    std::wstring command = L"\"" + std::filesystem::path(FV_GUI_EXECUTABLE).native() + L"\"";
    for (const std::wstring& arg : args) {
        command += L" \"" + arg + L"\"";
    }
    const auto outPath = scratch / "out.txt";
    const auto errPath = scratch / "err.txt";
    HANDLE out = inheritable(outPath, GENERIC_WRITE, CREATE_ALWAYS);
    HANDLE err = inheritable(errPath, GENERIC_WRITE, CREATE_ALWAYS);
    HANDLE in = inheritable(L"NUL", GENERIC_READ, OPEN_EXISTING);
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdInput = in;
    startup.hStdOutput = out;
    startup.hStdError = err;
    PROCESS_INFORMATION info{};
    Run run;
    if (CreateProcessW(nullptr, command.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr,
                       nullptr, &startup, &info)) {
        if (WaitForSingleObject(info.hProcess, 60'000) == WAIT_OBJECT_0) {
            DWORD code = 0;
            GetExitCodeProcess(info.hProcess, &code);
            run.exitCode = code;
        } else {
            TerminateProcess(info.hProcess, 1);
            WaitForSingleObject(info.hProcess, 10'000);
        }
        CloseHandle(info.hThread);
        CloseHandle(info.hProcess);
    }
    CloseHandle(out);
    CloseHandle(err);
    CloseHandle(in);
    run.out = readFile(outPath);
    run.err = readFile(errPath);
    return run;
}

// What CRLF-translating text mode wrote, as "\n".
std::string unixLines(std::string text) {
    std::string out;
    for (std::size_t i = 0; i < text.size(); ++i) {
        if (text[i] == '\r' && i + 1 < text.size() && text[i + 1] == '\n') {
            continue;
        }
        out += text[i];
    }
    return out;
}

#endif

} // namespace

TEST_CASE("dispatch is on the first argument only", "[test_main_entrypoint][unit]") {
    CHECK(parsed({}) == EarlyCommand::Gui);
    CHECK(parsed({"--help"}) == EarlyCommand::Help);
    CHECK(parsed({"-h"}) == EarlyCommand::Help);
    CHECK(parsed({"--help", "--version"}) == EarlyCommand::Help);
    CHECK(parsed({"--version"}) == EarlyCommand::Version);
    CHECK(parsed({"--version", "extra"}) == EarlyCommand::Gui);
    CHECK(parsed({"extra", "--version"}) == EarlyCommand::Gui);
    CHECK(parsed({"--self-test-gui"}) == EarlyCommand::SelfTestGui);
    CHECK(parsed({"--self-test-gui", "x"}) == EarlyCommand::Gui);
    CHECK(parsed({"--self-test"}) == EarlyCommand::Gui);
    CHECK(parsed({"--diagnostics"}) == EarlyCommand::Diagnostics);
    CHECK(parsed({"-H"}) == EarlyCommand::Gui);
    CHECK(parsed({"--api"}) == EarlyCommand::Gui);

    const fv::EarlyCli diagnostics = fv::parseEarlyCli(line({"--diagnostics", "out.json", "x"}));
    CHECK(diagnostics.rest == std::vector<std::string>{"out.json", "x"});
}

TEST_CASE("the usage names this executable's options in OpenWhisper's layout", "[unit]") {
    const std::string usage =
        fv::guiUsageText("openwhisper-cpp", "OpenWhisper_CPP", "OPENWHISPER_CPP_");
    CHECK(usage.starts_with(
        "Usage: openwhisper-cpp [OPTION]\n\nWith no option, opens OpenWhisper_CPP.\n"));
    for (const char* option :
         {"  -h, --help              Show this help and exit.\n",
          "  --version               Print the version and exit.\n",
          "  --diagnostics OUTPUT.json\n", "  --api [--database PATH] [--port N]\n",
          "  OPENWHISPER_CPP_API_TOKEN to a random token of at least\n",
          "  --channel               Run as a Claude Code channel",
          "forwards OpenWhisper_CPP actions into a\n", "session. OpenWhisper_CPP must be\n",
          "  --self-test-gui         ", "  OPENWHISPER_CPP_DATA_DIR=PATH\n",
          "  OPENWHISPER_CPP_LOG_LEVEL=LEVEL\n"}) {
        CHECK(usage.find(option) != std::string::npos);
    }
    std::istringstream lines(usage);
    for (std::string text; std::getline(lines, text);) {
        CHECK(text.size() <= 78);
    }
    CHECK(usage.find("openwhisper") == std::string::npos);
}

TEST_CASE("diagnostics refuses a missing path with argparse's usage", "[unit]") {
    CHECK(fv::runDiagnostics(std::vector<std::string>{}, "openwhisper-cpp.exe") == 2);
    CHECK(fv::runDiagnostics(std::vector<std::string>{"--bogus", "x.json"},
                             "openwhisper-cpp.exe") == 2);
}

TEST_CASE("diagnostics writes a new report and never replaces a file", "[unit]") {
    fv::testkit::TempDataRoot root;
    const auto output = root.path() / "report.json";
    const std::string path = output.string();
    CHECK(fv::runDiagnostics(std::vector<std::string>{path}, "openwhisper-cpp.exe") == 0);
    REQUIRE(std::filesystem::exists(output));
    const auto size = std::filesystem::file_size(output);
    CHECK(fv::runDiagnostics(std::vector<std::string>{path}, "openwhisper-cpp.exe") == 1);
    CHECK(std::filesystem::file_size(output) == size);
}

TEST_CASE("the GUI self-test passes offscreen", "[widget]") {
    const std::vector<std::string> problems = fv::selfTestGuiProblems();
    for (const std::string& problem : problems) {
        UNSCOPED_INFO(problem);
    }
    CHECK(problems.empty());
}

#if defined(_WIN32) && defined(FV_GUI_EXECUTABLE)

// The GUI half of
// tests/test_main_entrypoint.py::test_help_prints_usage_and_exits_without_opening_the_app
TEST_CASE("help prints usage and exits without opening the app", "[platform]") {
    for (const wchar_t* flag : {L"--help", L"-h"}) {
        fv::testkit::TempDataRoot root;
        const Run run = runGui({flag}, root.path());
        REQUIRE(run.exitCode.has_value());
        CHECK(*run.exitCode == 0);
        CHECK(unixLines(run.out) == fv::guiUsageText("openwhisper-cpp", fv::identity().product,
                                                     fv::identity().env_prefix));
        // Before the lease and the logging: neither the lock nor the log exists.
        CHECK_FALSE(std::filesystem::exists(root.path() / ".openwhisper-cpp-data.lock"));
        CHECK_FALSE(std::filesystem::exists(root.path() / "openwhisper-cpp.log"));
    }
}

TEST_CASE("version prints the product and version to the console", "[platform]") {
    fv::testkit::TempDataRoot root;
    const Run run = runGui({L"--version"}, root.path());
    REQUIRE(run.exitCode.has_value());
    CHECK(*run.exitCode == 0);
    CHECK(unixLines(run.out) == "OpenWhisper_CPP " + std::string(fv::kVersion) + "\n");
    CHECK_FALSE(std::filesystem::exists(root.path() / ".openwhisper-cpp-data.lock"));
}

TEST_CASE("the executable's GUI self-test exits 0 offscreen", "[platform]") {
    fv::testkit::TempDataRoot root;
    const Run run = runGui({L"--self-test-gui"}, root.path());
    REQUIRE(run.exitCode.has_value());
    CHECK(*run.exitCode == 0);
    CHECK(unixLines(run.out) == "OpenWhisper_CPP package self-test passed\n");
    CHECK(run.err.empty());
}

// The workflow smoke is openwhisper-cpp-cli's (M3.md change 6): the windowed executable refuses it
// with its usage line, before the launch gate, the lease or the log.
TEST_CASE("the executable refuses the workflow smoke with its usage line", "[platform]") {
    fv::testkit::TempDataRoot root;
    const Run run = runGui({L"--workflow-smoke"}, root.path());
    REQUIRE(run.exitCode.has_value());
    CHECK(*run.exitCode == 2);
    CHECK(unixLines(run.err) == "Usage: openwhisper-cpp [OPTION]\n");
    CHECK(run.out.empty());
    CHECK_FALSE(std::filesystem::exists(root.path() / ".openwhisper-cpp-data.lock"));
    CHECK_FALSE(std::filesystem::exists(root.path() / "openwhisper-cpp.log"));
}

TEST_CASE("diagnostics without a path exits 2 from the executable", "[platform]") {
    fv::testkit::TempDataRoot root;
    const Run run = runGui({L"--diagnostics"}, root.path());
    REQUIRE(run.exitCode.has_value());
    CHECK(*run.exitCode == 2);
    CHECK(unixLines(run.err) == "usage: openwhisper-cpp.exe [-h] output\nopenwhisper-cpp.exe: "
                                "error: the following arguments are "
                                "required: output\n");
}

// The build tree is never the folder an install registration names, so the executable's report
// is not packaged (A01-29; the packaged case is in test_diagnostics_packaged.cpp).
TEST_CASE("the executable in the build tree reports itself not packaged", "[platform]") {
    fv::testkit::TempDataRoot root;
    const auto output = root.path() / "support.json";
    const Run run = runGui({L"--diagnostics", output.native()}, root.path());
    REQUIRE(run.exitCode.has_value());
    CHECK(*run.exitCode == 0);
    CHECK(fv::Json::parse(readFile(output))["packaged"] == false);
}

#endif
