// Owning a child's whole tree: OpenWhisper's tests/test_agent_process.py, and the launcher's own
// guarantees (job before the first instruction, kill_tree, disown, pipes, line splitting, the
// command line), with real fv_process_child processes on Windows.

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <limits>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "process/agent_process.h"
#include "process/detail/start_error.h"
#include "process/detail/text.h"
#include "process/launch.h"
#include "process/launcher.h"
#include "process/testing/fake_launch_backend.h"
#include "process/testing/fake_launcher.h"
#include "process_test_support.h"
#include "testkit/testkit.h"

using fv::process_test::child_argv;
using fv::process_test::wait_until;

namespace {

fv::ProcessSpec child_spec(const std::vector<std::string>& commands) {
    fv::ProcessSpec spec;
    spec.program = fv::path_from_utf8(fv::process_test::child());
    spec.args = commands;
    return spec;
}

// Collects a pipe's bytes and its end.
struct Collected {
    std::mutex mutex;
    std::condition_variable changed;
    std::string bytes;
    std::vector<std::string> lines;
    bool ended = false;

    fv::ByteSink bytes_sink() {
        return [this](std::span<const char> chunk) {
            std::lock_guard lock(mutex);
            if (chunk.empty()) {
                ended = true;
            } else {
                bytes.append(chunk.data(), chunk.size());
            }
            changed.notify_all();
        };
    }
    fv::LineSink line_sink() {
        return [this](std::string_view line) {
            std::lock_guard lock(mutex);
            lines.emplace_back(line);
            changed.notify_all();
        };
    }
    bool wait_ended() {
        std::unique_lock lock(mutex);
        return changed.wait_for(lock, std::chrono::seconds(10), [&] { return ended; });
    }
};

} // namespace

TEST_CASE("windows_command_line quotes as subprocess.list2cmdline does", "[process]") {
    using Args = std::vector<std::string>;
    CHECK(fv::windows_command_line(Args{"a", "b c", ""}) == R"(a "b c" "")");
    CHECK(fv::windows_command_line(Args{"tab\there"}) == "\"tab\there\"");
    CHECK(fv::windows_command_line(Args{R"(say "hi")"}) == R"("say \"hi\"")");
    CHECK(fv::windows_command_line(Args{R"(a\"b)"}) == R"(a\\\"b)");
    CHECK(fv::windows_command_line(Args{R"(C:\dir\)"}) == R"(C:\dir\)");
    CHECK(fv::windows_command_line(Args{R"(C:\a dir\)"}) == R"("C:\a dir\\")");
    CHECK(fv::windows_command_line(Args{R"(a\\b)"}) == R"(a\\b)");
    CHECK(fv::windows_command_line(Args{}).empty());
}

TEST_CASE("start errors read as programs.py _start_error words them", "[process]") {
    using fv::process_detail::start_error;
    CHECK(start_error(2) == "The program wasn't found.");
    CHECK(start_error(3) == "The program wasn't found.");
    CHECK(start_error(206) == "The program wasn't found.");
    CHECK(start_error(5) == "The program isn't allowed to run (permission denied).");
    CHECK(start_error(32) == "The program isn't allowed to run (permission denied).");
    CHECK(start_error(193) == "Windows can't run that file directly. Choose a program (.exe).");
    CHECK(start_error(87) == "The program couldn't start (OSError 87).");
    CHECK(start_error(267) == "The program couldn't start (NotADirectoryError 267).");
    CHECK(start_error(1455) == "The program couldn't start (OSError 1455).");
}

TEST_CASE("bytes decode as errors=replace does", "[process]") {
    using fv::process_detail::decode_utf8_replace;
    const std::string fffd = "\xEF\xBF\xBD";
    CHECK(decode_utf8_replace("plain \xC3\xA9") == "plain \xC3\xA9");
    CHECK(decode_utf8_replace("\xFF") == fffd);
    CHECK(decode_utf8_replace("\xE2\x82") == fffd); // a truncated sequence: one
    CHECK(decode_utf8_replace("\xE2\x82x") == fffd + "x");
    CHECK(decode_utf8_replace("\xF0\x80\x80") == fffd + fffd + fffd); // overlong: each byte
    CHECK(decode_utf8_replace("\xED\xA0\x80") == fffd + fffd + fffd); // a surrogate
    CHECK(decode_utf8_replace("\xF4\x90\x80\x80") == fffd + fffd + fffd + fffd);
    CHECK(decode_utf8_replace("\xF0\x9F\x98\x80") == "\xF0\x9F\x98\x80");
}

TEST_CASE("lines split at LF, drop a CR before it and keep the unterminated rest", "[process]") {
    std::vector<std::string> lines;
    auto sink =
        fv::process_detail::line_splitter([&](std::string_view line) { lines.emplace_back(line); });
    const std::string first = "one\r\ntw";
    const std::string second = "o\n\xFF\nlast";
    sink(std::span<const char>(first.data(), first.size()));
    sink(std::span<const char>(second.data(), second.size()));
    sink({});
    CHECK(lines == std::vector<std::string>{"one", "two", "\xEF\xBF\xBD", "last"});
}

TEST_CASE("an agent probe runs windowless with NUL for stdin in a job", "[process]") {
    fv::FakeProcessLauncher launcher;
    fv::FakeProcessScript script;
    script.stdout_bytes = "codex 0.1\r\n";
    script.stderr_bytes = "warn\r";
    launcher.push(script);
    fv::testkit::TempDataRoot root;
    const auto result = fv::run_agent_probe({"codex.exe", "--version"}, 5, root.path(), &launcher);
    CHECK(result.code == 0);
    CHECK(result.text == "codex 0.1\nwarn\n"); // text mode: line ends read as "\n"
    const auto spec = launcher.specs().at(0);
    CHECK(spec.stdin_null);
    CHECK(spec.pipes);
    CHECK(spec.own_tree);
    CHECK_FALSE(spec.new_console);
    CHECK(spec.cwd == root.path());
    CHECK(launcher.records().at(0)->kill_calls >= 1); // the tree is ended whatever happened
}

TEST_CASE("an agent probe that can't start or doesn't finish gives nothing", "[process]") {
    fv::FakeProcessLauncher launcher;
    fv::FakeProcessScript unstartable;
    unstartable.start_error = "The program wasn't found.";
    launcher.push(unstartable);
    fv::FakeProcessScript endless;
    endless.exit_code = std::nullopt;
    endless.stdout_bytes = "partial";
    launcher.push(endless);
    for (int i = 0; i < 2; ++i) {
        const auto result = fv::run_agent_probe({"agent.exe"}, 0.1, {}, &launcher);
        CHECK_FALSE(result.code);
        CHECK(result.text.empty());
    }
    CHECK(launcher.records().at(0)->killed);
}

TEST_CASE("waits as long as the clock allows end with the process", "[process]") {
    fv::FakeProcessLauncher launcher;
    fv::FakeProcessScript script;
    script.runs_for = std::chrono::milliseconds(100);
    script.exit_code = 3;
    launcher.push(script);
    auto started = launcher.start({});
    REQUIRE(started.has_value());
    CHECK((*started)->wait(std::chrono::milliseconds::max()) == 3);

    launcher.push(script);
    const auto probe =
        fv::run_agent_probe({"agent.exe"}, std::numeric_limits<double>::infinity(), {}, &launcher);
    CHECK(probe.code == 3);
}

TEST_CASE("the shell opens links and shows files through the launch backend", "[process]") {
    auto backend = std::make_shared<fv::FakeLaunchBackend>();
    fv::process_test::ScopedLaunchBackend installed(backend);
    backend->set_assoc("https", fv::AssocString::delegate_execute, 0, "{web}");
    const auto shell = fv::make_shell();
    CHECK(shell->open_url("https://example.com/"));
    CHECK_FALSE(shell->open_url(""));
    REQUIRE(backend->explorer_opened().size() == 1);
    CHECK(backend->explorer_opened()[0].first == "https://example.com/");

    fv::testkit::TempDataRoot root;
    const auto file = root.path() / "report.txt";
    CHECK(shell->reveal(file));
    CHECK(backend->revealed() == std::vector<std::filesystem::path>{file});

    CHECK(shell->launch_via_explorer(file, "--flag"));
    CHECK(backend->explorer_opened().back() ==
          std::pair<std::string, std::string>{fv::path_to_utf8(file), "--flag"});
    backend->set_explorer([](std::string_view, std::string_view) { return false; });
    CHECK(shell->launch_via_explorer(file, ""));
    REQUIRE(backend->opened_here().size() == 1);
    CHECK(backend->opened_here()[0].first == fv::path_to_utf8(file));
}

#ifdef _WIN32

// from tests/test_agent_process.py::test_cleanup_reaps_helpers_even_after_parent_exit
TEST_CASE("cleanup reaps helpers even after parent exit", "[test_agent_process][platform]") {
    const bool parent_exits = GENERATE(true, false);
    CAPTURE(parent_exits);
    fv::testkit::TempDataRoot root;
    const auto pid_file = root.path() / "helper.pid";
    std::vector<std::string> commands{
        "spawn", fv::path_to_utf8(pid_file), "null", "null", "sleep", "20", ";;"};
    if (!parent_exits) {
        commands.insert(commands.end(), {"sleep", "20"});
    }
    auto spec = child_spec(commands);
    spec.stdin_null = true;
    auto started = fv::make_process_launcher()->start(spec);
    REQUIRE(started.has_value());
    auto& process = **started;
    fv::process_test::PidGuard guard;
    const auto helper = fv::process_test::read_pid(pid_file);
    REQUIRE(helper);
    guard.add(*helper);
    CHECK(fv::process_test::alive(*helper));
    if (parent_exits) {
        CHECK(process.wait(std::chrono::seconds(5)) == 0);
    }
    fv::kill_process_tree(process);
    CHECK(wait_until([&] { return !fv::process_test::alive(*helper); }, std::chrono::seconds(3)));
    CHECK(process.exit_code().has_value());
    fv::kill_process_tree(process); // repeated driver/finally cleanup is harmless
}

// from tests/test_agent_process.py::test_probe_timeout_closes_owned_process
TEST_CASE("probe timeout closes owned process", "[test_agent_process][platform]") {
    fv::testkit::TempDataRoot root;
    const auto marker = root.path() / "late-write";
    const auto result = fv::run_agent_probe(
        child_argv({"sleep", "0.5", "write-file", fv::path_to_utf8(marker), "x"}), 0.1);
    CHECK_FALSE(result.code);
    CHECK(result.text.empty());
    std::this_thread::sleep_for(std::chrono::milliseconds(600));
    CHECK_FALSE(std::filesystem::exists(marker));
}

TEST_CASE("an agent probe returns its exit code and output", "[process][platform]") {
    const auto result =
        fv::run_agent_probe(child_argv({"print", "agent 1.2.3", "emit-err", "3", "exit", "2"}), 10);
    CHECK(result.code == 2);
    CHECK(result.text == "agent 1.2.3\nEEE");
}

TEST_CASE("a child is in its job before it runs", "[process][platform]") {
    // Whatever started the tests may hold every process in a job of its own; without own_tree
    // the child reports "0", so a "1" is the launcher's job.
    const bool own_tree = GENERATE(true, false);
    CAPTURE(own_tree);
    auto spec = child_spec({"in-job"});
    spec.pipes = true;
    spec.own_tree = own_tree;
    Collected out;
    auto started = fv::make_process_launcher()->start(spec);
    REQUIRE(started.has_value());
    (*started)->on_bytes(out.bytes_sink(), nullptr);
    (*started)->close_stdin();
    REQUIRE(out.wait_ended());
    CHECK(out.bytes == (own_tree ? "1" : "0"));
    CHECK((*started)->wait(std::chrono::seconds(5)) == 0);
}

TEST_CASE("kill_tree ends a grandchild", "[process][platform]") {
    fv::testkit::TempDataRoot root;
    const auto pid_file = root.path() / "helper.pid";
    fv::process_test::PidGuard guard;
    auto spec = child_spec(
        {"spawn", fv::path_to_utf8(pid_file), "null", "null", "sleep", "60", ";;", "sleep", "60"});
    spec.stdin_null = true;
    auto started = fv::make_process_launcher()->start(spec);
    REQUIRE(started.has_value());
    const auto helper = fv::process_test::read_pid(pid_file);
    REQUIRE(helper);
    guard.add(*helper);
    guard.add((*started)->pid());
    (*started)->kill_tree();
    CHECK((*started)->wait(std::chrono::seconds(5)).has_value());
    CHECK(wait_until([&] { return !fv::process_test::alive(*helper); }, std::chrono::seconds(5)));
}

TEST_CASE("destroying a process that still owns its job ends the tree", "[process][platform]") {
    fv::testkit::TempDataRoot root;
    const auto pid_file = root.path() / "helper.pid";
    fv::process_test::PidGuard guard;
    std::uint32_t parent = 0;
    std::optional<std::uint32_t> helper;
    {
        auto spec = child_spec({"spawn", fv::path_to_utf8(pid_file), "null", "null", "sleep", "60",
                                ";;", "sleep", "60"});
        spec.stdin_null = true;
        auto started = fv::make_process_launcher()->start(spec);
        REQUIRE(started.has_value());
        parent = (*started)->pid();
        guard.add(parent);
        helper = fv::process_test::read_pid(pid_file);
        REQUIRE(helper);
        guard.add(*helper);
    }
    CHECK(wait_until([&] { return !fv::process_test::alive(*helper); }, std::chrono::seconds(5)));
    CHECK(wait_until([&] { return !fv::process_test::alive(parent); }, std::chrono::seconds(5)));
}

TEST_CASE("disown lets what a child started outlive it", "[process][platform]") {
    fv::testkit::TempDataRoot root;
    const auto pid_file = root.path() / "helper.pid";
    const auto marker = root.path() / "marker.txt";
    fv::process_test::PidGuard guard;
    {
        auto spec = child_spec({"spawn", fv::path_to_utf8(pid_file), "null", "null", "sleep", "1",
                                "write-file", fv::path_to_utf8(marker), "ok", ";;"});
        spec.stdin_null = true;
        auto started = fv::make_process_launcher()->start(spec);
        REQUIRE(started.has_value());
        CHECK((*started)->wait(std::chrono::seconds(10)) == 0);
        (*started)->disown();
    }
    const auto helper = fv::process_test::read_pid(pid_file);
    REQUIRE(helper);
    guard.add(*helper);
    CHECK(wait_until([&] { return std::filesystem::exists(marker); }));
}

TEST_CASE("disown_on_clean_exit leaves what a cleanly exited child started",
          "[process][platform]") {
    fv::testkit::TempDataRoot root;
    const auto pid_file = root.path() / "helper.pid";
    const auto marker = root.path() / "marker.txt";
    fv::process_test::PidGuard guard;
    {
        auto spec = child_spec({"spawn", fv::path_to_utf8(pid_file), "null", "null", "sleep", "1",
                                "write-file", fv::path_to_utf8(marker), "ok", ";;"});
        spec.stdin_null = true;
        spec.disown_on_clean_exit = true;
        auto started = fv::make_process_launcher()->start(spec);
        REQUIRE(started.has_value());
        CHECK((*started)->wait(std::chrono::seconds(10)) == 0);
    }
    const auto helper = fv::process_test::read_pid(pid_file);
    REQUIRE(helper);
    guard.add(*helper);
    CHECK(wait_until([&] { return std::filesystem::exists(marker); }));
}

TEST_CASE("on_line splits CRLF and replaces invalid UTF-8", "[process][platform]") {
    auto spec = child_spec({"write-hex", "6f6e650d0a74776f0aff6e6f0a7461696c"});
    spec.pipes = true;
    Collected out;
    Collected err;
    auto started = fv::make_process_launcher()->start(spec);
    REQUIRE(started.has_value());
    (*started)->on_line(out.line_sink(), err.line_sink());
    (*started)->close_stdin();
    CHECK((*started)->wait(std::chrono::seconds(10)) == 0);
    (*started)->release_pipes(std::chrono::seconds(5));
    CHECK(out.lines == std::vector<std::string>{"one", "two",
                                                "\xEF\xBF\xBD"
                                                "no",
                                                "tail"});
    CHECK(err.lines.empty());
}

TEST_CASE("a child gets the environment it is given, or the user's", "[process][platform]") {
    const std::string name = "FV_PROCESS_ENV_PROBE";
    fv::process_test::ScopedEnv own(name, "inherited");
    for (const bool given : {true, false}) {
        auto spec = child_spec({"env", name});
        spec.pipes = true;
        if (given) {
            spec.env = {{name, "given"},
                        {"SYSTEMROOT", fv::testkit::get_env("SYSTEMROOT").value_or("")}};
        }
        Collected out;
        auto started = fv::make_process_launcher()->start(spec);
        REQUIRE(started.has_value());
        (*started)->on_bytes(out.bytes_sink(), nullptr);
        (*started)->close_stdin();
        REQUIRE(out.wait_ended());
        CHECK(out.bytes == (given ? "given\n" : "inherited\n"));
    }
}

TEST_CASE("a start that fails says why as run_once shows it", "[process][platform]") {
    fv::testkit::TempDataRoot root;
    const auto launcher = fv::make_process_launcher();
    fv::ProcessSpec missing;
    missing.program = root.path() / "nope.exe";
    CHECK(launcher->start(missing).error() == "The program wasn't found.");
    // A file that is no program is not started here: Windows may answer one that looks like a
    // DOS program with a message box instead of an error.
    fv::ProcessSpec nul;
    nul.program = fv::path_from_utf8(fv::process_test::child());
    nul.args = {std::string("a\0b", 3)};
    CHECK(launcher->start(nul).error() ==
          "The program couldn't start (its arguments aren't valid).");
}

#endif // _WIN32
