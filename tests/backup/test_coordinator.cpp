// The backup coordinator (OpenWhisper's tests/test_backup_manager.py), on manual executors and a
// fake clock: the UI actor and the backup worker each run only when the test drains them.

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "backup/coordinator.h"
#include "backup/preferences.h"
#include "backup/testing/fake_hooks.h"
#include "backup/testing/sample_data.h"
#include "base/executor.h"
#include "base/identity.h"
#include "base/paths.h"
#include "base/testing/fake_clock.h"
#include "base/testing/manual_executor.h"
#include "base/testing/map_env_reader.h"
#include "base/utf8.h"
#include "pyport/json.h"
#include "testkit/testkit.h"

namespace fs = std::filesystem;
using Catch::Matchers::ContainsSubstring;
using fv::Json;
using fv::backup::BackupCoordinator;
using fv::backup::BackupInfo;
using fv::backup::kPreferencesFile;
using fv::backup::testing::FakeBackupService;
using fv::backup::testing::FakeHooks;
using fv::backup::testing::read_bytes;
using fv::backup::testing::write_bytes;

namespace {

constexpr double kDay = 86400;

std::chrono::system_clock::time_point at_seconds(double seconds) {
    return std::chrono::system_clock::time_point(
        std::chrono::duration_cast<std::chrono::system_clock::duration>(
            std::chrono::duration<double>(seconds)));
}

fv::Paths paths_for(const fs::path& data) {
    fv::MapEnvReader env{{"FIORIVOCE_DATA_DIR", fv::path_to_utf8(data)}};
    // Inside the data folder: never created, and never outside the test's folder.
    fv::pin_per_user_folders(env, data / ".per-user");
    return fv::Paths::resolve(fv::identity(), env);
}

// Path.resolve() of a folder that exists.
std::string resolved(const fs::path& path) {
    return fv::path_to_utf8(fs::weakly_canonical(fs::absolute(path)));
}

// The UI actor, the backup worker and the clock (time.time() starts at `now`).
struct Rig {
    explicit Rig(double now = kDay + 1) : clock(at_seconds(now)), ui(clock) {}

    std::unique_ptr<BackupCoordinator> make(const fs::path& data,
                                            BackupCoordinator::Hooks hooks = {}) {
        return std::make_unique<BackupCoordinator>(paths_for(data), fv::identity(),
                                                   std::move(hooks), ui, worker, clock, "0.1.0",
                                                   &service);
    }

    // Runs the worker and the UI actor until neither has anything queued.
    void drain() {
        while (worker.run_all() + ui.run_all() > 0) {
        }
    }

    double now() const {
        return std::chrono::duration<double>(clock.utc().time_since_epoch()).count();
    }

    fv::FakeClock clock;
    fv::ManualExecutor ui;
    fv::ManualExecutor worker;
    FakeBackupService service;
};

using Result = std::pair<std::optional<BackupInfo>, std::string>;

} // namespace

TEST_CASE("worker completion releases reservation after context exit", "[test_backup_manager]") {
    // from
    // tests/test_backup_manager.py::test_worker_completion_releases_reservation_after_context_exit
    fv::testkit::TempDataRoot temp;
    Rig rig;
    FakeHooks hooks;
    auto manager = rig.make(temp.path() / "data", hooks.hooks());
    auto busy = manager->busy_changed.connect(
        rig.ui, [&](bool value) { hooks.record(value ? "busy" : "idle"); });
    auto finished = manager->backup_finished.connect(
        rig.ui,
        [&](const std::optional<BackupInfo>&, const std::string&) { hooks.record("finished"); });
    const fs::path destination = temp.path() / "manual.fvbackup";

    manager->create_backup(destination, false);
    rig.ui.run_all();
    CHECK(manager->busy());
    CHECK(manager->operation() == "backup");
    CHECK(hooks.events() == std::vector<std::string>{"acquire", "busy"});

    rig.drain();
    CHECK(rig.service.calls() ==
          std::vector<FakeBackupService::Call>{{"backup", fv::path_to_utf8(destination), "false"}});
    CHECK(hooks.events() == std::vector<std::string>{"acquire", "busy", "context_enter",
                                                     "context_exit", "release", "idle",
                                                     "finished"});
    CHECK_FALSE(manager->busy());
    CHECK(manager->snapshot()["last_backup"]["path"] == fv::path_to_utf8(destination));
    manager->shutdown();
}

TEST_CASE("cancelled backup reports error and releases reservation", "[test_backup_manager]") {
    // from
    // tests/test_backup_manager.py::test_cancelled_backup_reports_error_and_releases_reservation
    // (the cancel arrives while the request waits for the worker, which the test drives)
    fv::testkit::TempDataRoot temp;
    Rig rig;
    int releases = 0;
    BackupCoordinator::Hooks hooks;
    hooks.acquire = [] {
        return std::optional<std::string>();
    };
    hooks.release = [&] {
        ++releases;
    };
    rig.service.on_create = [](const fs::path&, const fv::backup::CreateOptions& options) {
        REQUIRE(options.cancel.cancelled());
        throw fv::backup::BackupCancelled();
    };
    auto manager = rig.make(temp.path() / "data", std::move(hooks));
    std::vector<Result> results;
    auto connection = manager->backup_finished.connect(
        rig.ui, [&](const std::optional<BackupInfo>& info, const std::string& error) {
            results.emplace_back(info, error);
        });
    const fs::path destination = temp.path() / "cancelled.fvbackup";

    manager->create_backup(destination);
    manager->cancel();
    rig.drain();
    REQUIRE(results.size() == 1);
    CHECK_FALSE(results[0].first.has_value());
    CHECK(results[0].second == "Backup canceled");
    CHECK(releases == 1);
    CHECK_FALSE(fs::exists(destination));
    CHECK_FALSE(manager->busy());
    manager->shutdown();
}

TEST_CASE("schedule is off by default and persists explicit choice", "[test_backup_manager]") {
    // from
    // tests/test_backup_manager.py::test_schedule_is_off_by_default_and_persists_explicit_choice
    fv::testkit::TempDataRoot temp;
    const fs::path data = temp.path() / "data";
    const fs::path folder = temp.path() / "backups";
    fs::create_directories(folder);
    Rig rig(kDay + 1);
    {
        auto manager = rig.make(data);
        CHECK(manager->snapshot()["frequency"] == "off");
        CHECK(manager->snapshot()["next_backup_at"].is_null());
        manager->scheduled_tick();
        rig.drain();
        CHECK(rig.service.calls().empty());
        CHECK(manager->set_schedule("weekly", 3, folder).empty());
        CHECK(manager->snapshot()["frequency"] == "weekly");
        CHECK(fs::is_regular_file(data / kPreferencesFile));
        manager->shutdown();
    }
    auto reopened = rig.make(data);
    CHECK(reopened->snapshot()["frequency"] == "weekly");
    CHECK(reopened->snapshot()["retention_count"] == 3);
    CHECK(reopened->snapshot()["destination"] == resolved(folder));
    reopened->shutdown();
}

TEST_CASE("calendar due date follows saved automatic backup and retry", "[test_backup_manager]") {
    // from
    // tests/test_backup_manager.py::test_calendar_due_date_follows_saved_automatic_backup_and_retry
    fv::testkit::TempDataRoot temp;
    const fs::path data = temp.path() / "data";
    const fs::path folder = temp.path() / "backups";
    fs::create_directories(folder);
    Rig rig(kDay + 1);
    auto manager = rig.make(data);
    CHECK(manager->set_schedule("daily", 3, folder).empty());
    CHECK(manager->snapshot()["next_backup_at"] == rig.now());
    manager->scheduled_tick();
    rig.drain();
    REQUIRE(rig.service.calls().size() == 1);
    CHECK_FALSE(manager->busy());
    CHECK(manager->snapshot()["next_backup_at"] == rig.now() + kDay);

    manager->create_backup(temp.path() / "manual.fvbackup");
    rig.drain();
    CHECK(rig.service.calls().size() == 2);
    CHECK(manager->snapshot()["next_backup_at"] == rig.now() + kDay);
    manager->shutdown();
    manager = rig.make(data);
    CHECK(manager->snapshot()["next_backup_at"] == rig.now() + kDay);

    rig.clock.advance(std::chrono::hours(48));
    CHECK(manager->snapshot()["next_backup_at"] == rig.now());
    manager->set_retry_after(rig.now() + 600);
    CHECK(manager->snapshot()["next_backup_at"] == rig.now() + 600);
    CHECK(manager->set_schedule("off", 3, folder).empty());
    CHECK(manager->snapshot()["next_backup_at"].is_null());
    manager->shutdown();
}

TEST_CASE("missing or malformed schedule destination does not run", "[test_backup_manager]") {
    // from
    // tests/test_backup_manager.py::test_missing_or_malformed_schedule_destination_does_not_run
    fv::testkit::TempDataRoot temp;
    const fs::path data = temp.path() / "data";
    const fs::path folder = temp.path() / "backups";
    fs::create_directories(folder);
    Rig rig(kDay + 1);
    {
        auto manager = rig.make(data);
        CHECK(manager->set_schedule("daily", 2, folder).empty());
        fs::remove(folder);
        manager->scheduled_tick();
        rig.drain();
        CHECK(rig.service.calls().empty());
        CHECK_THAT(manager->snapshot()["last_error"].get<std::string>(),
                   ContainsSubstring("destination folder"));
        manager->shutdown();
    }
    Json preferences = fv::py::loads(read_bytes(data / kPreferencesFile));
    preferences["destination"] = "relative-folder";
    write_bytes(data / kPreferencesFile, fv::py::dumps(preferences));
    auto reopened = rig.make(data);
    reopened->scheduled_tick();
    rig.drain();
    CHECK(rig.service.calls().empty());
    CHECK_THAT(reopened->snapshot()["last_error"].get<std::string>(),
               ContainsSubstring("destination folder"));
    reopened->shutdown();
}

TEST_CASE("automatic retention removes only registered archives", "[test_backup_manager]") {
    // from tests/test_backup_manager.py::test_automatic_retention_removes_only_registered_archives
    fv::testkit::TempDataRoot temp;
    const fs::path data = temp.path() / "data";
    const fs::path folder = temp.path() / "backups";
    const fs::path manual = folder / "my-manual.fvbackup";
    write_bytes(manual, "manual");
    const fs::path unregistered_auto =
        folder / fv::path_from_utf8(fv::backup::auto_backup_name(
                     fv::identity(), at_seconds(1'790'812'800), "aaaaaaaaaaaa"));
    write_bytes(unregistered_auto, "unregistered");
    const fs::path unrelated = folder / "notes.txt";
    write_bytes(unrelated, "keep");
    Rig rig(kDay + 1);
    auto manager = rig.make(data);
    CHECK(manager->set_schedule("daily", 2, folder).empty());
    std::vector<fs::path> created;
    for (int round = 0; round < 3; ++round) {
        const auto before = rig.service.calls().size();
        manager->scheduled_tick();
        rig.drain();
        REQUIRE(rig.service.calls().size() > before);
        CHECK_FALSE(manager->busy());
        created.push_back(fv::path_from_utf8(rig.service.calls().back()[1]));
        rig.clock.advance(std::chrono::seconds(86401));
    }
    CHECK_FALSE(fs::exists(created[0]));
    CHECK(fs::exists(created[1]));
    CHECK(fs::exists(created[2]));
    CHECK(fs::exists(manual));
    CHECK(fs::exists(unregistered_auto));
    CHECK(fs::exists(unrelated));
    const Json saved = fv::py::loads(read_bytes(data / kPreferencesFile));
    CHECK(saved["scheduled_archives"] ==
          Json::array({fv::path_to_utf8(created[1]), fv::path_to_utf8(created[2])}));
    for (const auto& path : created) {
        CHECK(fv::backup::is_auto_backup_name(fv::path_to_utf8(path.filename()), fv::identity()));
        CHECK(path.parent_path() == fs::path(fv::path_from_utf8(resolved(folder))));
    }
    manager->shutdown();
}

TEST_CASE("inspection does not reserve but restore does", "[test_backup_manager]") {
    // from tests/test_backup_manager.py::test_inspection_does_not_reserve_but_restore_does
    fv::testkit::TempDataRoot temp;
    const fs::path source = temp.path() / "backup.fvbackup";
    write_bytes(source, "archive");
    Rig rig;
    FakeHooks hooks;
    auto manager = rig.make(temp.path() / "data", hooks.hooks());
    std::vector<Result> inspected;
    std::vector<std::pair<std::string, std::string>> staged;
    auto on_inspect = manager->inspection_finished.connect(
        rig.ui, [&](const std::optional<BackupInfo>& info, const std::string& error) {
            inspected.emplace_back(info, error);
        });
    auto on_staged = manager->restore_staged.connect(
        rig.ui, [&](const std::string& path, const std::string& error) {
            staged.emplace_back(path, error);
        });

    manager->inspect_backup(source);
    rig.drain();
    REQUIRE(inspected.size() == 1);
    CHECK(inspected[0].second.empty());
    CHECK(hooks.events().empty());
    manager->prepare_restore(source);
    rig.drain();
    CHECK(staged ==
          std::vector<std::pair<std::string, std::string>>{{fv::path_to_utf8(source), ""}});
    // The restore ran inside the worker context; inspection did not.
    CHECK(hooks.events() ==
          std::vector<std::string>{"acquire", "context_enter", "context_exit", "release"});
    manager->shutdown();
}

TEST_CASE("snapshot exposes previous data location when available", "[test_backup_manager]") {
    // from
    // tests/test_backup_manager.py::test_snapshot_exposes_previous_data_location_when_available
    fv::testkit::TempDataRoot temp;
    const fs::path data = temp.path() / "data";
    const fs::path previous = data / ".fiorivoce-restore" / "previous-123";
    Rig rig;
    rig.service.on_previous = [&](const fs::path& root) -> std::optional<fs::path> {
        CHECK(fv::path_to_utf8(root) == resolved(data));
        return previous;
    };
    auto manager = rig.make(data);
    CHECK(manager->snapshot()["previous_data_dir"] == fv::path_to_utf8(previous));
    rig.service.on_previous = [](const fs::path&) -> std::optional<fs::path> {
        throw std::runtime_error("bad metadata");
    };
    CHECK(manager->snapshot()["previous_data_dir"] == "");
    manager->shutdown();
}

TEST_CASE("a refused reservation reaches manual callers only", "[backup]") {
    fv::testkit::TempDataRoot temp;
    const fs::path folder = temp.path() / "backups";
    fs::create_directories(folder);
    Rig rig;
    FakeHooks hooks;
    hooks.set_refusal("Wait for the recording to finish.");
    auto manager = rig.make(temp.path() / "data", hooks.hooks());
    std::vector<Result> results;
    std::vector<std::pair<std::string, std::string>> staged;
    int busy_signals = 0;
    auto on_backup = manager->backup_finished.connect(
        rig.ui, [&](const std::optional<BackupInfo>& info, const std::string& error) {
            results.emplace_back(info, error);
        });
    auto on_staged = manager->restore_staged.connect(
        rig.ui, [&](const std::string& path, const std::string& error) {
            staged.emplace_back(path, error);
        });
    auto on_busy = manager->busy_changed.connect(rig.ui, [&](bool) { ++busy_signals; });

    manager->create_backup(temp.path() / "manual.fvbackup");
    manager->prepare_restore(temp.path() / "manual.fvbackup");
    rig.drain();
    REQUIRE(results.size() == 1);
    CHECK_FALSE(results[0].first.has_value());
    CHECK(results[0].second == "Wait for the recording to finish.");
    CHECK(staged == std::vector<std::pair<std::string, std::string>>{
                        {"", "Wait for the recording to finish."}});

    // An automatic backup that cannot reserve the data stays silent and is not retried early.
    CHECK(manager->set_schedule("daily", 2, folder).empty());
    manager->scheduled_tick();
    rig.drain();
    CHECK(results.size() == 1);
    CHECK(busy_signals == 0);
    CHECK(rig.service.calls().empty());
    CHECK(hooks.events() == std::vector<std::string>{"acquire", "acquire", "acquire"});

    // A hook that throws refuses with its message.
    BackupCoordinator::Hooks throwing;
    throwing.acquire = []() -> std::optional<std::string> {
        throw std::runtime_error("The data is busy.");
    };
    auto other = rig.make(temp.path() / "data2", std::move(throwing));
    std::vector<Result> other_results;
    auto on_other = other->backup_finished.connect(
        rig.ui, [&](const std::optional<BackupInfo>& info, const std::string& error) {
            other_results.emplace_back(info, error);
        });
    other->create_backup(temp.path() / "other.fvbackup");
    rig.drain();
    REQUIRE(other_results.size() == 1);
    CHECK(other_results[0].second == "The data is busy.");
    other->shutdown();
    manager->shutdown();
}

TEST_CASE("one operation at a time, and nothing after shutdown", "[backup]") {
    fv::testkit::TempDataRoot temp;
    Rig rig;
    auto manager = rig.make(temp.path() / "data");
    std::vector<Result> results;
    auto connection = manager->backup_finished.connect(
        rig.ui, [&](const std::optional<BackupInfo>& info, const std::string& error) {
            results.emplace_back(info, error);
        });
    manager->create_backup(temp.path() / "first.fvbackup");
    manager->create_backup(temp.path() / "second.fvbackup");
    CHECK(manager->set_schedule("off", 5, "") ==
          "Wait for the current backup operation to finish.");
    rig.drain();
    REQUIRE(results.size() == 2);
    CHECK(results[0].second == "Another backup operation is already running.");
    CHECK(results[1].first.has_value());
    CHECK(results[1].second.empty());
    CHECK(rig.service.calls().size() == 1);

    manager->shutdown();
    manager->create_backup(temp.path() / "third.fvbackup");
    rig.drain();
    REQUIRE(results.size() == 3);
    CHECK(results[2].second == "Another backup operation is already running.");
    CHECK(rig.service.calls().size() == 1);
}

TEST_CASE("shutdown drops a request that has not started and ends its reservation", "[backup]") {
    fv::testkit::TempDataRoot temp;
    Rig rig;
    FakeHooks hooks;
    auto manager = rig.make(temp.path() / "data", hooks.hooks());
    std::vector<Result> results;
    std::vector<bool> busy;
    auto on_result = manager->backup_finished.connect(
        rig.ui, [&](const std::optional<BackupInfo>& info, const std::string& error) {
            results.emplace_back(info, error);
        });
    auto on_busy =
        manager->busy_changed.connect(rig.ui, [&](bool value) { busy.push_back(value); });
    manager->create_backup(temp.path() / "queued.fvbackup");
    manager->shutdown();
    CHECK(hooks.events() == std::vector<std::string>{"acquire", "release"});
    rig.drain();
    CHECK(rig.service.calls().empty());
    CHECK(results.empty()); // a closed coordinator reports no result
    CHECK(busy == std::vector<bool>{true, false});
    CHECK(hooks.events() == std::vector<std::string>{"acquire", "release"});
}

TEST_CASE("set_schedule refuses what Python refuses", "[backup]") {
    fv::testkit::TempDataRoot temp;
    const fs::path data = temp.path() / "data";
    const fs::path folder = temp.path() / "backups";
    fs::create_directories(folder);
    fs::create_directories(data / "recordings" / "inside");
    fs::create_directories(data / ".fiorivoce-restore" / "x");
    Rig rig;
    auto manager = rig.make(data);
    int changes = 0;
    auto connection = manager->schedule_changed.connect(rig.ui, [&] { ++changes; });
    CHECK(manager->set_schedule("hourly", 3, folder) == "Choose Off, Daily, or Weekly.");
    CHECK(manager->set_schedule("daily", 0, folder) == "Keep between 1 and 100 automatic backups.");
    CHECK(manager->set_schedule("daily", 101, folder) ==
          "Keep between 1 and 100 automatic backups.");
    CHECK(manager->set_schedule("daily", 3, "") ==
          "Choose an existing folder for automatic backups.");
    CHECK(manager->set_schedule("daily", 3, temp.path() / "missing") ==
          "Choose an existing folder for automatic backups.");
    CHECK(manager->set_schedule("daily", 3, data / "recordings" / "inside") ==
          "Choose a backup folder outside FioriVoce's recording folders.");
    CHECK(manager->set_schedule("weekly", 3, data / ".fiorivoce-restore" / "x") ==
          "Choose a backup folder outside FioriVoce's recording folders.");
    rig.ui.run_all();
    CHECK(changes == 0);
    CHECK_FALSE(fs::exists(data / kPreferencesFile));

    // "off" keeps the text it was given, stripped, and is saved without checking a folder.
    CHECK(manager->set_schedule("off", 7, fv::path_from_utf8("  nowhere  ")).empty());
    rig.ui.run_all();
    CHECK(changes == 1);
    CHECK(manager->snapshot()["destination"] == "nowhere");
    CHECK(manager->snapshot()["retention_count"] == 7);
    manager->shutdown();
}

TEST_CASE("an automatic failure is retried ten minutes later", "[backup]") {
    fv::testkit::TempDataRoot temp;
    const fs::path folder = temp.path() / "backups";
    fs::create_directories(folder);
    Rig rig;
    rig.service.on_create = [](const fs::path&, const fv::backup::CreateOptions&) {
        throw fv::backup::BackupError("Not enough free disk space for a safe backup or restore");
    };
    auto manager = rig.make(temp.path() / "data");
    std::vector<Result> results;
    auto connection = manager->backup_finished.connect(
        rig.ui, [&](const std::optional<BackupInfo>& info, const std::string& error) {
            results.emplace_back(info, error);
        });
    CHECK(manager->set_schedule("daily", 2, folder).empty());
    manager->scheduled_tick();
    rig.drain();
    REQUIRE(results.size() == 1);
    CHECK(results[0].second == "Not enough free disk space for a safe backup or restore");
    CHECK(manager->snapshot()["last_error"] == results[0].second);
    CHECK(manager->snapshot()["next_backup_at"] == rig.now() + 600);

    manager->scheduled_tick(); // before the retry time: nothing
    rig.drain();
    CHECK(rig.service.calls().size() == 1);
    rig.clock.advance(std::chrono::seconds(600));
    manager->scheduled_tick();
    rig.drain();
    CHECK(rig.service.calls().size() == 2);
    manager->shutdown();
}

TEST_CASE("the schedule ticks every 60 seconds once started", "[backup]") {
    fv::testkit::TempDataRoot temp;
    const fs::path folder = temp.path() / "backups";
    fs::create_directories(folder);
    Rig rig;
    auto manager = rig.make(temp.path() / "data");
    CHECK(manager->set_schedule("daily", 2, folder).empty());
    rig.ui.advance(std::chrono::seconds(120));
    CHECK(rig.service.calls().empty()); // not started

    manager->start();
    rig.ui.advance(std::chrono::seconds(59));
    rig.drain();
    CHECK(rig.service.calls().empty());
    rig.ui.advance(std::chrono::seconds(1));
    rig.drain();
    CHECK(rig.service.calls().size() == 1);
    // The next day's backup comes from a later tick.
    rig.ui.advance(std::chrono::seconds(86400));
    rig.drain();
    CHECK(rig.service.calls().size() == 2);
    manager->shutdown();
    rig.ui.advance(std::chrono::seconds(86400 * 2));
    rig.drain();
    CHECK(rig.service.calls().size() == 2);
}

TEST_CASE("progress is scaled to 1000 and same-phase steps within 0.1 s are dropped", "[backup]") {
    fv::testkit::TempDataRoot temp;
    Rig rig;
    rig.service.on_create = [&](const fs::path&, const fv::backup::CreateOptions& options) {
        options.progress("scan", 3, 3);
        options.progress("archive", 1, 3);
        options.progress("archive", 2, 3); // same phase, unfinished, too soon
        rig.clock.advance(std::chrono::milliseconds(100));
        options.progress("archive", 2, 3);
        options.progress("archive", 3, 3); // finishes the phase
        options.progress("stage", 0, 0);
    };
    auto manager = rig.make(temp.path() / "data");
    std::vector<std::tuple<std::string, int, int>> steps;
    auto connection =
        manager->progress.connect(rig.ui, [&](const std::string& phase, int done, int total) {
            steps.emplace_back(phase, done, total);
        });
    manager->create_backup(temp.path() / "x.fvbackup");
    rig.drain();
    // The fake's own final step ("Writing archive", 7, 7) comes last.
    CHECK(steps == std::vector<std::tuple<std::string, int, int>>{{"scan", 1000, 1000},
                                                                  {"archive", 333, 1000},
                                                                  {"archive", 666, 1000},
                                                                  {"archive", 1000, 1000},
                                                                  {"stage", 0, 0},
                                                                  {"Writing archive", 1000, 1000}});
    manager->shutdown();
}

TEST_CASE("a backup records itself and a failed history update is reported", "[backup]") {
    fv::testkit::TempDataRoot temp;
    const fs::path data = temp.path() / "data";
    Rig rig;
    auto manager = rig.make(data);
    std::vector<Result> results;
    auto connection = manager->backup_finished.connect(
        rig.ui, [&](const std::optional<BackupInfo>& info, const std::string& error) {
            results.emplace_back(info, error);
        });
    const fs::path destination = temp.path() / "kept.fvbackup";
    manager->create_backup(destination);
    rig.drain();
    REQUIRE(results.size() == 1);
    CHECK(results[0].second.empty());
    const Json saved = fv::py::loads(read_bytes(data / kPreferencesFile));
    CHECK(saved["last_backup"] == Json{{"path", fv::path_to_utf8(destination)},
                                       {"created_at", "2026-10-04T12:00:00+00:00"},
                                       {"app_version", "2.5.0"},
                                       {"schema_version", 1},
                                       {"include_recordings", true},
                                       {"file_count", 2},
                                       {"total_bytes", 20},
                                       {"archive_bytes", 7},
                                       {"warnings", Json::array()}});
    CHECK(saved["last_auto_at"] == 0);
    CHECK(saved["scheduled_archives"] == Json::array());

    // The preferences file turned into a folder: the archive stands, the history update fails.
    fs::remove(data / kPreferencesFile);
    fs::create_directories(data / kPreferencesFile / "blocked");
    manager->create_backup(temp.path() / "second.fvbackup");
    rig.drain();
    REQUIRE(results.size() == 2);
    CHECK(results[1].first.has_value());
    CHECK_THAT(
        results[1].second,
        ContainsSubstring("Backup saved, but its history or retention could not be updated: "));
    // As in Python, the coordinator still holds the new backup although the file was not saved.
    CHECK(manager->snapshot()["last_backup"]["path"] ==
          fv::path_to_utf8(temp.path() / "second.fvbackup"));
    manager->shutdown();
}

TEST_CASE("a failure that is not a std::exception still ends the request", "[backup]") {
    // Python's finished() catches every Exception: str() of one without a message is "", which
    // reports "Backup operation failed.".
    fv::testkit::TempDataRoot temp;
    Rig rig;
    FakeHooks hooks;
    rig.service.on_create = [](const fs::path&, const fv::backup::CreateOptions&) {
        throw 42;
    };
    auto manager = rig.make(temp.path() / "data", hooks.hooks());
    std::vector<Result> results;
    auto connection = manager->backup_finished.connect(
        rig.ui, [&](const std::optional<BackupInfo>& info, const std::string& error) {
            results.emplace_back(info, error);
        });
    manager->create_backup(temp.path() / "odd.fvbackup");
    rig.drain();
    REQUIRE(results.size() == 1);
    CHECK_FALSE(results[0].first.has_value());
    CHECK(results[0].second == "Backup operation failed.");
    CHECK_FALSE(manager->busy());
    CHECK(manager->snapshot()["last_error"] == "Backup operation failed.");
    CHECK(hooks.events() ==
          std::vector<std::string>{"acquire", "context_enter", "context_exit", "release"});
    // The next request runs.
    rig.service.on_create = nullptr;
    manager->create_backup(temp.path() / "next.fvbackup");
    rig.drain();
    REQUIRE(results.size() == 2);
    CHECK(results[1].second.empty());
    manager->shutdown();
}

TEST_CASE("a backup cancelled while it runs on a real worker thread reports the cancel",
          "[backup]") {
    // tests/test_backup_manager.py's cancel test as it runs there: the cancel lands while the
    // worker thread is inside create_backup.
    fv::testkit::TempDataRoot temp;
    Rig rig;
    fv::ThreadPool pool("fv-test-pool", 1);
    fv::Strand worker(pool, "backup", 1, fv::ShutdownMode::CancelQueued);
    std::atomic<bool> started = false;
    int releases = 0;
    BackupCoordinator::Hooks hooks;
    hooks.release = [&] {
        ++releases;
    };
    rig.service.on_create = [&](const fs::path&, const fv::backup::CreateOptions& options) {
        started = true;
        if (!options.cancel.wait_for(std::chrono::seconds(3))) {
            throw std::runtime_error("the cancel never came");
        }
        throw fv::backup::BackupCancelled();
    };
    BackupCoordinator manager(paths_for(temp.path() / "data"), fv::identity(), std::move(hooks),
                              rig.ui, worker, rig.clock, "0.1.0", &rig.service);
    std::vector<Result> results;
    auto connection = manager.backup_finished.connect(
        rig.ui, [&](const std::optional<BackupInfo>& info, const std::string& error) {
            results.emplace_back(info, error);
        });
    const fs::path destination = temp.path() / "cancelled.fvbackup";
    manager.create_backup(destination);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!started && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    REQUIRE(started);
    CHECK(manager.busy());
    manager.cancel();
    while (results.empty() && std::chrono::steady_clock::now() < deadline) {
        rig.ui.run_all();
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    REQUIRE(results.size() == 1);
    CHECK(results[0].second == "Backup canceled");
    CHECK(releases == 1);
    CHECK_FALSE(fs::exists(destination));
    CHECK_FALSE(manager.busy());
    manager.shutdown();
}

TEST_CASE("shutdown waits for a request that is running", "[backup]") {
    fv::testkit::TempDataRoot temp;
    Rig rig;
    fv::ThreadPool pool("fv-test-pool", 1);
    fv::Strand worker(pool, "backup", 1, fv::ShutdownMode::CancelQueued);
    std::atomic<bool> started = false;
    std::atomic<bool> cancelled = false;
    std::atomic<bool> finished = false;
    std::vector<std::string> events;
    BackupCoordinator::Hooks hooks;
    hooks.release = [&] {
        events.emplace_back("release");
    };
    rig.service.on_create = [&](const fs::path&, const fv::backup::CreateOptions& options) {
        started = true;
        // shutdown cancels the backup; the work still returns before shutdown does.
        cancelled = options.cancel.wait_for(std::chrono::seconds(3));
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        finished = true;
    };
    BackupCoordinator manager(paths_for(temp.path() / "data"), fv::identity(), std::move(hooks),
                              rig.ui, worker, rig.clock, "0.1.0", &rig.service);
    manager.create_backup(temp.path() / "running.fvbackup");
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!started && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    REQUIRE(started);
    manager.shutdown();
    CHECK(finished);
    CHECK(cancelled);
    CHECK(events == std::vector<std::string>{"release"});
    rig.ui.run_all();
    CHECK_FALSE(manager.busy());
    CHECK(events == std::vector<std::string>{"release"});
}
