// The crash handler, exercised in a child process (fv_log_crash_helper) so this process keeps
// Catch2's own handlers.

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <filesystem>
#include <regex>
#include <string>

#include "base/identity.h"
#include "base/paths.h"
#include "base/testing/map_env_reader.h"
#include "base/utf8.h"
#include "log/crash_handler.h"
#include "log_test_support.h"

namespace flog = fv::log;
using fv::logtest::TempDir;

namespace {

fv::Paths paths_for(const std::filesystem::path& root) {
    fv::MapEnvReader env;
    env.set(std::string(fv::identity().env_prefix) + "DATA_DIR", fv::path_to_utf8(root));
    fv::pin_per_user_folders(env, root / "per-user");
    return fv::Paths::resolve(fv::identity(), env);
}

} // namespace

TEST_CASE("crash lines round-trip through parse_line", "[log]") {
    const auto info = flog::CrashHandler::parse_line(
        "2026-11-14T10:20:30Z Fatal error 0xC0000005 in fv_log_crash_helper.exe; minidump "
        "crashes\\20261114-102030-1234.dmp\r\n");
    REQUIRE(info);
    CHECK(info->utc == "2026-11-14T10:20:30Z");
    CHECK(info->code == 0xC0000005u);
    CHECK(info->module == "fv_log_crash_helper.exe");
    CHECK(info->dump == "crashes\\20261114-102030-1234.dmp");

    const auto signal = flog::CrashHandler::parse_line(
        "2026-11-14T10:20:30Z Fatal error 0x0000000B in unknown; no minidump");
    REQUIRE(signal);
    CHECK(signal->code == 11u);
    CHECK(signal->module == "unknown");
    CHECK(signal->dump.empty());

    CHECK_FALSE(flog::CrashHandler::parse_line("Fatal Python error: Segmentation fault"));
    CHECK_FALSE(flog::CrashHandler::parse_line("x Fatal error 0xZZ in a; no minidump"));
    CHECK_FALSE(flog::CrashHandler::parse_line(""));
}

TEST_CASE("the crash log is the log file with_suffix(.crash.log), the dumps go to crashes",
          "[log]") {
    TempDir root;
    const auto paths = paths_for(root.path());
    CHECK(flog::CrashHandler::crash_log_file(paths, fv::identity()).filename() ==
          std::filesystem::path(std::string(fv::identity().log_file)).stem().string() +
              ".crash.log");
    CHECK(flog::CrashHandler::dump_folder(paths) == paths.data_root() / "crashes");
    CHECK_FALSE(flog::CrashHandler::last_crash(paths, fv::identity()));
}

TEST_CASE("last_crash returns the last parseable line of the crash log", "[log]") {
    TempDir root;
    const auto paths = paths_for(root.path());
    fv::logtest::write_file(
        flog::CrashHandler::crash_log_file(paths, fv::identity()),
        "Fatal Python error: Segmentation fault\n"
        "2026-11-13T09:00:00Z Fatal error 0xC0000409 in ucrtbase.dll; no minidump\n"
        "2026-11-14T10:20:30Z Fatal error 0xC0000005 in nemo_speech.dll; minidump "
        "crashes\\20261114-102030-77.dmp\n"
        "Current thread 0x0001 (most recent call first):\n");
    const auto crash = flog::CrashHandler::last_crash(paths, fv::identity());
    REQUIRE(crash);
    CHECK(crash->utc == "2026-11-14T10:20:30Z");
    CHECK(crash->module == "nemo_speech.dll");
}

#if defined(_WIN32)

namespace {

std::optional<std::uint32_t> run_helper(const std::string& mode,
                                        const std::filesystem::path& root) {
    return fv::logtest::run_child(FV_LOG_CRASH_HELPER, {mode, fv::path_to_utf8(root)},
                                  std::chrono::seconds(120));
}

} // namespace

TEST_CASE("crash handler writes a minidump for a deliberate access violation in a child process",
          "[platform][log]") {
    TempDir root;
    const auto exit_code = run_helper("access-violation", root.path());
    REQUIRE(exit_code);
    CHECK(*exit_code == 0xC0000005u);

    const auto paths = paths_for(root.path());
    const auto crash = flog::CrashHandler::last_crash(paths, fv::identity());
    REQUIRE(crash);
    CHECK(crash->code == 0xC0000005u);
    CHECK(crash->module == "fv_log_crash_helper.exe");
    CHECK(std::regex_match(crash->utc, std::regex(R"(\d{4}-\d\d-\d\dT\d\d:\d\d:\d\dZ)")));
    CHECK(std::regex_match(crash->dump, std::regex(R"(crashes\\\d{8}-\d{6}-\d+\.dmp)")));
    const auto dump = paths.data_root() / std::filesystem::path(crash->dump);
    REQUIRE(std::filesystem::exists(dump));
    // A minidump starts with "MDMP" and holds at least the faulting thread's stack.
    const std::string bytes = fv::logtest::read_file(dump);
    CHECK(bytes.starts_with("MDMP"));
    CHECK(bytes.size() > 4096);
}

TEST_CASE("crash handler writes a minidump when std::terminate is called", "[platform][log]") {
    TempDir root;
    const auto exit_code = run_helper("terminate", root.path());
    REQUIRE(exit_code);
    CHECK(*exit_code == flog::kCrashTerminate);
    const auto crash = flog::CrashHandler::last_crash(paths_for(root.path()), fv::identity());
    REQUIRE(crash);
    CHECK(crash->code == flog::kCrashTerminate);
    CHECK_FALSE(crash->dump.empty());
}

// A crash in a DLL's detach code at exit finds fv-crash-watch already ended by ExitProcess.
TEST_CASE("a crash with the watcher gone writes the line without a dump and does not wait",
          "[platform][log]") {
    TempDir root;
    const auto started = std::chrono::steady_clock::now();
    const auto exit_code = run_helper("watcher-gone", root.path());
    REQUIRE(exit_code);
    CHECK(*exit_code == 0xC0000005u);
    // The faulting thread would otherwise wait out the 60 s dump timeout.
    CHECK(std::chrono::steady_clock::now() - started < std::chrono::seconds(30));
    const auto paths = paths_for(root.path());
    const auto crash = flog::CrashHandler::last_crash(paths, fv::identity());
    REQUIRE(crash);
    CHECK(crash->code == 0xC0000005u);
    CHECK(crash->module == "fv_log_crash_helper.exe");
    CHECK(crash->dump.empty());
    CHECK_FALSE(std::filesystem::exists(flog::CrashHandler::dump_folder(paths)));
}

#endif
