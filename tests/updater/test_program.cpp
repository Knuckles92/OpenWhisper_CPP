// The built openwhisper-cpp-updater, run as a child process: the program the installer extracts and
// the build scripts call. OpenWhisper checks its frozen helper the same way
// (tests/test_frozen_updater.py); what each command does is tested in process by
// tests/update_native/test_updater_cli.cpp.

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <string>
#include <vector>

#include "base/atomic_file.h"
#include "base/identity.h"
#include "base/utf8.h"
#ifdef _WIN32
#include "process/run_once.h"
#else
#include <fcntl.h>
#include <sys/wait.h>
#include <unistd.h>
#endif
#include "testkit/testkit.h"
#include "update/update_contract.h"
#include "update_native/archive.h"

namespace {
namespace fs = std::filesystem;

#ifdef FV_UPDATER_EXECUTABLE
struct ProgramResult {
    std::optional<int> exit_code; // none: it could not be started or did not end by itself
    std::string output;           // standard output
    std::string error;            // why it could not be run
};

#ifdef _WIN32
ProgramResult updater(std::vector<std::string> args, const fs::path& cwd) {
    args.insert(args.begin(), fv::path_to_utf8(fs::path(FV_UPDATER_EXECUTABLE)));
    const auto ran = fv::run_once(args, "", 60, {}, cwd);
    return {ran.exit_code, ran.output, ran.error};
}
#else
// fv_process starts programs on Windows only; a pipe, fork and exec do here.
ProgramResult updater(std::vector<std::string> args, const fs::path& cwd) {
    args.insert(args.begin(), FV_UPDATER_EXECUTABLE);
    std::vector<char*> argv;
    for (auto& argument : args)
        argv.push_back(argument.data());
    argv.push_back(nullptr);
    int out[2];
    if (::pipe(out) != 0)
        return {std::nullopt, {}, "pipe failed"};
    const pid_t pid = ::fork();
    if (pid < 0)
        return {std::nullopt, {}, "fork failed"};
    if (pid == 0) {
        ::dup2(out[1], STDOUT_FILENO);
        ::close(out[0]);
        ::close(out[1]);
        const int null = ::open("/dev/null", O_RDWR);
        if (null >= 0) {
            ::dup2(null, STDIN_FILENO);
            ::dup2(null, STDERR_FILENO);
        }
        if (::chdir(cwd.c_str()) == 0)
            ::execv(argv[0], argv.data());
        ::_exit(127);
    }
    ::close(out[1]);
    ProgramResult result;
    char buffer[4096];
    for (;;) {
        const auto count = ::read(out[0], buffer, sizeof(buffer));
        if (count <= 0)
            break;
        result.output.append(buffer, static_cast<std::size_t>(count));
    }
    ::close(out[0]);
    int status = 0;
    if (::waitpid(pid, &status, 0) == pid && WIFEXITED(status))
        result.exit_code = WEXITSTATUS(status);
    else
        result.error = "the program did not end by itself";
    return result;
}
#endif

// Points the per-user folder (LOCALAPPDATA on Windows, XDG_DATA_HOME elsewhere) at `local` for
// the children started meanwhile, so updater.log never lands in the real user's folder.
struct LocalFolder {
    std::vector<std::pair<std::string, std::optional<std::string>>> saved;
    explicit LocalFolder(const fs::path& local) {
        for (const char* name : {"LOCALAPPDATA", "XDG_DATA_HOME"}) {
            saved.emplace_back(name, fv::testkit::get_env(name));
            fv::testkit::set_env(name, fv::path_to_utf8(local));
        }
    }
    ~LocalFolder() {
        for (const auto& [name, value] : saved)
            fv::testkit::set_env(name, value);
    }
    LocalFolder(const LocalFolder&) = delete;
    LocalFolder& operator=(const LocalFolder&) = delete;
};
#endif
} // namespace

TEST_CASE("the built updater writes a manifest, packs a release and verifies it", "[platform]") {
#ifndef FV_UPDATER_EXECUTABLE
    SKIP("openwhisper-cpp-updater is not built in this configuration");
#else
    fv::testkit::TempDataRoot temp;
    const LocalFolder folder(temp.path() / "local");
    const auto tree = temp.path() / "stage";
    fs::create_directories(tree / "platforms");
    fv::write_file_atomic(tree / "openwhisper-cpp.exe", "app");
    fv::write_file_atomic(tree / "openwhisper-cpp-updater.exe", "helper");
    fv::write_file_atomic(tree / "platforms" / "qwindows.dll", "plugin");
    const auto zip = temp.path() / "release.zip";
    const auto stage = fv::path_to_utf8(tree);
    const auto archive = fv::path_to_utf8(zip);

    const auto written = updater({"--write-manifest", stage, "--version", "0.1.1"}, temp.path());
    INFO(FV_UPDATER_EXECUTABLE << ": " << written.error);
    REQUIRE(written.error.empty());
    CHECK(written.exit_code == 0);
    CHECK(fs::is_regular_file(tree / fv::update::manifest_name()));

    const auto packed =
        updater({"--pack-archive", stage, "--version", "0.1.1", "--out", archive}, temp.path());
    CHECK(packed.exit_code == 0);
    // The archive the program wrote is one this library verifies.
    CHECK(fv::update_native::verify_archive(zip, "0.1.1").at("files").size() == 3);

    const auto verified = updater({"--verify-archive", archive, "--version", "0.1.1"}, temp.path());
    CHECK(verified.exit_code == 0);
    CHECK(verified.output.find(" 0.1.1 (3 files).") != std::string::npos);

    // A failure is exit code 1, a command line it does not know exit code 2: neither waits for
    // anyone, since the build scripts and the installer run this program unattended.
    CHECK(updater({"--verify-archive", archive, "--version", "0.1.2"}, temp.path()).exit_code == 1);
    CHECK(updater({}, temp.path()).exit_code == 2);
    CHECK(updater({"--transaction-id", std::string(32, 'a')}, temp.path()).exit_code == 2);
    const auto help = updater({"--help"}, temp.path());
    CHECK(help.exit_code == 0);
    CHECK(help.output.find("openwhisper-cpp-updater --pack-archive") != std::string::npos);
#endif
}

TEST_CASE("the built updater writes updater.log in the product's local folder", "[platform]") {
#ifndef FV_UPDATER_EXECUTABLE
    SKIP("openwhisper-cpp-updater is not built in this configuration");
#else
    fv::testkit::TempDataRoot temp;
    const auto local = temp.path() / "local";
    const auto log_file = local / fv::path_from_utf8(fv::identity().data_dir_name) / "updater.log";
    const LocalFolder folder(local);
    // A packaging command, run by the build scripts, leaves no log.
    const auto zip = fv::path_to_utf8(temp.path() / "missing.zip");
    const auto verified = updater({"--verify-archive", zip}, temp.path());
    const bool logged_packaging = fs::exists(log_file);
    // A command line it does not know is the installer's or the application's: it is logged.
    const auto unknown = updater({"--transaction-id", std::string(32, 'a')}, temp.path());

    INFO(FV_UPDATER_EXECUTABLE << ": " << unknown.error);
    CHECK(verified.exit_code == 1);
    CHECK_FALSE(logged_packaging);
    CHECK(unknown.exit_code == 2);
    REQUIRE(fs::is_regular_file(log_file));
    std::ifstream in(log_file, std::ios::binary);
    const std::string text{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
    CHECK(text.find(" - WARNING - scripts.updater_helper - The updater was started without a "
                    "command it knows (2 arguments).") != std::string::npos);
#endif
}
