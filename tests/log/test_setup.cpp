// install_app_logging, run in fv_log_crash_helper so the crash handler it installs and the
// facade it configures stay out of this process.

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <regex>
#include <string>

#include "base/identity.h"
#include "base/paths.h"
#include "base/testing/map_env_reader.h"
#include "base/utf8.h"
#include "log/crash_handler.h"
#include "log/failure_journal.h"
#include "log/metrics_capture.h"
#include "log_test_support.h"
#include "pyport/json.h"

namespace flog = fv::log;
using fv::Json;

TEST_CASE("after install_app_logging, lines read asctime - LEVEL - name - message and an ERROR "
          "with an exception appends one event without its text",
          "[log]") {
    fv::logtest::TempDir root;
    const auto exit_code = fv::logtest::run_child(
        FV_LOG_CRASH_HELPER, {"logging", fv::path_to_utf8(root.path())}, std::chrono::seconds(60));
    REQUIRE(exit_code);
    REQUIRE(*exit_code == 0u);

    fv::MapEnvReader env;
    env.set(std::string(fv::identity().env_prefix) + "DATA_DIR", fv::path_to_utf8(root.path()));
    fv::pin_per_user_folders(env, root.path() / "per-user");
    const auto paths = fv::Paths::resolve(fv::identity(), env);
    const auto crash_log = flog::CrashHandler::crash_log_file(paths, fv::identity());
    const auto log_file = root.path() / std::string(fv::identity().log_file);
    const auto lines = fv::logtest::read_lines(log_file);
    const std::string stamp = R"(\d{4}-\d\d-\d\d \d\d:\d\d:\d\d,\d{3} - )";
    // httpx's and httpcore's INFO lines are dropped (bootstrap.py setup_logging), their warnings
    // kept.
    REQUIRE(lines.size() == 4);
    CHECK(std::regex_match(lines[3],
                           std::regex(stamp + R"(WARNING - httpcore\.http11 - connection reset)")));
    CHECK(std::regex_search(lines[0], std::regex("^" + stamp)));
    CHECK(lines[0].ends_with(" - INFO - root - Crash handler enabled for crash diagnostics: " +
                             fv::path_to_utf8(crash_log)));
    CHECK(std::regex_match(lines[1], std::regex(stamp + R"(INFO - fv\.test - message)")));
    CHECK(std::regex_match(lines[2],
                           std::regex(stamp + R"(ERROR - fv\.test - private failure text)")));
#if defined(_WIN32)
    CHECK(fv::logtest::read_file(log_file).find("message\r\n") != std::string::npos);
#endif

    const std::string journal_text =
        fv::logtest::read_file(root.path() / std::string(flog::kJournalName));
    const Json journal = fv::py::loads(journal_text);
    REQUIRE(journal.size() == 1);
    CHECK(journal[0]["component"] == "application");
    CHECK(journal[0]["category"] == "TimeoutError");
    CHECK(journal[0]["line"].is_number_integer());
    CHECK(journal_text.find("private") == std::string::npos);

    const Json metrics =
        fv::py::loads(fv::logtest::read_file(root.path() / std::string(flog::kMetricsName)));
    CHECK(metrics == Json{{"shutdown_s", Json::array({0.25})}});

    CHECK(std::filesystem::exists(crash_log));
}

// logging.shutdown never waits on a stalled disk for long: the metrics get 100 ms and the queued
// lines the rest of one second, in total, not once per part.
TEST_CASE("releasing the logging with fv-log stalled takes about one second in total", "[log]") {
    fv::logtest::TempDir root;
    const auto exit_code = fv::logtest::run_child(
        FV_LOG_CRASH_HELPER, {"logging-stalled", fv::path_to_utf8(root.path())},
        std::chrono::seconds(60));
    REQUIRE(exit_code);
    CHECK(*exit_code == 0u);
}
