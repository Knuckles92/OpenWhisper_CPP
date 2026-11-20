// run_once with real child processes (fv_process_child): OpenWhisper's
// tests/test_automation_hooks_programs.py (the run_once part),
// tests/test_automation_fix_actions_programs.py, tests/test_automation_r2c_programs.py and
// tests/test_automation_r3c_programs.py, where Python ran sys.executable -c. Every helper a test
// starts is ended when the test ends.

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <filesystem>
#include <future>
#include <limits>
#include <string>
#include <thread>
#include <vector>

#include "base/cancel.h"
#include "base/identity.h"
#include "base/json.h"
#include "credentials/dotenv.h"
#include "process/process_env.h"
#include "process/run_once.h"
#include "process/testing/fake_launcher.h"
#include "process_test_support.h"
#include "testkit/testkit.h"

using fv::run_once;
using fv::process_test::child_argv;
using fv::process_test::wait_until;

namespace {

std::string trimmed(std::string text) {
    while (!text.empty() && (text.back() == '\n' || text.back() == '\r' || text.back() == ' ')) {
        text.pop_back();
    }
    return text;
}

} // namespace

TEST_CASE("run_once refuses before starting anything", "[process]") {
    fv::FakeProcessLauncher launcher;
    fv::testkit::TempDataRoot root;
    CHECK(run_once({}, "", 5, {}, {}, {}, &launcher).error == "Choose the program to run.");
    CHECK(run_once({"  "}, "", 5, {}, {}, {}, &launcher).error == "Choose the program to run.");
    CHECK(run_once({"x.exe"}, "", 5, {}, root.path() / "gone", {}, &launcher).error ==
          "The working folder doesn't exist.");
    CHECK(launcher.specs().empty());
}

TEST_CASE("run_once starts windowless in its own group and job with stderr discarded",
          "[process]") {
    fv::FakeProcessLauncher launcher;
    fv::FakeProcessScript script;
    script.stdout_bytes = "out";
    script.exit_code = 4;
    launcher.push(script);
    const auto result =
        run_once({"tool.exe", "a b"}, "input", 5, {}, {}, {{"EXTRA", "1"}}, &launcher);
    CHECK(result.exit_code == 4);
    CHECK(result.output == "out");
    CHECK(result.error.empty());
    const auto specs = launcher.specs();
    REQUIRE(specs.size() == 1);
    const auto& spec = specs[0];
    CHECK(spec.program == fv::path_from_utf8("tool.exe"));
    CHECK(spec.args == std::vector<std::string>{"a b"});
    CHECK_FALSE(spec.new_console); // CREATE_NO_WINDOW
    CHECK(spec.own_tree);
    CHECK(spec.pipes);
    CHECK(spec.stderr_null);
    CHECK(spec.new_process_group);
    CHECK(spec.env.at("EXTRA") == "1");
    const auto record = launcher.records().at(0);
    CHECK(record->stdin_data == "input");
    CHECK(record->stdin_closed);
    CHECK(record->disowned); // it ended by itself
    CHECK_FALSE(record->killed);
    CHECK(record->released == std::chrono::milliseconds(1000));
}

TEST_CASE("run_once stops a program at its time limit and says so", "[process]") {
    fv::FakeProcessLauncher launcher;
    fv::FakeProcessScript script;
    script.exit_code = std::nullopt;
    script.stdout_bytes = "partial";
    launcher.push(script);
    const auto result = run_once({"tool.exe"}, "", 0.01, {}, {}, {}, &launcher);
    CHECK(result.error == "Timed out after 0.05 s"); // the floor
    CHECK(result.timed_out);
    CHECK_FALSE(result.exit_code);
    CHECK(result.output == "partial");
    const auto record = launcher.records().at(0);
    CHECK(record->killed);
    CHECK_FALSE(record->disowned);
    CHECK(record->released == std::chrono::milliseconds(2000));
}

TEST_CASE("run_once takes a time limit too long for the clock as none", "[process]") {
    // Python's monotonic() + 1e300 never comes; a deadline in nanoseconds would overflow.
    for (const double limit : {1e300, std::numeric_limits<double>::infinity()}) {
        fv::FakeProcessLauncher launcher;
        fv::FakeProcessScript script;
        script.runs_for = std::chrono::milliseconds(120);
        script.exit_code = 7;
        launcher.push(script);
        const auto result = run_once({"tool.exe"}, "", limit, {}, {}, {}, &launcher);
        CHECK(result.error.empty());
        CHECK_FALSE(result.timed_out);
        CHECK(result.exit_code == 7);
    }
}

TEST_CASE("run_once passes a start error on", "[process]") {
    fv::FakeProcessLauncher launcher;
    fv::FakeProcessScript script;
    script.start_error = "The program wasn't found.";
    launcher.push(script);
    const auto result = run_once({"missing.exe"}, "", 5, {}, {}, {}, &launcher);
    CHECK(result.error == "The program wasn't found.");
    CHECK_FALSE(result.exit_code);
}

TEST_CASE("timed out messages round as Python's :g does", "[process]") {
    CHECK(fv::timed_out_message(2) == "Timed out after 2 s");
    CHECK(fv::timed_out_message(1.5) == "Timed out after 1.5 s");
    CHECK(fv::timed_out_message(0.05) == "Timed out after 0.05 s");
    CHECK(fv::timed_out_message(2.0004) == "Timed out after 2 s");
    CHECK(fv::timed_out_message(2.0005) == "Timed out after 2.001 s"); // the double just above
    CHECK(fv::timed_out_message(30) == "Timed out after 30 s");
    CHECK(fv::timed_out_message(1234567) == "Timed out after 1.23457e+06 s");
}

#ifdef _WIN32

// -- tests/test_automation_hooks_programs.py ---------------------------------------------------

// from
// tests/test_automation_hooks_programs.py::test_run_once_passes_stdin_and_returns_stdout_and_exit_code
TEST_CASE("run_once passes stdin and returns stdout and exit code",
          "[test_automation_hooks_programs][platform]") {
    const auto result = run_once(child_argv({"upper", "exit", "3"}), "h\xC3\xA9llo", 20);
    CHECK(result.exit_code == 3);
    CHECK(result.output == "H\xC3\x89LLO");
    CHECK(result.error.empty());
}

// from
// tests/test_automation_hooks_programs.py::test_run_once_passes_arguments_literally_without_a_shell
TEST_CASE("run_once passes arguments literally without a shell",
          "[test_automation_hooks_programs][platform]") {
    const std::string tricky = "a & echo pwned | \"quoted\" $HOME %PATH% ; rm -rf /";
    const auto result = run_once(child_argv({"args", tricky, ""}), "", 20);
    CHECK(result.exit_code == 0);
    CHECK(result.error.empty());
    CHECK(fv::Json::parse(result.output) == fv::Json::array({tricky, ""}));
}

// from
// tests/test_automation_hooks_programs.py::test_run_once_starts_its_own_windowless_process_group
// The spec's flags are checked through the fake launcher above; here the program finds itself in
// the job that owns its tree.
TEST_CASE("run_once starts its own windowless process group",
          "[test_automation_hooks_programs][platform]") {
    const auto result = run_once(child_argv({"in-job"}), "", 20);
    CHECK(result.exit_code == 0);
    CHECK(result.error.empty());
    CHECK(result.output == "1");
}

// from
// tests/test_automation_hooks_programs.py::test_run_once_keeps_at_most_the_stdout_limit_and_discards_stderr
// OpenWhisper lowered the limit to 1,000 bytes for the test; the limit here is a constant, so
// the program writes past the real one.
TEST_CASE("run_once keeps at most the stdout limit and discards stderr",
          "[test_automation_hooks_programs][platform]") {
    const auto result = run_once(
        child_argv({"emit-err", "300000", "emit", std::to_string(fv::MAX_STDOUT_BYTES + 300000)}),
        "", 20);
    CHECK(result.exit_code == 0);
    CHECK(result.error.empty());
    CHECK(result.output == std::string(fv::MAX_STDOUT_BYTES, 'x'));
}

// from
// tests/test_automation_hooks_programs.py::test_run_once_survives_a_program_that_ignores_a_large_stdin
TEST_CASE("run_once survives a program that ignores a large stdin",
          "[test_automation_hooks_programs][platform]") {
    const auto started = std::chrono::steady_clock::now();
    const auto result =
        run_once(child_argv({"ignore-stdin"}), std::string(4 * 1024 * 1024, 'x'), 20);
    CHECK(result.exit_code == 0);
    CHECK(result.error.empty());
    CHECK(std::chrono::steady_clock::now() - started < std::chrono::seconds(10));
}

// from tests/test_automation_hooks_programs.py::test_run_once_explains_a_missing_program
TEST_CASE("run_once explains a missing program", "[test_automation_hooks_programs][platform]") {
    fv::testkit::TempDataRoot root;
    const auto missing = run_once({fv::path_to_utf8(root.path() / "nope.exe")}, "", 5);
    CHECK_FALSE(missing.exit_code);
    CHECK(missing.output.empty());
    CHECK(missing.error == "The program wasn't found.");
    CHECK(run_once({}, "", 5).error == "Choose the program to run.");
    CHECK(run_once({fv::process_test::child()}, "", 5, {}, root.path() / "gone").error ==
          "The working folder doesn't exist.");
}

// from
// tests/test_automation_hooks_programs.py::test_run_once_runs_in_the_home_folder_unless_told_otherwise
TEST_CASE("run_once runs in the home folder unless told otherwise",
          "[test_automation_hooks_programs][platform]") {
    fv::testkit::TempDataRoot root;
    const auto home = root.path() / "home";
    const auto elsewhere = root.path() / "elsewhere";
    std::filesystem::create_directories(home);
    std::filesystem::create_directories(elsewhere);
    // The home folder is read from USERPROFILE on Windows and from HOME elsewhere.
    fv::process_test::ScopedEnv profile(fv::ProcessEnv::kWindows ? "USERPROFILE" : "HOME",
                                        fv::path_to_utf8(home));
    const auto show = child_argv({"print-cwd"});
    auto result = run_once(show, "", 20);
    CHECK(std::filesystem::equivalent(fv::path_from_utf8(trimmed(result.output)), home));
    result = run_once(show, "", 20, {}, elsewhere);
    CHECK(std::filesystem::equivalent(fv::path_from_utf8(trimmed(result.output)), elsewhere));
}

// from
// tests/test_automation_hooks_programs.py::test_run_once_strips_openwhispers_secrets_from_the_environment
TEST_CASE("run_once strips the app's secrets from the environment",
          "[test_automation_hooks_programs][platform]") {
    fv::testkit::TempDataRoot root;
    const std::string sidecar = std::string(fv::identity().env_prefix) + "SIDECAR_TOKEN";
    fv::process_test::ScopedEnv key("OPENAI_API_KEY", "from-dotenv");
    fv::process_test::ScopedEnv token(sidecar, "internal");
    fv::process_test::ScopedEnv own("MY_OWN_SETTING", "kept");
    fv::process_test::write_file(root.path() / ".env", "OPENAI_API_KEY=from-dotenv\n");
    auto& process_env = fv::ProcessEnv::instance();
    process_env.forget();
    process_env.adopt_dotenv(fv::DotEnv(root.path() / ".env"));
    const auto result = run_once(
        child_argv({"env", "OPENAI_API_KEY", "env", sidecar, "env", "MY_OWN_SETTING"}), "", 20);
    process_env.forget();
    CHECK(result.exit_code == 0);
    CHECK(result.output == "<unset>\n<unset>\nkept\n");
}

// from
// tests/test_automation_hooks_programs.py::test_a_timeout_stops_the_program_and_everything_it_started
TEST_CASE("a timeout stops the program and everything it started",
          "[test_automation_hooks_programs][platform]") {
    fv::testkit::TempDataRoot root;
    const auto pid_file = root.path() / "child.pid";
    fv::process_test::PidGuard guard;
    const auto started = std::chrono::steady_clock::now();
    const auto result = run_once(child_argv({"spawn", fv::path_to_utf8(pid_file), "inherit",
                                             "inherit", "sleep", "60", ";;", "sleep", "60"}),
                                 "", 2);
    const auto elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - started);
    const auto grandchild = fv::process_test::read_pid(pid_file);
    REQUIRE(grandchild);
    guard.add(*grandchild);
    CHECK_FALSE(result.exit_code);
    CHECK(result.output.empty());
    CHECK(result.error == "Timed out after 2 s");
    CHECK(result.timed_out);
    CHECK(elapsed.count() >= 1.9);
    CHECK(elapsed.count() < 6);
    CHECK(
        wait_until([&] { return !fv::process_test::alive(*grandchild); }, std::chrono::seconds(5)));
}

// -- tests/test_automation_fix_actions_programs.py ---------------------------------------------

namespace {

struct Launched {
    fv::RunOnceResult result;
    double elapsed_s;
    std::filesystem::path marker;
    std::uint32_t helper;
};

// The program starts a helper that writes its pid, then a marker after `delay` seconds, and
// exits at once after printing "launched".
Launched launch(const std::filesystem::path& folder, bool holds_output, double delay = 2.5) {
    const auto marker = folder / "marker.txt";
    const auto pid_file = folder / "helper.pid";
    const auto started = std::chrono::steady_clock::now();
    auto result =
        run_once(child_argv({"spawn", fv::path_to_utf8(pid_file), holds_output ? "inherit" : "null",
                             "inherit", "sleep", std::to_string(delay), "write-file",
                             fv::path_to_utf8(marker), "ok", ";;", "print", "launched"}),
                 "", 30);
    const auto elapsed =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
    const auto helper = fv::process_test::read_pid(pid_file);
    return {std::move(result), elapsed, marker, helper.value_or(0)};
}

} // namespace

// from
// tests/test_automation_fix_actions_programs.py::test_an_app_the_program_opened_outlives_its_clean_exit
TEST_CASE("an app the program opened outlives its clean exit",
          "[test_automation_fix_actions_programs][platform]") {
    fv::testkit::TempDataRoot root;
    fv::process_test::PidGuard guard;
    const auto launched = launch(root.path(), false);
    REQUIRE(launched.helper != 0);
    guard.add(launched.helper);
    CHECK(launched.result.exit_code == 0);
    CHECK(trimmed(launched.result.output) == "launched");
    CHECK(launched.result.error.empty());
    CHECK(launched.elapsed_s < fv::OUTPUT_GRACE_S + 3);
    // The helper finishes its own work after run_once returned.
    CHECK(wait_until([&] { return std::filesystem::exists(launched.marker); }));
}

// from
// tests/test_automation_fix_actions_programs.py::test_a_helper_holding_the_output_open_survives_too
TEST_CASE("a helper holding the output open survives too",
          "[test_automation_fix_actions_programs][platform]") {
    // It inherited stdout, so run_once stops waiting for the output after the grace.
    fv::testkit::TempDataRoot root;
    fv::process_test::PidGuard guard;
    const auto launched = launch(root.path(), true);
    REQUIRE(launched.helper != 0);
    guard.add(launched.helper);
    CHECK(launched.result.exit_code == 0);
    CHECK(trimmed(launched.result.output) == "launched");
    CHECK(launched.result.error.empty());
    CHECK(launched.elapsed_s < fv::OUTPUT_GRACE_S + 3);
    CHECK(fv::process_test::alive(launched.helper));
    CHECK(wait_until([&] { return std::filesystem::exists(launched.marker); }));
}

// from
// tests/test_automation_fix_actions_programs.py::test_cancel_stops_the_program_and_everything_it_started
TEST_CASE("cancel stops the program and everything it started",
          "[test_automation_fix_actions_programs][platform]") {
    fv::testkit::TempDataRoot root;
    const auto pid_file = root.path() / "child.pid";
    fv::process_test::PidGuard guard;
    // "started" is in the pipe before the pid file appears, so a cancel sent as soon as the test
    // sees the file can't beat the print.
    fv::CancelSource cancel;
    auto running = std::async(std::launch::async, [&] {
        return run_once(child_argv({"print", "started", "spawn", fv::path_to_utf8(pid_file),
                                    "inherit", "inherit", "sleep", "60", ";;", "sleep", "60"}),
                        "", 60, cancel.token());
    });
    const auto grandchild = fv::process_test::read_pid(pid_file);
    REQUIRE(grandchild);
    guard.add(*grandchild);
    const auto canceled_at = std::chrono::steady_clock::now();
    cancel.cancel();
    REQUIRE(running.wait_for(std::chrono::seconds(10)) == std::future_status::ready);
    CHECK(std::chrono::steady_clock::now() - canceled_at < std::chrono::seconds(5));
    const auto result = running.get();
    CHECK_FALSE(result.exit_code);
    CHECK(result.error == "Canceled");
    CHECK(result.canceled);
    CHECK(trimmed(result.output) == "started");
    CHECK(
        wait_until([&] { return !fv::process_test::alive(*grandchild); }, std::chrono::seconds(5)));
}

// from tests/test_automation_fix_actions_programs.py::test_a_cancel_before_the_start_runs_nothing
TEST_CASE("a cancel before the start runs nothing",
          "[test_automation_fix_actions_programs][platform]") {
    fv::testkit::TempDataRoot root;
    const auto marker = root.path() / "ran.txt";
    fv::CancelSource cancel;
    cancel.cancel();
    const auto result =
        run_once(child_argv({"write-file", fv::path_to_utf8(marker), "x"}), "", 20, cancel.token());
    CHECK_FALSE(result.exit_code);
    CHECK(result.output.empty());
    CHECK(result.error == "Canceled");
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    CHECK_FALSE(std::filesystem::exists(marker));
}

// -- tests/test_automation_r2c_programs.py -------------------------------------------------------

// from
// tests/test_automation_r2c_programs.py::test_a_clean_run_lets_go_of_the_output_a_helper_still_holds
// OpenWhisper also checks that no thread of the run is left; run_once joins its pipe threads
// before it returns, so returning within the grace shows it.
TEST_CASE("a clean run lets go of the output a helper still holds",
          "[test_automation_r2c_programs][platform]") {
    fv::testkit::TempDataRoot root;
    const auto marker = root.path() / "closed.txt";
    const auto pid_file = root.path() / "helper.pid";
    fv::process_test::PidGuard guard;
    const auto started = std::chrono::steady_clock::now();
    const auto result =
        run_once(child_argv({"spawn", fv::path_to_utf8(pid_file), "inherit", "inherit", "ticks",
                             fv::path_to_utf8(marker), ";;", "print", "launched"}),
                 "", 30);
    const auto elapsed = std::chrono::steady_clock::now() - started;
    const auto helper = fv::process_test::read_pid(pid_file);
    REQUIRE(helper);
    guard.add(*helper);
    CHECK(result.exit_code == 0);
    CHECK(result.error.empty());
    CHECK(result.output.find("launched") != std::string::npos);
    CHECK(elapsed < std::chrono::duration<double>(fv::OUTPUT_GRACE_S + 3));
    // The pipe is closed: the helper's next write fails, and it was still running to notice
    // (nothing killed it).
    CHECK(wait_until(
        [&] {
            return std::filesystem::exists(marker) &&
                   fv::process_test::read_file(marker) == "closed";
        },
        std::chrono::seconds(10)));
}

// from tests/test_automation_r2c_programs.py::test_output_is_still_read_in_full_and_quickly
TEST_CASE("output is still read in full and quickly", "[test_automation_r2c_programs][platform]") {
    const auto result = run_once(child_argv({"emit", std::to_string(1024 * 1024)}), "", 30);
    CHECK(result.exit_code == 0);
    CHECK(result.error.empty());
    CHECK(result.output == std::string(1024 * 1024, 'x'));
}

// from tests/test_automation_r2c_programs.py::test_output_written_just_before_the_exit_is_kept
TEST_CASE("output written just before the exit is kept",
          "[test_automation_r2c_programs][platform]") {
    const auto result = run_once(child_argv({"sleep", "0.3", "write", "last words"}), "", 30);
    CHECK(result.exit_code == 0);
    CHECK(result.output == "last words");
    CHECK(result.error.empty());
}

// from tests/test_automation_r2c_programs.py::test_a_timed_out_run_lets_go_too
TEST_CASE("a timed out run lets go too", "[test_automation_r2c_programs][platform]") {
    const auto started = std::chrono::steady_clock::now();
    const auto result = run_once(child_argv({"print", "working", "sleep", "60"}), "", 1);
    CHECK_FALSE(result.exit_code);
    CHECK(result.error == "Timed out after 1 s");
    CHECK(trimmed(result.output) == "working");
    CHECK(std::chrono::steady_clock::now() - started < std::chrono::seconds(6));
}

// -- tests/test_automation_r3c_programs.py -------------------------------------------------------

namespace {

// More than any pipe buffer (4 KB on Windows).
const std::string kPayload(1024 * 1024, 'x');

struct HelperRun {
    fv::RunOnceResult result;
    std::uint32_t helper;
    std::filesystem::path marker;
};

// The program starts a helper that inherits its stdin (not its stdout) and exits at once
// without reading anything.
HelperRun run_with_helper(const std::filesystem::path& folder,
                          std::vector<std::string> helper_commands, const std::string& payload) {
    const auto pid_file = folder / "helper.pid";
    const auto marker = folder / "read.txt";
    std::vector<std::string> commands{"spawn", fv::path_to_utf8(pid_file), "null", "inherit"};
    for (auto& command : helper_commands) {
        commands.push_back(command == "MARKER" ? fv::path_to_utf8(marker) : command);
    }
    commands.push_back(";;");
    auto result = run_once(child_argv(commands), payload, 30);
    return {std::move(result), fv::process_test::read_pid(pid_file).value_or(0), marker};
}

} // namespace

// from
// tests/test_automation_r3c_programs.py::test_a_clean_run_lets_go_of_the_input_a_helper_holds_without_reading
TEST_CASE("a clean run lets go of the input a helper holds without reading",
          "[test_automation_r3c_programs][platform]") {
    fv::testkit::TempDataRoot root;
    fv::process_test::PidGuard guard;
    const auto started = std::chrono::steady_clock::now();
    const auto run = run_with_helper(root.path(), {"sleep", "30"}, kPayload);
    REQUIRE(run.helper != 0);
    guard.add(run.helper);
    CHECK(run.result.exit_code == 0);
    CHECK(run.result.error.empty());
    // The writer stops instead of waiting for the helper to read or exit...
    CHECK(std::chrono::steady_clock::now() - started < std::chrono::seconds(5));
    // ...and nothing stopped the helper.
    CHECK(fv::process_test::alive(run.helper));
}

// from tests/test_automation_r3c_programs.py::test_a_helper_still_gets_the_input_it_reads_in_time
// OpenWhisper lengthened the grace to 15 s because Python starts slowly; fv_process_child reads
// its stdin within the real one.
TEST_CASE("a helper still gets the input it reads in time",
          "[test_automation_r3c_programs][platform]") {
    // A program may hand its stdin on and exit (cmd's start /b); what the helper reads before
    // the run lets go still reaches it in full.
    fv::testkit::TempDataRoot root;
    fv::process_test::PidGuard guard;
    const std::string payload(200'000, 'y');
    const auto run = run_with_helper(root.path(), {"read-to", "MARKER"}, payload);
    REQUIRE(run.helper != 0);
    guard.add(run.helper);
    CHECK(run.result.exit_code == 0);
    CHECK(run.result.error.empty());
    CHECK(wait_until([&] {
        return std::filesystem::exists(run.marker) &&
               fv::process_test::read_file(run.marker) == std::to_string(payload.size());
    }));
}

// from tests/test_automation_r3c_programs.py::test_a_program_that_reads_its_input_gets_all_of_it
TEST_CASE("a program that reads its input gets all of it",
          "[test_automation_r3c_programs][platform]") {
    const auto result = run_once(child_argv({"count-stdin"}), kPayload, 30);
    CHECK(result.exit_code == 0);
    CHECK(result.output == std::to_string(kPayload.size()));
    CHECK(result.error.empty());
}

// -- acceptance ---------------------------------------------------------------------------

TEST_CASE("a clean exit with a helper holding stdout returns within the grace",
          "[process][platform]") {
    fv::testkit::TempDataRoot root;
    const auto marker = root.path() / "closed.txt";
    const auto pid_file = root.path() / "helper.pid";
    fv::process_test::PidGuard guard;
    std::chrono::steady_clock::time_point returned_at;
    auto running = std::async(std::launch::async, [&] {
        auto done =
            run_once(child_argv({"print", "before the exit", "spawn", fv::path_to_utf8(pid_file),
                                 "inherit", "inherit", "ticks", fv::path_to_utf8(marker), ";;"}),
                     "", 30);
        returned_at = std::chrono::steady_clock::now();
        return done;
    });
    // The program writes the helper's pid as its last act before it exits: the grace runs from
    // about then, however long the two processes took to start on a loaded machine.
    const auto helper = fv::process_test::read_pid(pid_file);
    const auto exiting_at = std::chrono::steady_clock::now();
    REQUIRE(helper);
    guard.add(*helper);
    REQUIRE(running.wait_for(std::chrono::seconds(60)) == std::future_status::ready);
    const auto result = running.get();
    CHECK(result.exit_code == 0);
    CHECK(result.output.starts_with("before the exit\n"));
    // OUTPUT_GRACE_S after the exit, not the 30 s timeout the helper's stdout would hold it to.
    CHECK(std::chrono::duration<double>(returned_at - exiting_at).count() <
          fv::OUTPUT_GRACE_S + 0.5);
    // Nothing writes once the helper noticed: no process is left behind writing.
    CHECK(wait_until([&] { return std::filesystem::exists(marker); }, std::chrono::seconds(10)));
    fv::process_test::kill_pid(*helper);
    CHECK(fv::process_test::stops_changing(marker));
}

#endif // _WIN32
