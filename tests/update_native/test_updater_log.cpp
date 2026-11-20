// The updater's own log (update_native/updater_log.h): OpenWhisper's setup_updater_logging and
// updater_log_path (services/app_update_apply.py), installed by run_updater before the first
// transaction step. Audit A16-47.

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "base/atomic_file.h"
#include "base/identity.h"
#include "base/log.h"
#include "base/testing/map_env_reader.h"
#include "base/utf8.h"
#include "testkit/testkit.h"
#include "transaction_support.h"
#include "update_native/transaction_runner.h"
#include "update_native/updater_cli.h"
#include "update_native/updater_log.h"

namespace fs = std::filesystem;
namespace u = fv::update_native;
using u::test::Fixture;
using u::test::kId;

namespace {

std::string read_text(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

// A transaction command line whose run is `run`, with the box and the streams recorded.
struct Run {
    std::vector<std::string> boxes;
    std::string errors;
    u::UpdaterActions actions;
    explicit Run(const fs::path& log_file) {
        actions.log_file = log_file;
        actions.message_box = [this](std::string_view text) {
            boxes.emplace_back(text);
        };
        actions.out = [](std::string_view) {
        };
        actions.err = [this](std::string_view text) {
            errors += text;
        };
    }
    int operator()(std::vector<std::string> args) { return u::run_updater(args, actions); }
};

} // namespace

TEST_CASE("the updater log lives beside the application's log, outside the data override",
          "[update_native]") {
    fv::testkit::TempDataRoot temp;
    const auto base = fv::path_to_utf8(temp.path() / "local");
    const fv::MapEnvReader env{{"LOCALAPPDATA", base},
                               {"XDG_DATA_HOME", base},
                               {"HOME", base},
                               {"FIORIVOCE_DATA_DIR", fv::path_to_utf8(temp.path() / "moved")}};
    CHECK(u::updater_log_path(env) ==
          temp.path() / "local" / fv::path_from_utf8(fv::identity().data_dir_name) / "updater.log");
    CHECK(u::kUpdaterLogMaxBytes == 512 * 1024);
    CHECK(u::kUpdaterLogBackupCount == 1);
}

// from tests/test_updater_helper.py (setup_updater_logging before main's first step); A16-47
TEST_CASE("a failed update leaves its reason in updater.log", "[update_native]") {
    Fixture f;
    f.platform.health_result = false;
    const fs::path log_file = f.temp.path() / "local" / "updater.log";
    Run run(log_file);
    run.actions.run_transaction = [&f](std::string_view, bool, std::uint32_t, std::uint64_t) {
        // A DEBUG line stays out: the helper logs at INFO, as Python's root logger did.
        fv::log::Logger("services.app_update_apply").debug("a debug line nobody reads");
        return u::run_transaction(f.context, f.journal, f.platform, false, {});
    };

    CHECK(run({"--apply", "--transaction-id", std::string(kId)}) == 1);

    // The folder is made; the lines are written out before run_updater returns.
    REQUIRE(fs::is_regular_file(log_file));
    const std::string text = read_text(log_file);
    INFO(text);
    CHECK(text.find(" - ERROR - services.app_update_apply - The update transaction failed in "
                    "state ") != std::string::npos);
    CHECK(text.find("The new version started but did not become ready.") != std::string::npos);
    CHECK(text.find("a debug line nobody reads") == std::string::npos);
    // The box is still shown: the log is a diagnosis, not a substitute.
    CHECK(run.boxes ==
          std::vector<std::string>{"The new version started but did not become ready."});
    // The sink is gone with the run: later lines do not reach the file.
    fv::log::Logger("services.app_update_apply").error("after the run");
    CHECK(read_text(log_file).find("after the run") == std::string::npos);
}

TEST_CASE("updater.log rolls over at 512 KiB and keeps one backup", "[update_native]") {
    fv::testkit::TempDataRoot temp;
    const fs::path log_file = temp.path() / "updater.log";
    Run run(log_file);
    run.actions.run_transaction = [](std::string_view, bool, std::uint32_t, std::uint64_t) {
        const std::string filler(1000, 'x');
        // About 1.5 MiB in all: enough for two rollovers.
        for (int i = 0; i < 1600; ++i) {
            fv::log::Logger("services.app_update_apply").info("line {} {}", i, filler);
        }
        return u::TransactionState::Healthy;
    };
    CHECK(run({"--apply", "--transaction-id", std::string(kId)}) == 0);
    REQUIRE(fs::is_regular_file(log_file));
    CHECK(fs::file_size(log_file) <= u::kUpdaterLogMaxBytes);
    REQUIRE(fs::is_regular_file(fs::path(log_file) += ".1"));
    CHECK(fs::file_size(fs::path(log_file) += ".1") <= u::kUpdaterLogMaxBytes);
    CHECK_FALSE(fs::exists(fs::path(log_file) += ".2"));
    CHECK(read_text(log_file).find("line 1599 ") != std::string::npos);
}

TEST_CASE("a log that cannot be opened does not stop the update", "[update_native]") {
    fv::testkit::TempDataRoot temp;
    // The log's folder is a file: neither the folder nor the log can be made.
    fv::write_file_atomic(temp.path() / "blocked", "a file");
    Run run(temp.path() / "blocked" / "updater.log");
    bool ran = false;
    run.actions.run_transaction = [&ran](std::string_view, bool, std::uint32_t, std::uint64_t) {
        ran = true;
        return u::TransactionState::Healthy;
    };
    CHECK(run({"--apply", "--transaction-id", std::string(kId)}) == 0);
    CHECK(ran);
    CHECK(run.boxes.empty());
    CHECK_FALSE(u::UpdaterLog(temp.path() / "blocked" / "updater.log").active());
}

TEST_CASE("the packaging commands and --help open no updater log", "[update_native]") {
    fv::testkit::TempDataRoot temp;
    const fs::path log_file = temp.path() / "updater.log";
    Run run(log_file);
    CHECK(run({"--help"}) == 0);
    CHECK(run({"--verify-archive", fv::path_to_utf8(temp.path() / "missing.zip")}) == 1);
    CHECK_FALSE(fs::exists(log_file));
    // A command line it does not know is the installer's or the application's: logged.
    CHECK(run({"--transaction-id", std::string(kId)}) == 2);
    CHECK(read_text(log_file).find(
              " - WARNING - scripts.updater_helper - The updater was started without a command "
              "it knows (2 arguments).") != std::string::npos);
}
