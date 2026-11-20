#pragma once

// The Backup page over fakes: the real BackupCoordinator on a fake clock and manual executors (the
// UI actor and the backup worker each run only when the test drains them), with
// backup::testing::FakeBackupService doing the archive work. OpenWhisper's tests drive a fake
// coordinator; the real one with a fake service gives the page the same signals, and a test can
// also emit them itself (they are public members).

#include <chrono>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <QApplication>
#include <QCoreApplication>
#include <QString>

#include "appkit/qt_bridge.h"
#include "backup/coordinator.h"
#include "backup/preferences.h"
#include "backup/testing/fake_hooks.h"
#include "base/identity.h"
#include "base/json.h"
#include "base/paths.h"
#include "base/testing/fake_clock.h"
#include "base/testing/manual_executor.h"
#include "base/testing/map_env_reader.h"
#include "base/utf8.h"
#include "testkit/testkit.h"
#include "ui_settings/settings_backup.h"

namespace fvtest {

// A coordinator's data folder, clock (2026-10-04 12:30 UTC, in UTC) and executors.
class BackupRig {
public:
    // `preferences` is written as backup-preferences.json before the coordinator reads it.
    explicit BackupRig(const fv::Json& preferences = fv::Json())
        : clock(std::chrono::sys_days{std::chrono::year{2026} / 10 / 4} + std::chrono::hours{12} +
                    std::chrono::minutes{30},
                "UTC"),
          ui(clock) {
        data = temp.path() / "data";
        std::filesystem::create_directories(data);
        if (!preferences.is_null()) {
            fv::backup::BackupPreferences loaded =
                fv::backup::BackupPreferences::from_json(preferences);
            fv::backup::save_preferences(data, loaded);
        }
        fv::MapEnvReader env{{"OPENWHISPER_CPP_DATA_DIR", fv::path_to_utf8(data)}};
        fv::pin_per_user_folders(env, data.parent_path() / "per-user");
        const fv::Paths paths = fv::Paths::resolve(fv::identity(), env);
        coordinator = std::make_shared<fv::backup::BackupCoordinator>(
            paths, fv::identity(), hooks.hooks(), ui, worker, clock, "0.1.0", &service);
    }

    // The coordinator's executors run dry, then the page's queued updates are delivered.
    void drain() {
        while (worker.run_all() + ui.run_all() > 0) {
        }
        settle();
    }

    static void settle() {
        for (int i = 0; i < 4; ++i) {
            QCoreApplication::processEvents();
        }
    }

    // A file the user could pick: it exists, under the rig's folder.
    std::filesystem::path file(const std::string& name) const {
        std::filesystem::create_directories(temp.path() / "files");
        const auto path = temp.path() / "files" / fv::path_from_utf8(name);
        std::ofstream(path, std::ios::binary) << "x";
        return path;
    }

    static QString text(const std::filesystem::path& path) {
        return fv::qt::toQString(fv::path_to_utf8(path));
    }

    fv::testkit::TempDataRoot temp;
    std::filesystem::path data;
    fv::FakeClock clock;
    fv::ManualExecutor ui;
    fv::ManualExecutor worker;
    fv::backup::testing::FakeBackupService service;
    fv::backup::testing::FakeHooks hooks;
    std::shared_ptr<fv::backup::BackupCoordinator> coordinator;
};

// What the user picks in the page's dialogs, and what the page asked.
struct Choices {
    QString save;
    QString open;
    QString folder;
    bool restore = false;
    std::vector<QString> titles;
    std::vector<QString> filters;
    std::vector<QString> starts;
    QString confirmText;
    int confirmations = 0;

    fv::BackupSettingsPage::Dialogs dialogs() {
        fv::BackupSettingsPage::Dialogs made;
        made.saveFile = [this](QWidget*, const QString& title, const QString& start,
                               const QString& filter) {
            titles.push_back(title);
            starts.push_back(start);
            filters.push_back(filter);
            return save;
        };
        made.openFile = [this](QWidget*, const QString& title, const QString& start,
                               const QString& filter) {
            titles.push_back(title);
            starts.push_back(start);
            filters.push_back(filter);
            return open;
        };
        made.chooseFolder = [this](QWidget*, const QString& title, const QString& start) {
            titles.push_back(title);
            starts.push_back(start);
            return folder;
        };
        made.confirm = [this](QWidget*, const QString& title, const QString& text) {
            titles.push_back(title);
            confirmText = text;
            ++confirmations;
            return restore;
        };
        return made;
    }
};

inline std::unique_ptr<fv::BackupSettingsPage>
makeBackupPage(BackupRig& rig, Choices& choices, fv::BackupImportOffer* offer = nullptr,
               bool withCoordinator = true) {
    fv::BackupSettingsPage::Options options;
    options.clock = &rig.clock;
    options.dataRoot = rig.data;
    options.dialogs = choices.dialogs();
    options.import = offer;
    return std::make_unique<fv::BackupSettingsPage>(
        withCoordinator ? rig.coordinator.get() : nullptr, std::move(options));
}

} // namespace fvtest
