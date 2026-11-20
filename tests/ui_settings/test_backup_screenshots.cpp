// The Backup & restore page against the references OpenWhisper rendered for it
// (tools/parity/builders/a11.py, tests/reference/backup_settings_page/<variant>/): the page in
// its Settings page container, laid out at the builder's width, taken out of the window as the
// builder takes OpenWhisper's out of its dialog. The widget tree first, then the pixels, with the
// application styled as OpenWhisper styles it.
//
// The builder pins the local zone to UTC and today to 2026-10-04, as this test's clock is, and
// its coordinator's snapshot: the last backup of 2026-10-04T12:30:00Z (734,003,200 bytes) in
// C:\Backups\OpenWhisper_CPP, daily, keeping 5, next due 2026-10-05 12:30 UTC, nothing inspected.
#include <catch2/catch_test_macros.hpp>

#include <memory>
#include <string>

#include <QCoreApplication>
#include <QResizeEvent>
#include <QScrollArea>

#include "backup_support.h"
#include "screenshot/screenshot.h"
#include "settings_support.h"
#include "ui_settings/settings_page.h"

using fv::Json;
using fvtest::AppTheme;
using fvtest::BackupRig;
namespace dest = fvtest::dest;

namespace {

constexpr int kWide = 900;
constexpr int kNarrow = 360;

// Tree classes, ignoring the page container's (OpenWhisper's private _SettingsPage).
void checkClasses(const Json& expected, const Json& actual, const std::string& path) {
    std::string wanted = expected.value("class", std::string());
    if (wanted == "_SettingsPage") {
        wanted = "SettingsPage";
    }
    INFO(path);
    CHECK(actual.value("class", std::string()) == wanted);
    const Json empty = Json::array();
    const Json& want = expected.contains("children") ? expected["children"] : empty;
    const Json& have = actual.contains("children") ? actual["children"] : empty;
    REQUIRE(want.size() == have.size());
    for (std::size_t i = 0; i < want.size(); ++i) {
        const std::string name = want[i].value("objectName", std::string());
        checkClasses(want[i], have[i],
                     path + "/" + (name.empty() ? wanted + "[" + std::to_string(i) + "]" : name));
    }
}

Json snapshot() {
    Json last = {{"path", "C:\\Backups\\OpenWhisper_CPP\\OpenWhisper_CPP-backup.fvbackup"},
                 {"created_at", "2026-10-04T12:30:00Z"},
                 {"app_version", "0.1.0"},
                 {"schema_version", 1},
                 {"include_recordings", true},
                 {"file_count", 14},
                 {"total_bytes", 1468006400},
                 {"archive_bytes", 734003200},
                 {"warnings", Json::array()}};
    // The last automatic run, 2026-10-04T12:30:00Z, a day before the next.
    return Json{{"frequency", "daily"},
                {"retention_count", 5},
                {"destination", "C:\\Backups\\OpenWhisper_CPP"},
                {"last_backup", last},
                {"last_auto_at", 1791117000}};
}

// What Bootstrap builds for Settings, with the clock of the builder.
class ShotRig {
public:
    ShotRig()
        : backup(snapshot()), paths_(fv::Paths::resolve(fv::identity(), fv::system_env())),
          settings_(Json::object()), ui_(qApp), context_(services()) {
        context_.provide<fv::backup::BackupCoordinator>(backup.coordinator);
        fv::registerBackupPage(context_);
        dialog = std::make_unique<fv::SettingsDialog>(context_);
        dialog->availableScreenRect = [] {
            return QRect(0, 0, 1920, 1080);
        };
    }

    ~ShotRig() {
        dialog.reset();
        QCoreApplication::processEvents();
    }

    BackupRig backup;
    std::unique_ptr<fv::SettingsDialog> dialog;

private:
    fv::AppServices services() {
        fv::AppServices made;
        made.identity = &fv::identity();
        made.paths = &paths_;
        made.settings = &settings_;
        made.ui = &ui_;
        made.clock = &backup.clock;
        return made;
    }

    fv::Paths paths_;
    fv::MemorySettingsStore settings_;
    fv::qt::QtUiExecutor ui_;
    fv::AppContext context_;
};

// The builder's measurement of a page that is not given a height: shown at its width, events
// processed, hidden again; a grab lays out what the tree dump then reads.
void settle(QWidget& widget, int width) {
    widget.resize(width, 4000);
    widget.show();
    for (int i = 0; i < 3; ++i) {
        QCoreApplication::processEvents();
    }
    (void)widget.grab();
    widget.hide();
}

void backupShot(const std::string& theme, int scale, const char* name, int width, bool measure) {
    AppTheme appTheme(QString::fromStdString(theme), scale);
    ShotRig rig;
    fv::SettingsDialog& dialog = *rig.dialog;
    fv::SettingsPage* page = dialog.ensurePage(dest::backup);
    REQUIRE(page != nullptr);
    BackupRig::settle();
    QScrollArea* scroll = dialog.pageScroll(dest::backup);
    REQUIRE(scroll != nullptr);
    std::unique_ptr<QWidget> holder(scroll->takeWidget());
    REQUIRE(holder.get() == page);
    holder->setParent(nullptr);
    QCoreApplication::processEvents();
    if (measure) {
        const QSize old = page->size();
        page->resize(width, 4000);
        QResizeEvent resized(page->size(), old);
        QCoreApplication::sendEvent(page, &resized);
        settle(*page, width);
    }
    const std::string variant = theme + "-windows11-dpr1-s" + std::to_string(scale);
    page->ensurePolished();
    const Json reference = fv::screenshot::load_reference(name, variant).at("tree");
    checkClasses(reference, fv::screenshot::dump_tree(*page), name);
    fv::screenshot::Options options;
    options.ignore_keys = {"class"};
    fv::screenshot::expect_matches(*page, name, variant, options);
}

} // namespace

TEST_CASE("reference screenshot backup_settings_page default dark", "[screenshot]") {
    backupShot("dark", 100, "backup_settings_page/default", kWide, false);
}

TEST_CASE("reference screenshot backup_settings_page default light", "[screenshot]") {
    backupShot("light", 100, "backup_settings_page/default", kWide, false);
}

TEST_CASE("reference screenshot backup_settings_page default dark 130", "[screenshot]") {
    backupShot("dark", 130, "backup_settings_page/default", kWide, false);
}

TEST_CASE("reference screenshot backup_settings_page narrow dark", "[screenshot]") {
    backupShot("dark", 100, "backup_settings_page/narrow", kNarrow, true);
}
