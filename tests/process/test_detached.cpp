// A child that outlives its parent: OpenWhisper's tests/test_updater_helper.py (the detached
// start), against a real helper process, plus subprocess.list2cmdline's quoting.

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "base/utf8.h"
#include "process/detached.h"
#include "testkit/testkit.h"

#ifdef __linux__
#include <fcntl.h>
#include <unistd.h>
#endif

namespace {

std::string read_file(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    std::ostringstream text;
    text << in.rdbuf();
    return text.str();
}

// The helper writes its file through a rename; wait for it to appear.
bool wait_for(const std::filesystem::path& path) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
    while (std::chrono::steady_clock::now() < deadline) {
        if (std::filesystem::exists(path)) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    return false;
}

} // namespace

// from
// tests/test_updater_helper.py::test_start_detached_uses_independent_process_and_reports_failure
TEST_CASE("start_detached uses an independent process and reports failure",
          "[test_updater_helper][platform]") {
    fv::testkit::TempDataRoot root;
    const auto output = root.path() / "child output.txt";
    const auto workdir = root.path() / "work dir";
    std::filesystem::create_directories(workdir);
    const std::filesystem::path helper = FV_DETACHED_HELPER;

    const std::vector<std::string> args{fv::path_to_utf8(output), "--recover", "two words",
                                        "quote\"d", "trailing\\"};
    const auto started = fv::start_detached(helper, args, workdir);
    REQUIRE(started.has_value());
    CHECK(*started != 0);
    REQUIRE(wait_for(output));
    const auto text = read_file(output);
    std::istringstream lines(text);
    std::string cwd;
    std::getline(lines, cwd);
    CHECK(std::filesystem::equivalent(fv::path_from_utf8(cwd), workdir));
    std::vector<std::string> received;
    for (std::string line; std::getline(lines, line);) {
        received.push_back(line);
    }
    CHECK(received == std::vector<std::string>{"--recover", "two words", "quote\"d", "trailing\\"});

    const auto missing = fv::start_detached(root.path() / "missing.exe", {});
    REQUIRE_FALSE(missing.has_value());
    CHECK(missing.error() == "Could not start missing.exe.");
}

TEST_CASE("start_detached runs in the program's folder by default", "[process][platform]") {
    fv::testkit::TempDataRoot root;
    const auto output = root.path() / "out.txt";
    const std::filesystem::path helper = FV_DETACHED_HELPER;
    const std::vector<std::string> args{fv::path_to_utf8(output)};
    REQUIRE(fv::start_detached(helper, args).has_value());
    REQUIRE(wait_for(output));
    std::istringstream lines(read_file(output));
    std::string cwd;
    std::getline(lines, cwd);
    CHECK(std::filesystem::equivalent(fv::path_from_utf8(cwd), helper.parent_path()));
}

#ifdef __linux__
// subprocess.Popen(close_fds=True): a descriptor this process opened without O_CLOEXEC (a data
// lock, a socket) does not reach the child. Windows starts the child with bInheritHandles FALSE.
TEST_CASE("start_detached passes no descriptor to the child", "[process][platform]") {
    fv::testkit::TempDataRoot root;
    const int opened = ::open("/dev/null", O_RDONLY);
    REQUIRE(opened >= 0);
    // F_DUPFD takes the lowest free number from 100, so no descriptor of the runner is replaced.
    const int leaked = ::fcntl(opened, F_DUPFD, 100);
    ::close(opened);
    REQUIRE(leaked >= 100);
    REQUIRE((::fcntl(leaked, F_GETFD) & FD_CLOEXEC) == 0);
    // The shell's own test looks at its own descriptors; "[" is a builtin.
    const std::string probe =
        "if [ -e /proc/self/fd/" + std::to_string(leaked) +
        " ]; then echo open; else echo closed; fi > fd.tmp && mv fd.tmp fd.txt";
    const std::vector<std::string> args{"-c", probe};
    const auto started = fv::start_detached("/bin/sh", args, root.path());
    ::close(leaked);
    REQUIRE(started.has_value());
    REQUIRE(wait_for(root.path() / "fd.txt"));
    CHECK(read_file(root.path() / "fd.txt") == "closed\n");
}
#endif

TEST_CASE("list2cmdline quotes as subprocess does", "[process]") {
    using Args = std::vector<std::string>;
    CHECK(fv::list2cmdline(Args{"a", "b c", ""}) == R"(a "b c" "")");
    CHECK(fv::list2cmdline(Args{R"(a\b)", R"(a\"b)", R"(a\\)"}) == R"(a\b a\\\"b a\\)");
    CHECK(fv::list2cmdline(Args{R"(C:\Program Files\x\)"}) == R"("C:\Program Files\x\\")");
    CHECK(fv::list2cmdline(Args{"tab\there", "/DIR=C:\\a b"}) == "\"tab\there\" \"/DIR=C:\\a b\"");
    CHECK(fv::list2cmdline(Args{}).empty());
}
