// fiorivoce-updater's command line (update_native/updater_cli.h), from OpenWhisper's
// tests/test_updater_helper.py and tests/test_frozen_updater.py. The program itself is only
// main(): these tests give run_updater what main() gives it.

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <string>
#include <vector>

#include "base/atomic_file.h"
#include "base/json.h"
#include "base/utf8.h"
#include "prepare_support.h"
#include "testkit/testkit.h"
#include "update/update_contract.h"
#include "update_native/archive.h"
#include "update_native/manifest.h"
#include "update_native/transaction_runner.h"
#include "update_native/updater_cli.h"

namespace fs = std::filesystem;
namespace u = fv::update_native;
using u::test::read_text;

namespace {
constexpr auto kId = "0123456789abcdef0123456789abcdef";

// The helper's surroundings, recorded.
struct Program {
    std::vector<std::string> calls;
    std::vector<std::string> boxes;
    std::string out, err;
    std::function<u::TransactionState()> outcome = [] {
        return u::TransactionState::Healthy;
    };
    u::UpdaterActions actions() {
        u::UpdaterActions result;
        result.run_transaction = [this](std::string_view id, bool recover, std::uint32_t pid,
                                        std::uint64_t time) {
            calls.push_back(std::string(recover ? "recover " : "apply ") + std::string(id) +
                            (pid ? " after " + std::to_string(pid) + "/" + std::to_string(time)
                                 : std::string()));
            return outcome();
        };
        result.leave_launch_directory = [this] {
            calls.emplace_back("left");
        };
        result.message_box = [this](std::string_view text) {
            boxes.emplace_back(text);
        };
        result.out = [this](std::string_view text) {
            out += text;
        };
        result.err = [this](std::string_view text) {
            err += text;
        };
        return result;
    }
    int run(std::vector<std::string> args) { return u::run_updater(args, actions()); }
};
} // namespace

// from tests/test_updater_helper.py::test_helper_requires_transaction_id
TEST_CASE("the helper started without a transaction does nothing and exits 2", "[update_native]") {
    Program p;
    CHECK(p.run({}) == 2);
    CHECK(p.run({"--apply"}) == 2);
    CHECK(p.run({"--apply", "--transaction-id"}) == 2);
    CHECK(p.run({"--recover", kId}) == 2);
    CHECK(p.calls.empty());
    // The usage goes to standard error. No box: build scripts run this program too.
    CHECK(p.err.find("fiorivoce-updater --apply --transaction-id <id>") != std::string::npos);
    CHECK(p.out.empty());
    CHECK(p.boxes.empty());
}

// from tests/test_updater_helper.py::test_helper_commits_prepared_journal and
// tests/test_updater_helper.py::test_helper_leaves_its_launch_directory_before_committing
TEST_CASE("the helper leaves its launch directory and then commits the journal",
          "[update_native]") {
    Program p;
    CHECK(p.run({"--apply", "--transaction-id", kId}) == 0);
    CHECK(p.calls == std::vector<std::string>{"left", std::string("apply ") + kId});
    CHECK(p.out == "Update transaction finished healthy.\n");
    CHECK(p.boxes.empty());
    CHECK(p.err.empty());
}

// from tests/test_updater_helper.py::test_helper_recover_path
TEST_CASE("the helper recovers the transaction it is given", "[update_native]") {
    Program p;
    p.outcome = [] {
        return u::TransactionState::RolledBack;
    };
    CHECK(p.run({"--recover", "--transaction-id", kId}) == 0);
    // The application that found the interrupted update names itself, to be waited for.
    CHECK(p.run({"--recover", "--transaction-id", kId, "--wait-for-launcher", "11", "22"}) == 0);
    CHECK(p.calls == std::vector<std::string>{"left", std::string("recover ") + kId, "left",
                                              std::string("recover ") + kId + " after 11/22"});
    CHECK(p.boxes.empty());
    // A launcher identity that is not two positive numbers runs nothing.
    Program bad;
    CHECK(bad.run({"--recover", "--transaction-id", kId, "--wait-for-launcher", "0", "22"}) == 1);
    CHECK(bad.run({"--recover", "--transaction-id", kId, "--wait-for-launcher", "x", "22"}) == 1);
    CHECK(bad.calls.empty());
}

// from tests/test_updater_helper.py::test_helper_reports_apply_error
TEST_CASE("the helper shows an update failure nobody else will show", "[update_native]") {
    Program p;
    p.outcome = []() -> u::TransactionState {
        throw u::UpdateError("missing journal");
    };
    CHECK(p.run({"--apply", "--transaction-id", kId}) == 1);
    CHECK(p.boxes == std::vector<std::string>{"missing journal"});
    CHECK(p.err == "fiorivoce-updater: missing journal\n");

    // Anything unexpected is worded as OpenWhisper's last-resort handler words it.
    Program odd;
    odd.outcome = []() -> u::TransactionState {
        throw std::runtime_error("disk full");
    };
    CHECK(odd.run({"--recover", "--transaction-id", kId}) == 1);
    CHECK(odd.boxes == std::vector<std::string>{"The update failed: disk full"});

    // After a rollback the previous version is running again and shows the reason itself.
    Program told;
    told.outcome = []() -> u::TransactionState {
        throw u::ReportedByApplication("The new version started but did not become ready.");
    };
    CHECK(told.run({"--apply", "--transaction-id", kId}) == 1);
    CHECK(told.boxes.empty());
    CHECK(told.err == "fiorivoce-updater: The new version started but did not become ready.\n");
}

TEST_CASE("the helper has no transaction or setup commands where the product has none",
          "[update_native]") {
    std::string err;
    u::UpdaterActions actions;
    actions.err = [&](std::string_view text) {
        err += text;
    };
    const std::vector<std::string> apply{"--apply", "--transaction-id", kId};
    const std::vector<std::string> setup{"--setup-action", "prepare", "--app-dir", "x"};
    CHECK(u::run_updater(apply, actions) == 2);
    CHECK(u::run_updater(setup, actions) == 2);
    CHECK(err.find("--apply") == std::string::npos);
    CHECK(err.find("--pack-archive") != std::string::npos);
}

TEST_CASE("the helper packs and verifies a release from its command line", "[update_native]") {
    fv::testkit::TempDataRoot temp;
    const auto tree = temp.path() / "stage";
    fs::create_directories(tree / "platforms");
    fv::write_file_atomic(tree / "fiorivoce.exe", "app");
    fv::write_file_atomic(tree / "fiorivoce-updater.exe", "helper");
    fv::write_file_atomic(tree / "platforms" / "qwindows.dll", "plugin");
    const auto zip = temp.path() / "release.zip";
    Program p;
    CHECK(p.run({"--write-manifest", fv::path_to_utf8(tree), "--version", "0.1.1"}) == 0);
    CHECK(fs::is_regular_file(tree / fv::update::manifest_name()));
    CHECK(p.run({"--pack-archive", fv::path_to_utf8(tree), "--version", "0.1.1", "--out",
                 fv::path_to_utf8(zip)}) == 0);
    CHECK(p.run({"--verify-archive", fv::path_to_utf8(zip), "--version", "0.1.1"}) == 0);
    CHECK(p.out.find(" 0.1.1 (3 files).\n") != std::string::npos);
    CHECK(p.err.empty());
    // A failure is a line on standard error and exit code 1, never a box.
    CHECK(p.run({"--verify-archive", fv::path_to_utf8(zip), "--version", "0.1.2"}) == 1);
    CHECK(p.err.starts_with("fiorivoce-updater: "));
    CHECK(p.boxes.empty());
    CHECK(p.run({"--help"}) == 0);
    CHECK(p.out.find("--verify-archive <zip> [--version <version>]") != std::string::npos);
}

// from tests/test_frozen_updater.py::test_frozen_setup_helper_replaces_and_rolls_back
TEST_CASE("the setup helper replaces an installation and rolls an incomplete one back",
          "[update_native]") {
    fv::testkit::TempDataRoot temp;
    const auto app = temp.path() / "installed";
    const auto helper = temp.path() / "tmp" / "fv-helper";
    const auto next = temp.path() / "new";
    for (const auto& dir : {app, helper, next})
        fs::create_directories(dir);
    fv::write_file_atomic(app / "fiorivoce.exe", "old");
    fv::write_file_atomic(app / "fiorivoce-updater.exe", "old helper");
    u::write_manifest(app, "0.1.0");
    fv::write_file_atomic(app / "unins000.exe", "uninstaller");
    for (const auto* name : {"fiorivoce.exe", "fiorivoce-updater.exe"})
        fv::write_file_atomic(next / name, std::string("new ") + name);
    u::write_manifest(next, "0.1.1");
    // Inno extracts the helper with the manifest of what it is about to install.
    fs::copy_file(next / fv::update::manifest_name(), helper / fv::update::manifest_name());

    int retired = 0;
    u::UpdaterActions actions;
    std::string err;
    actions.err = [&](std::string_view text) {
        err += text;
    };
    actions.setup_action = [&](std::string_view action, const fs::path& install) {
        u::SetupSession session;
        session.helper_dir = helper;
        session.registered = [&](const fs::path& target) {
            return target == app;
        };
        session.retire_transactions = [&] {
            ++retired;
        };
        return u::run_setup_action(action, install, session);
    };
    const auto run = [&](const char* action, const fs::path& target) {
        const std::vector<std::string> args{"--setup-action", action, "--app-dir",
                                            fv::path_to_utf8(target)};
        return u::run_updater(args, actions);
    };
    const auto install_new = [&] {
        // Inno's [InstallDelete] clears the managed names, then its [Files] land.
        for (const auto& entry : fs::directory_iterator(app))
            if (entry.path().extension() == ".dll" || entry.path().filename() == "fiorivoce.exe" ||
                entry.path().filename() == "fiorivoce-updater.exe" ||
                entry.path().filename() == fv::update::manifest_name())
                fs::remove(entry.path());
        for (const auto& entry : fs::directory_iterator(next))
            fs::copy_file(entry.path(), app / entry.path().filename());
    };

    REQUIRE(run("prepare", app) == 0);
    CHECK(retired == 1);
    fv::write_file_atomic(app / "fiorivoce.exe", "incomplete");
    REQUIRE(run("rollback", app) == 0);
    CHECK(read_text(app / "fiorivoce.exe") == "old");
    CHECK_FALSE(fs::exists(temp.path() / "installed.setup-backup"));

    fv::write_file_atomic(app / "retired.dll", "old dependency");
    REQUIRE(run("prepare", app) == 0);
    // The folder that was prepared is the only one that may be finished or rolled back.
    CHECK(run("finish", temp.path() / "elsewhere") == 1);
    CHECK(read_text(helper / "setup-error.txt") ==
          "The setup destination changed after preparation.");
    install_new();
    REQUIRE(run("finish", app) == 0);
    CHECK(read_text(app / "fiorivoce.exe") == "new fiorivoce.exe");
    CHECK_FALSE(fs::exists(app / "retired.dll"));
    CHECK(read_text(app / "unins000.exe") == "uninstaller");
    CHECK_FALSE(fs::exists(temp.path() / "installed.setup-backup"));

    // An unknown action is not a command; the helper never works on its own folder.
    CHECK(run("repair", app) == 2);
    CHECK(run("prepare", helper) == 1);
    CHECK(read_text(helper / "setup-error.txt") == "Setup must use its temporary helper.");
    CHECK(err.find("Setup must use its temporary helper.") != std::string::npos);
}
