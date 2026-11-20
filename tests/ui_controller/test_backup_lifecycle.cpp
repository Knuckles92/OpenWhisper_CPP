// The backup and restore side of the application controller: the reservation and its gates, the
// worker's pauses, the restart a staged restore asks for, and the coordinator the controller
// provides and starts. Ported from tests/test_application_controller.py (4 functions; the
// cleanup-rule variant waits for M4's rule dictation) over the controller's feature graph.
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <filesystem>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

#include <QWidget>

#include "backup/coordinator.h"
#include "controller_support.h"
#include "engine/version.h"
#include "ui_controller/backup_lifecycle.h"

using fvtest::ControllerRig;
using fvtest::waitUntil;
using namespace std::chrono_literals;

namespace {

fv::BackupLifecycle& lifecycle(ControllerRig& rig) {
    return rig.context().service<fv::BackupLifecycle>();
}

bool contains(const std::optional<std::string>& text, const std::string& part) {
    return text.has_value() && text->find(part) != std::string::npos;
}

} // namespace

// from
// tests/test_application_controller.py::TestApplicationController::test_backup_reservation_blocks_capture_jobs_and_meetings
// (Meeting Mode and transcribe_clip arrive with M7; the gates they read are the same activity)
TEST_CASE("backup reservation blocks capture jobs and meetings",
          "[test_application_controller][widget]") {
    ControllerRig rig;
    rig.activity().set_startup_ready(true);
    QWidget* central = rig.window().centralWidget();
    REQUIRE(central != nullptr);
    central->setEnabled(true);

    CHECK_FALSE(lifecycle(rig).acquire().has_value());
    CHECK_FALSE(central->isEnabled());
    CHECK(rig.activity().snapshot().backup_in_progress);
    CHECK_FALSE(rig.runtime().start_recording());
    CHECK_FALSE(rig.runtime().claim_job());
    CHECK_FALSE(rig.app().start_recording("ui"));
    CHECK(rig.hasStatus("Wait for backup or restore preparation to finish"));

    lifecycle(rig).release();
    CHECK(central->isEnabled());
    CHECK_FALSE(rig.activity().snapshot().backup_in_progress);
    CHECK(rig.runtime().claim_job());
    rig.runtime().finish_job();
}

// from
// tests/test_application_controller.py::TestApplicationController::test_backup_waits_for_startup_recovery_and_active_work
TEST_CASE("backup waits for startup recovery and active work",
          "[test_application_controller][widget]") {
    ControllerRig rig;
    fv::BackupLifecycle& backup = lifecycle(rig);
    CHECK(contains(backup.acquire(), "starting"));
    rig.activity().set_startup_ready(true);
    rig.activity().set_recovery_running(true);
    CHECK(contains(backup.acquire(), "recovery"));
    rig.activity().set_recovery_running(false);
    rig.activity().set_recording(true);
    CHECK(contains(backup.acquire(), "recording"));
    rig.activity().set_recording(false);
    rig.activity().set_transcribing(true);
    CHECK(contains(backup.acquire(), "transcription"));
    rig.activity().set_transcribing(false);
    rig.activity().set_meeting_finishing(true);
    CHECK(contains(backup.acquire(), "meeting"));
    CHECK_FALSE(rig.activity().snapshot().backup_in_progress);
    CHECK(rig.window().centralWidget()->isEnabled());
}

// from
// tests/test_application_controller.py::TestApplicationController::test_backup_restore_only_requests_restart_after_success
TEST_CASE("backup restore only requests restart after success",
          "[test_application_controller][widget]") {
    ControllerRig rig;
    fv::backup::BackupCoordinator& coordinator =
        rig.context().service<fv::backup::BackupCoordinator>();
    CHECK(&coordinator == &lifecycle(rig).coordinator());

    coordinator.restore_staged.emit("", "Failed validation");
    CHECK_FALSE(rig.calls().has("exit:73"));
    coordinator.restore_staged.emit("test.owbackup", "");
    CHECK(rig.calls().has("exit:73"));
}

// from
// tests/test_application_controller.py::TestApplicationController::test_backup_pauses_client_sync_and_host_requests_and_releases_both
// (the record sync and the host register their pauses in M9; recording ones stand for them)
TEST_CASE("backup pauses client sync and host requests and releases both",
          "[test_application_controller][widget]") {
    ControllerRig rig;
    std::vector<std::string> events;
    const auto pause = [&events](std::string name) {
        return [&events, name](const std::function<void()>& body) {
            events.push_back("enter " + name);
            try {
                body();
            } catch (...) {
                events.push_back("leave " + name);
                throw;
            }
            events.push_back("leave " + name);
        };
    };
    lifecycle(rig).addWorkerContext(pause("client"));
    lifecycle(rig).addWorkerContext(pause("host"));

    CHECK_THROWS_WITH(lifecycle(rig).runInWorkerContext([&events] {
        events.push_back("backup");
        throw std::runtime_error("disk full");
    }),
                      "disk full");
    CHECK(events == std::vector<std::string>{"enter client", "enter host", "backup", "leave host",
                                             "leave client"});
}

TEST_CASE("Quit ends the reservation without giving the window back", "[widget]") {
    ControllerRig rig;
    rig.activity().set_startup_ready(true);
    QWidget* central = rig.window().centralWidget();
    central->setEnabled(true);
    REQUIRE_FALSE(lifecycle(rig).acquire().has_value());
    lifecycle(rig).shutdown();
    lifecycle(rig).release();
    CHECK_FALSE(central->isEnabled());
    CHECK_FALSE(rig.activity().snapshot().backup_in_progress);
    // A shutting-down app refuses a new reservation and restarts for nothing.
    CHECK(contains(lifecycle(rig).acquire(), "starting"));
    lifecycle(rig).onRestoreStaged("stage", "");
    CHECK_FALSE(rig.calls().has("exit:73"));
}

// The manifest names the build that made it (open item: BackupCoordinator gets kVersion), the
// reservation holds while the worker writes, and the window comes back after.
TEST_CASE("a backup through the provided coordinator records this build's version", "[widget]") {
    ControllerRig rig;
    REQUIRE(rig.startEngine());
    REQUIRE(waitUntil([&rig] { return !rig.activity().snapshot().recovery_running; }));
    auto& coordinator = rig.context().service<fv::backup::BackupCoordinator>();
    std::optional<fv::backup::BackupInfo> made;
    std::string error = "pending";
    std::vector<bool> busy;
    auto finished = coordinator.backup_finished.connect(
        rig.uiExecutor(), [&](std::optional<fv::backup::BackupInfo> info, std::string failure) {
            made = std::move(info);
            error = std::move(failure);
        });
    auto busyLink = coordinator.busy_changed.connect(rig.uiExecutor(), [&busy, &rig](bool value) {
        busy.push_back(value);
        if (value) {
            CHECK_FALSE(rig.window().centralWidget()->isEnabled());
        }
    });
    const std::filesystem::path archive =
        rig.paths().data_root() / "out" / "FioriVoce-backup.fvbackup";
    std::filesystem::create_directories(archive.parent_path());

    coordinator.create_backup(archive);

    REQUIRE(waitUntil([&error] { return error != "pending"; }, 60s));
    CHECK(error.empty());
    REQUIRE(made.has_value());
    CHECK(made->app_version == fv::kVersion);
    CHECK(std::filesystem::exists(archive));
    CHECK(rig.window().centralWidget()->isEnabled());
    CHECK_FALSE(rig.activity().snapshot().backup_in_progress);
    CHECK_FALSE(busy.empty());
}
