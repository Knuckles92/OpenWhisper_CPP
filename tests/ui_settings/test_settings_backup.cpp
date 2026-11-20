// The Backup & restore page and its calendar (OpenWhisper's tests/test_settings_backup.py), over
// the real coordinator with a fake archive service (backup_support.h), plus what OpenWhisper_CPP
// adds: the import button, the lock that follows the coordinator, and the page's registration.
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <algorithm>
#include <filesystem>
#include <string>
#include <vector>

#include <QBoxLayout>
#include <QCheckBox>
#include <QImage>
#include <QLineEdit>
#include <QProgressBar>
#include <QSpinBox>
#include <QTest>
#include <QToolButton>

#include "backup_support.h"
#include "home_support.h"
#include "ui_settings/backup_calendar.h"
#include "ui_settings/settings_backup.h"
#include "ui_theme/palette.h"
#include "ui_widgets/buttons.h"
#include "ui_widgets/eliding_label.h"
#include "ui_widgets/nav_rail.h"
#include "ui_widgets/no_wheel.h"
#include "ui_widgets/setting_tile.h"
#include "ui_widgets/wrapped_label.h"

namespace fs = std::filesystem;
using Catch::Matchers::ContainsSubstring;
using fv::BackupCalendar;
using fv::BackupSettingsPage;
using fv::Json;
using fv::backup::BackupInfo;
using fv::backup::testing::FakeBackupService;
using fv::history::Day;
using fv::history::MonthKey;
using fvtest::AppTheme;
using fvtest::BackupRig;
using fvtest::Choices;
using fvtest::makeBackupPage;
namespace dest = fvtest::dest;

namespace {

Day day(int year, unsigned month, unsigned dayOfMonth) {
    return Day{std::chrono::year{year} / std::chrono::month{month} / std::chrono::day{dayOfMonth}};
}

std::string text(const QLabel* label) {
    return fv::qt::toUtf8(label->text());
}

std::string text(const fv::ElidingLabel* label) {
    return fv::qt::toUtf8(label->text());
}

std::string text(const fv::WrappedLabel* label) {
    return fv::qt::toUtf8(label->text());
}

std::vector<FakeBackupService::Call> callsOf(const BackupRig& rig, const std::string& kind) {
    std::vector<FakeBackupService::Call> found;
    for (const auto& call : rig.service.calls()) {
        if (call.front() == kind) {
            found.push_back(call);
        }
    }
    return found;
}

BackupInfo infoWith(const fs::path& path) {
    BackupInfo info = FakeBackupService::info_for(path);
    info.created_at = "2026-10-04T12:30:00";
    info.app_version = "2.5.0";
    info.file_count = 4;
    info.total_bytes = 4096;
    info.archive_bytes = 2048;
    return info;
}

// The preferences of test_calendar_shows_saved_schedule_and_latest_backup_only: weekly, the last
// backup on 2026-10-04 and the next one due a week after the last automatic one.
Json weeklyPreferences(const std::string& folder) {
    Json last = {{"path", "C:\\Backups\\manual.fvbackup"},
                 {"created_at", "2026-10-04T12:30:00Z"},
                 {"app_version", "2.5.0"},
                 {"schema_version", 1},
                 {"include_recordings", true},
                 {"file_count", 4},
                 {"total_bytes", 4096},
                 {"archive_bytes", 2048},
                 {"warnings", Json::array()}};
    // 2026-10-04T12:00:00Z.
    return Json{{"frequency", "weekly"},
                {"retention_count", 7},
                {"destination", folder},
                {"last_backup", last},
                {"last_auto_at", 1791115200}};
}

// An OpenWhisper import the page can offer.
class FakeOffer : public fv::BackupImportOffer {
public:
    bool available() const override { return found; }
    void refresh() override { ++refreshes; }
    void open(QWidget* parent) override { opened.push_back(parent); }

    void find(bool now) {
        found = now;
        Q_EMIT availabilityChanged(now);
    }

    bool found = false;
    int refreshes = 0;
    std::vector<QWidget*> opened;
};

} // namespace

// from tests/test_settings_backup.py::test_manual_backup_uses_selected_path_and_reports_progress
TEST_CASE("test_manual_backup_uses_selected_path_and_reports_progress",
          "[test_settings_backup][widget]") {
    BackupRig rig;
    Choices choices;
    auto page = makeBackupPage(rig, choices);
    CHECK(page->includeRecordingsCheck()->isChecked());
    CHECK_THAT(text(page->privacyLabel()), ContainsSubstring("not encrypted"));
    fs::create_directories(rig.temp.path() / "out");
    const fs::path target = rig.temp.path() / "out" / "manual.fvbackup";
    choices.save = BackupRig::text(target);

    page->includeRecordingsCheck()->setChecked(false);
    page->createButton()->click();
    BackupRig::settle();
    CHECK(choices.titles.back() == "Create OpenWhisper_CPP backup");
    CHECK(choices.filters.back() == "OpenWhisper_CPP backups (*.fvbackup);;All files (*)");
    CHECK(choices.starts.back() == "OpenWhisper_CPP-backup.fvbackup");
    CHECK(rig.coordinator->busy());
    CHECK_FALSE(page->createButton()->isEnabled());
    CHECK_FALSE(page->cancelButton()->isHidden());
    // Cancelling asks the coordinator, whose worker then stops the backup.
    page->cancelButton()->click();
    CHECK_FALSE(page->cancelButton()->isEnabled());
    CHECK(text(page->statusLabel()) == "Cancelling\xE2\x80\xA6");
    rig.drain();
    CHECK(callsOf(rig, "backup") ==
          std::vector<FakeBackupService::Call>{{"backup", fv::path_to_utf8(target), "false"}});
    CHECK(text(page->statusLabel()) == "Backup failed: Backup canceled");
    CHECK(page->createButton()->isEnabled());
    CHECK(page->cancelButton()->isHidden());
    CHECK_FALSE(fs::exists(target));

    // The second backup reports its progress, then lands.
    page->createButton()->click();
    BackupRig::settle();
    CHECK_FALSE(page->cancelButton()->isHidden());
    rig.coordinator->progress.emit("Saving history", 1, 2);
    BackupRig::settle();
    CHECK_THAT(text(page->statusLabel()), ContainsSubstring("50%"));
    rig.drain();
    CHECK(page->createButton()->isEnabled());
    CHECK_THAT(text(page->lastLocationLabel()), ContainsSubstring("manual.fvbackup"));
    CHECK_THAT(text(page->lastBackupLabel()), ContainsSubstring("Last backup"));
    CHECK_THAT(text(page->statusLabel()), ContainsSubstring("Backup created: "));

    rig.coordinator->backup_finished.emit(FakeBackupService::info_for(target),
                                          "retention could not be updated");
    BackupRig::settle();
    CHECK_THAT(text(page->statusLabel()), ContainsSubstring("Backup saved"));
    CHECK_THAT(text(page->statusLabel()), ContainsSubstring("retention could not be updated"));
    BackupInfo warned = FakeBackupService::info_for(target);
    warned.warnings = {"one", "two"};
    rig.coordinator->backup_finished.emit(warned, "");
    BackupRig::settle();
    CHECK_THAT(text(page->statusLabel()), ContainsSubstring("\nWarnings: one; two"));
}

// from tests/test_settings_backup.py::test_without_coordinator_controls_are_unavailable
TEST_CASE("test_without_coordinator_controls_are_unavailable", "[test_settings_backup][widget]") {
    BackupRig rig;
    Choices choices;
    auto page = makeBackupPage(rig, choices, nullptr, false);
    CHECK_FALSE(page->createButton()->isEnabled());
    CHECK_FALSE(page->inspectButton()->isEnabled());
    CHECK_FALSE(page->saveScheduleButton()->isEnabled());
    CHECK_FALSE(page->importButton()->isEnabled());
    CHECK_THAT(text(page->statusLabel()), ContainsSubstring("unavailable"));
    CHECK(text(page->statusLabel()) == "Backup and restore are unavailable in this session.");
    // Nothing reaches a dialog either.
    page->createButton()->click();
    CHECK(choices.titles.empty());
}

// from tests/test_settings_backup.py::test_previous_restore_data_path_is_visible_and_selectable
TEST_CASE("test_previous_restore_data_path_is_visible_and_selectable",
          "[test_settings_backup][widget]") {
    BackupRig rig;
    Choices choices;
    const fs::path previous = rig.temp.path() / "previous-123";
    rig.service.on_previous = [&](const fs::path&) {
        return std::optional<fs::path>(previous);
    };
    auto page = makeBackupPage(rig, choices);
    CHECK_THAT(text(page->recoveryLocationLabel()), ContainsSubstring("previous-123"));
    CHECK(text(page->recoveryLocationLabel()).starts_with("Previous data: "));
    CHECK_FALSE(page->recoveryLocationLabel()->isHidden());
    CHECK((page->recoveryLocationLabel()->textInteractionFlags() & Qt::TextSelectableByMouse) != 0);
}

// from tests/test_settings_backup.py::test_restore_requires_inspection_and_explicit_confirmation
TEST_CASE("test_restore_requires_inspection_and_explicit_confirmation",
          "[test_settings_backup][widget]") {
    BackupRig rig;
    Choices choices;
    auto page = makeBackupPage(rig, choices);
    CHECK_FALSE(page->restoreButton()->isEnabled());
    const fs::path archive = rig.file("recover.fvbackup");
    choices.open = BackupRig::text(archive);

    page->inspectButton()->click();
    BackupRig::settle();
    CHECK(choices.titles.back() == "Inspect OpenWhisper_CPP backup");
    CHECK(choices.filters.back() ==
          "OpenWhisper_CPP backups (*.fvbackup *.owbackup *.zip);;All files (*)");
    CHECK(text(page->inspectionLabel()) == "Inspecting backup\xE2\x80\xA6");
    CHECK_FALSE(page->restoreButton()->isEnabled());
    // Inspecting is not cancellable.
    CHECK(page->cancelButton()->isHidden());
    rig.drain();
    CHECK(callsOf(rig, "inspect") ==
          std::vector<FakeBackupService::Call>{{"inspect", fv::path_to_utf8(archive)}});
    CHECK(page->restoreButton()->isEnabled());
    CHECK_THAT(text(page->inspectionLabel()), ContainsSubstring("2 files"));
    CHECK_THAT(text(page->inspectionLabel()),
               ContainsSubstring("Saved recordings and captures: included"));
    CHECK(text(page->statusLabel()) == "Backup inspected. Review the contents before restoring.");
    CHECK(page->inspectedPath() == BackupRig::text(archive));
    CHECK(text(page->inspectionLocationLabel()) == "Location: " + fv::path_to_utf8(archive));

    BackupInfo warned = FakeBackupService::info_for(archive);
    warned.warnings = {"One saved recording was missing"};
    warned.include_recordings = false;
    rig.coordinator->inspection_finished.emit(warned, "");
    BackupRig::settle();
    CHECK_THAT(text(page->inspectionLabel()),
               ContainsSubstring("Saved recordings and captures: not included"));
    CHECK_THAT(text(page->inspectionLabel()), ContainsSubstring("One saved recording was missing"));

    // The box defaults to No, and refusing it starts nothing.
    choices.restore = false;
    page->restoreButton()->click();
    CHECK(choices.confirmations == 1);
    CHECK_THAT(fv::qt::toUtf8(choices.confirmText),
               ContainsSubstring("Restore this backup and restart OpenWhisper_CPP?"));
    CHECK(choices.titles.back() == "Restore local data");
    rig.drain();
    CHECK(callsOf(rig, "restore").empty());
    choices.restore = true;
    page->restoreButton()->click();
    BackupRig::settle();
    CHECK(page->cancelButton()->isHidden());
    CHECK_FALSE(page->restoreButton()->isEnabled());
    rig.drain();
    CHECK(callsOf(rig, "restore") ==
          std::vector<FakeBackupService::Call>{{"restore", fv::path_to_utf8(archive)}});
    CHECK_THAT(text(page->statusLabel()), ContainsSubstring("restart"));
    CHECK(text(page->statusLabel()) == "Restore staged. OpenWhisper_CPP will restart to apply it.");

    page->inspectButton()->click();
    BackupRig::settle();
    rig.coordinator->inspection_finished.emit(std::nullopt, "Bad archive");
    BackupRig::settle();
    CHECK_FALSE(page->restoreButton()->isEnabled());
    CHECK_THAT(text(page->inspectionLabel()), ContainsSubstring("Bad archive"));
    CHECK(text(page->statusLabel()) == "Could not inspect backup: Bad archive");
}

TEST_CASE("an inspection without a path keeps the path that was picked",
          "[test_settings_backup][widget]") {
    BackupRig rig;
    Choices choices;
    auto page = makeBackupPage(rig, choices);
    const fs::path archive = rig.file("picked.fvbackup");
    choices.open = BackupRig::text(archive);
    page->inspectButton()->click();
    BackupRig::settle();
    BackupInfo info = FakeBackupService::info_for(archive);
    info.path.clear();
    rig.coordinator->inspection_finished.emit(info, "");
    BackupRig::settle();
    CHECK(page->inspectedPath() == BackupRig::text(archive));
}

TEST_CASE("a restore request that is refused says why", "[test_settings_backup][widget]") {
    BackupRig rig;
    Choices choices;
    auto page = makeBackupPage(rig, choices);
    const fs::path archive = rig.file("refused.fvbackup");
    choices.open = BackupRig::text(archive);
    page->inspectButton()->click();
    rig.drain();
    REQUIRE(page->restoreButton()->isEnabled());
    rig.hooks.set_refusal("Quit the recording first.");
    choices.restore = true;
    page->restoreButton()->click();
    rig.drain();
    CHECK(text(page->statusLabel()) == "Restore could not be prepared: Quit the recording first.");
    CHECK(page->restoreButton()->isEnabled());
    CHECK(callsOf(rig, "restore").empty());
}

// from tests/test_settings_backup.py::test_automatic_backup_is_off_until_explicitly_saved
TEST_CASE("test_automatic_backup_is_off_until_explicitly_saved", "[test_settings_backup][widget]") {
    BackupRig rig;
    Choices choices;
    auto page = makeBackupPage(rig, choices);
    CHECK(page->frequency() == "off");
    CHECK_FALSE(page->destinationEdit()->isEnabled());
    CHECK(rig.coordinator->snapshot()["frequency"] == "off");

    page->frequencyCombo()->setCurrentIndex(page->frequencyCombo()->findData("daily"));
    CHECK(page->destinationEdit()->isEnabled());
    page->saveScheduleButton()->click();
    CHECK(rig.coordinator->snapshot()["frequency"] == "off");
    CHECK_THAT(text(page->statusLabel()), ContainsSubstring("destination folder"));

    const fs::path folder = rig.temp.path() / "backups";
    fs::create_directories(folder);
    choices.folder = BackupRig::text(folder);
    page->destinationButton()->click();
    CHECK(choices.titles.back() == "Choose automatic backup folder");
    page->retentionSpin()->setValue(5);
    CHECK(rig.coordinator->snapshot()["frequency"] == "off");
    page->saveScheduleButton()->click();
    BackupRig::settle();
    const Json saved = rig.coordinator->snapshot();
    CHECK(saved["frequency"] == "daily");
    CHECK(saved["retention_count"] == 5);
    CHECK(saved["destination"] == fv::path_to_utf8(fs::weakly_canonical(folder)));
    CHECK(text(page->statusLabel()) == "Automatic backups set to daily.");

    page->frequencyCombo()->setCurrentIndex(page->frequencyCombo()->findData("off"));
    page->saveScheduleButton()->click();
    BackupRig::settle();
    CHECK(rig.coordinator->snapshot()["frequency"] == "off");
    CHECK(text(page->statusLabel()) == "Automatic backups are off.");
    CHECK_THAT(text(page->scheduleSummaryLabel()), ContainsSubstring("off"));
}

// from tests/test_settings_backup.py::test_schedule_rejects_missing_and_recording_destinations
TEST_CASE("test_schedule_rejects_missing_and_recording_destinations",
          "[test_settings_backup][widget]") {
    BackupRig rig;
    Choices choices;
    auto page = makeBackupPage(rig, choices);
    page->frequencyCombo()->setCurrentIndex(page->frequencyCombo()->findData("weekly"));
    page->destinationEdit()->setText(BackupRig::text(rig.temp.path() / "missing"));
    page->saveScheduleButton()->click();
    CHECK(rig.coordinator->snapshot()["frequency"] == "off");
    CHECK_THAT(text(page->statusLabel()), ContainsSubstring("existing folder"));
    CHECK(text(page->statusLabel()) ==
          "The automatic backup destination must be an existing folder.");

    const fs::path nested = rig.data / "recordings" / "backups";
    fs::create_directories(nested);
    page->destinationEdit()->setText(BackupRig::text(nested));
    page->saveScheduleButton()->click();
    CHECK(rig.coordinator->snapshot()["frequency"] == "off");
    CHECK_THAT(text(page->statusLabel()), ContainsSubstring("outside"));
    CHECK(text(page->statusLabel()) ==
          "Choose a folder outside OpenWhisper_CPP's recordings, meetings, and captures folders.");
    for (const char* owned : {"meetings", "captures"}) {
        const fs::path folder = rig.data / owned;
        fs::create_directories(folder);
        page->destinationEdit()->setText(BackupRig::text(folder));
        page->saveScheduleButton()->click();
        CHECK_THAT(text(page->statusLabel()), ContainsSubstring("outside"));
    }
    // The coordinator's own refusals reach the status line.
    fs::create_directories(rig.data / ".openwhisper-cpp-restore");
    page->destinationEdit()->setText(BackupRig::text(rig.data / ".openwhisper-cpp-restore"));
    page->saveScheduleButton()->click();
    CHECK(text(page->statusLabel()) ==
          "Could not save automatic backup settings: Choose a backup folder outside "
          "OpenWhisper_CPP's recording folders.");
    CHECK(rig.coordinator->snapshot()["frequency"] == "off");
}

// from tests/test_settings_backup.py::test_calendar_shows_saved_schedule_and_latest_backup_only
TEST_CASE("test_calendar_shows_saved_schedule_and_latest_backup_only",
          "[test_settings_backup][widget]") {
    BackupRig weekly(weeklyPreferences("C:/Backups"));
    Choices choices;
    auto page = makeBackupPage(weekly, choices);
    BackupCalendar& calendar = *page->calendar();
    const Day due = day(2026, 10, 11);
    const Day latest = day(2026, 10, 4);
    CHECK(calendar.grid()->latest() == latest);
    CHECK(calendar.grid()->isPlanned(due));
    CHECK(calendar.grid()->isPlanned(day(2026, 10, 18)));
    CHECK_FALSE(calendar.grid()->isPlanned(day(2026, 10, 4)));
    CHECK_FALSE(calendar.grid()->isPlanned(day(2026, 10, 12)));

    // An unsaved choice moves nothing.
    page->frequencyCombo()->setCurrentIndex(page->frequencyCombo()->findData("daily"));
    CHECK_FALSE(calendar.grid()->isPlanned(day(2026, 10, 12)));
    calendar.selectDay(due);
    CHECK_THAT(text(calendar.detailLabel()), ContainsSubstring("Automatic backup planned"));
    calendar.selectDay(latest);
    CHECK_THAT(text(calendar.detailLabel()), ContainsSubstring("Latest backup saved"));
    calendar.showMonth(MonthKey{2026, 10});
    calendar.nextButton()->click();
    CHECK(calendar.view() == MonthKey{2026, 11});
    calendar.todayButton()->click();
    CHECK(calendar.selectedDate() == latest);
    CHECK(calendar.view() == MonthKey{2026, 10});
    CHECK(callsOf(weekly, "backup").empty());
    CHECK(text(page->scheduleSummaryLabel()) == "Next backup: Oct 11, 2026 at 12:00 PM");

    CHECK(weekly.coordinator->set_schedule("off", 7, {}).empty());
    BackupRig::settle();
    CHECK_FALSE(calendar.grid()->isPlanned(due));
    CHECK_THAT(text(page->scheduleSummaryLabel()), ContainsSubstring("off"));
    // The last backup stays on the calendar after a schedule change, and a new one replaces it.
    CHECK(calendar.grid()->latest() == latest);
    const fs::path next = weekly.file("new.fvbackup");
    BackupInfo fresh = FakeBackupService::info_for(next);
    fresh.created_at = "2026-10-06T08:00:00Z";
    weekly.coordinator->backup_finished.emit(fresh, "");
    BackupRig::settle();
    CHECK(calendar.grid()->latest() == day(2026, 10, 6));
}

TEST_CASE("the schedule summary says when the next backup is due",
          "[test_settings_backup][widget]") {
    SECTION("no date yet") {
        BackupRig weekly(weeklyPreferences("C:/Backups"));
        Choices choices;
        auto page = makeBackupPage(weekly, choices);
        // A week after the last automatic run, which is still ahead.
        CHECK(text(page->scheduleSummaryLabel()).starts_with("Next backup: Oct 11, 2026"));
    }
    SECTION("due now") {
        Json prefs = weeklyPreferences("C:/Backups");
        // The last automatic backup was a week and a day ago.
        prefs["last_auto_at"] = 1791115200 - 8 * 86400;
        BackupRig overdue(prefs);
        Choices choices;
        auto page = makeBackupPage(overdue, choices);
        CHECK(text(page->scheduleSummaryLabel()) == "Next backup: when the app is idle");
    }
    SECTION("off") {
        BackupRig rig;
        Choices choices;
        auto page = makeBackupPage(rig, choices);
        CHECK(text(page->scheduleSummaryLabel()) == "Automatic backups are off");
        CHECK(page->calendar()->grid()->intervalDays() == 0);
    }
}

// from tests/test_settings_backup.py::test_calendar_title_opens_picker_and_keys_move_the_month
TEST_CASE("test_calendar_title_opens_picker_and_keys_move_the_month",
          "[test_settings_backup][widget]") {
    BackupRig rig;
    BackupCalendar calendar(rig.clock);
    calendar.showMonth(MonthKey{2026, 10});
    calendar.setDates(day(2026, 10, 4), day(2026, 10, 11), 7);

    calendar.openMonthPicker();
    fv::MonthPicker& picker = *calendar.monthPicker();
    CHECK(picker.isVisible());
    CHECK(picker.year() == 2026);
    CHECK(calendar.titleButton()->isOpen());
    picker.pick(2);
    CHECK_FALSE(picker.isVisible());
    CHECK_FALSE(calendar.titleButton()->isOpen());
    CHECK(calendar.view() == MonthKey{2026, 3});
    CHECK(calendar.titleButton()->accessibleName() == "March 2026");
    CHECK(calendar.previousButton()->accessibleName() == "Previous month");
    CHECK(calendar.todayButton()->accessibleName() == "Go to today");

    QTest::keyClick(calendar.grid(), Qt::Key_PageDown);
    CHECK(calendar.view() == MonthKey{2026, 4});
    calendar.selectDay(day(2026, 4, 29));
    QTest::keyClick(calendar.grid(), Qt::Key_Right);
    QTest::keyClick(calendar.grid(), Qt::Key_Right);
    CHECK(calendar.view() == MonthKey{2026, 5});
    CHECK(calendar.selectedDate() == day(2026, 5, 1));

    calendar.selectDay(day(2026, 10, 11));
    CHECK(calendar.view() == MonthKey{2026, 10});
    CHECK_THAT(text(calendar.detailLabel()), ContainsSubstring("planned"));
    calendar.showMonth(MonthKey{2026, 1});
    calendar.goToday();
    CHECK(calendar.view() == MonthKey{2026, 10});
    CHECK(calendar.selectedDate() == day(2026, 10, 4));
}

TEST_CASE("the calendar's texts and month tint", "[test_settings_backup][widget]") {
    BackupRig rig;
    BackupCalendar calendar(rig.clock);
    CHECK(calendar.grid()->accessibleName() == "Backup calendar");
    CHECK(calendar.detailLabel()->accessibleName() == "Selected backup date");
    CHECK(calendar.titleButton()->toolTip() == "Choose a month or year (M)");
    CHECK(text(calendar.detailLabel()) == "Oct 4 \xE2\x80\x94 No backup shown for this date");
    CHECK(calendar.grid()->firstWeekday() == fv::history::MONDAY);
    calendar.setDates(day(2026, 10, 4), day(2026, 10, 4), 1);
    CHECK(text(calendar.detailLabel()) ==
          "Oct 4 \xE2\x80\x94 Latest backup saved \xC2\xB7 Automatic backup planned");
    // The picker is tinted by the latest backup and a year of planned ones.
    const auto counts = calendar.monthPicker()->counts();
    CHECK(counts.at(MonthKey{2026, 10}) == 29);
    CHECK(counts.at(MonthKey{2027, 10}) == 5);
    CHECK_FALSE(counts.contains(MonthKey{2027, 11}));
}

namespace {

// test_backup_card_text_has_no_opaque_background_and_controls_fit for one theme and text size.
void cardTextFits(const std::string& theme, int scale) {
    AppTheme appTheme(QString::fromStdString(theme), scale);
    // A backup on record: the labels that carry its date, place and size.
    BackupRig recorded(Json{{"last_backup", Json{{"path", "C:\\Backups\\manual.fvbackup"},
                                                 {"created_at", "2026-10-04T12:30:00Z"},
                                                 {"archive_bytes", 2048}}}});
    Choices choices;
    auto page = makeBackupPage(recorded, choices);
    page->resize(860, 1000);
    page->show();
    for (int i = 0; i < 4; ++i) {
        QApplication::processEvents();
    }
    const QImage picture = page->grab().toImage();
    const QColor surface = fv::ui::tokenColor("slate-surface");
    for (QWidget* label : std::vector<QWidget*>{page->lastBackupLabel(), page->lastLocationLabel(),
                                                page->lastSizeLabel()}) {
        const QPoint point = label->mapTo(page.get(), QPoint(label->width() - 2, 2));
        INFO(label->objectName().toStdString());
        CHECK(picture.pixelColor(point) == surface);
    }

    // Every month, since the title is the month's name: "September" is far wider than "May".
    for (int month = 1; month <= 12; ++month) {
        INFO("month " << month);
        page->calendar()->showMonth(MonthKey{2026, month});
        page->resize(360, 1500);
        for (int i = 0; i < 4; ++i) {
            QApplication::processEvents();
        }
        page->resize(360, 1500);
        QApplication::processEvents();
        REQUIRE(page->width() == 360);
        CHECK(page->scheduleLayout()->direction() == QBoxLayout::TopToBottom);
        for (QWidget* control : std::vector<QWidget*>{
                 page->createButton(), page->inspectButton(), page->restoreButton(),
                 page->saveScheduleButton(), page->destinationEdit(), page->calendar()}) {
            INFO(control->objectName().toStdString());
            const int right = control->mapTo(page.get(), QPoint(control->width(), 0)).x();
            CHECK(right <= page->width());
        }
    }
}

} // namespace

// from
// tests/test_settings_backup.py::test_backup_card_text_has_no_opaque_background_and_controls_fit
TEST_CASE("test_backup_card_text_has_no_opaque_background_and_controls_fit dark 100",
          "[test_settings_backup][widget]") {
    cardTextFits("dark", 100);
}

TEST_CASE("test_backup_card_text_has_no_opaque_background_and_controls_fit light 100",
          "[test_settings_backup][widget]") {
    cardTextFits("light", 100);
}

TEST_CASE("test_backup_card_text_has_no_opaque_background_and_controls_fit dark 115",
          "[test_settings_backup][widget]") {
    cardTextFits("dark", 115);
}

TEST_CASE("test_backup_card_text_has_no_opaque_background_and_controls_fit dark 130",
          "[test_settings_backup][widget]") {
    cardTextFits("dark", 130);
}

TEST_CASE("the page lays out wide and stacks narrow", "[test_settings_backup][widget]") {
    AppTheme theme;
    BackupRig rig;
    Choices choices;
    auto page = makeBackupPage(rig, choices);
    page->resize(900, 1100);
    page->show();
    BackupRig::settle();
    CHECK(page->scheduleLayout()->direction() == QBoxLayout::LeftToRight);
    CHECK(page->createActions()->direction() == QBoxLayout::LeftToRight);
    CHECK(page->restoreActions()->direction() == QBoxLayout::LeftToRight);
    page->resize(679, 1500);
    BackupRig::settle();
    CHECK(page->scheduleLayout()->direction() == QBoxLayout::TopToBottom);
    page->resize(680, 1500);
    BackupRig::settle();
    CHECK(page->scheduleLayout()->direction() == QBoxLayout::LeftToRight);
    // Both action rows stack together when either cannot hold its controls.
    page->resize(360, 1500);
    BackupRig::settle();
    page->resize(360, 1500);
    BackupRig::settle();
    CHECK(page->createActions()->direction() == QBoxLayout::TopToBottom);
    CHECK(page->restoreActions()->direction() == QBoxLayout::TopToBottom);
}

TEST_CASE("the lock follows what the page is doing", "[test_settings_backup][widget]") {
    BackupRig rig;
    Choices choices;
    auto page = makeBackupPage(rig, choices);
    std::vector<bool> changes;
    QObject::connect(page.get(), &BackupSettingsPage::operationBusyChanged, page.get(),
                     [&](bool busy) { changes.push_back(busy); });
    choices.save = BackupRig::text(rig.temp.path() / "lock.fvbackup");
    page->createButton()->click();
    BackupRig::settle();
    // The coordinator's own report of the same state is not a change.
    CHECK(changes == std::vector<bool>{true});
    CHECK_FALSE(page->frequencyCombo()->isEnabled());
    CHECK_FALSE(page->includeRecordingsCheck()->isEnabled());
    CHECK_FALSE(page->saveScheduleButton()->isEnabled());
    CHECK_FALSE(page->inspectButton()->isEnabled());
    CHECK_FALSE(page->progressBar()->isHidden());
    // A second request while one runs does nothing.
    page->inspectButton()->click();
    CHECK(choices.titles.size() == 1);
    rig.drain();
    CHECK(changes == std::vector<bool>{true, false});
    CHECK(page->frequencyCombo()->isEnabled());
    CHECK(page->progressBar()->isHidden());
}

TEST_CASE("a request the coordinator refuses ends the lock with its reason",
          "[test_settings_backup][widget]") {
    BackupRig rig;
    Choices choices;
    auto page = makeBackupPage(rig, choices);
    rig.hooks.set_refusal("Stop recording first.");
    choices.save = BackupRig::text(rig.temp.path() / "refused.fvbackup");
    page->createButton()->click();
    rig.drain();
    CHECK(text(page->statusLabel()) == "Backup failed: Stop recording first.");
    CHECK(page->createButton()->isEnabled());
    CHECK_FALSE(rig.coordinator->busy());
}

TEST_CASE("a backup running in the coordinator locks a page that did not start it",
          "[test_settings_backup][widget]") {
    BackupRig rig;
    Choices choices;
    auto page = makeBackupPage(rig, choices);
    rig.coordinator->create_backup(rig.temp.path() / "scheduled.fvbackup", true);
    BackupRig::settle();
    CHECK_FALSE(page->createButton()->isEnabled());
    // The coordinator says which operation runs: a backup can be cancelled.
    CHECK_FALSE(page->cancelButton()->isHidden());
    rig.drain();
    CHECK(page->createButton()->isEnabled());
    CHECK(page->cancelButton()->isHidden());
}

TEST_CASE("the import button follows what was found", "[test_settings_backup][widget]") {
    BackupRig rig;
    Choices choices;
    FakeOffer offer;
    auto page = makeBackupPage(rig, choices, &offer);
    CHECK(offer.refreshes == 1);
    CHECK(page->importButton()->isHidden());
    CHECK(fv::qt::toUtf8(page->importButton()->text()) == "Import from OpenWhisper\xE2\x80\xA6");
    offer.find(true);
    CHECK_FALSE(page->importButton()->isHidden());
    CHECK(page->importButton()->isEnabled());
    page->importButton()->click();
    CHECK(offer.opened == std::vector<QWidget*>{page.get()});
    offer.find(false);
    CHECK(page->importButton()->isHidden());

    // Found before the page was built, the button is there at once.
    offer.found = true;
    auto second = makeBackupPage(rig, choices, &offer);
    CHECK_FALSE(second->importButton()->isHidden());

    // It is locked with the rest while an operation runs.
    choices.save = BackupRig::text(rig.temp.path() / "busy.fvbackup");
    second->createButton()->click();
    BackupRig::settle();
    CHECK_FALSE(second->importButton()->isEnabled());
    rig.drain();
    CHECK(second->importButton()->isEnabled());
}

TEST_CASE("the action rows stack when the import button appears where it does not fit",
          "[test_settings_backup][widget]") {
    AppTheme theme;
    BackupRig rig;
    Choices choices;
    FakeOffer offer;
    auto page = makeBackupPage(rig, choices, &offer);
    page->show();
    BackupRig::settle();
    // _fits_side_by_side's measure of a control.
    const auto widthOf = [](const QWidget* widget) {
        return std::max(widget->sizeHint().width(), widget->minimumWidth());
    };
    const int restoreGap = page->restoreActions()->spacing();
    const int restoreNeeds =
        widthOf(page->inspectButton()) + widthOf(page->restoreButton()) + restoreGap;
    const int withImport = restoreNeeds + widthOf(page->importButton()) + restoreGap;
    const int createNeeds = widthOf(page->includeRecordingsCheck()) +
                            widthOf(page->createButton()) + page->createActions()->spacing();
    // A page width whose rows hold their two controls each but not the import button too.
    bool found = false;
    for (int width = 900; width >= 360 && !found; width -= 5) {
        page->resize(width, 1500);
        BackupRig::settle();
        const int row = page->restoreRow()->width();
        found =
            page->createRow()->width() >= createNeeds && row >= restoreNeeds && row < withImport;
    }
    REQUIRE(found);
    CHECK(page->restoreActions()->direction() == QBoxLayout::LeftToRight);
    CHECK(page->createActions()->direction() == QBoxLayout::LeftToRight);

    offer.find(true);
    CHECK(page->restoreActions()->direction() == QBoxLayout::TopToBottom);
    CHECK(page->createActions()->direction() == QBoxLayout::TopToBottom);
    offer.find(false);
    CHECK(page->restoreActions()->direction() == QBoxLayout::LeftToRight);
    CHECK(page->createActions()->direction() == QBoxLayout::LeftToRight);
}

TEST_CASE("without OpenWhisper data the page has no import button to show",
          "[test_settings_backup][widget]") {
    BackupRig rig;
    Choices choices;
    auto page = makeBackupPage(rig, choices);
    CHECK(page->importButton()->isHidden());
    page->show();
    BackupRig::settle();
    CHECK(page->importButton()->isHidden());
}

// from tests/test_settings_backup.py::test_backup_page_is_lazy_searchable_and_usable_at_narrow_size
TEST_CASE("test_backup_page_is_lazy_searchable_and_usable_at_narrow_size",
          "[test_settings_backup][widget]") {
    AppTheme theme;
    BackupRig backup;
    fvtest::HomeRig rig(Json::object(), {dest::backup});
    rig.context().provide<fv::backup::BackupCoordinator>(backup.coordinator);
    fv::registerBackupPage(rig.context());
    fv::SettingsDialog& dialog = rig.dialog();

    CHECK_FALSE(dialog.isBuilt(dest::backup));
    const auto index = fv::buildSearchIndex(dialog);
    CHECK(std::ranges::any_of(index, [](const fv::appcore::SearchEntry& entry) {
        return entry.destination == dest::backup && entry.title == "Schedule";
    }));
    CHECK(dialog.rail()->value(QStringLiteral("backup")) == "Local backups");
    dialog.show();
    dialog.resize(fv::SettingsDialog::kMinimumSize);
    dialog.selectDestination(QStringLiteral("backup"));
    BackupRig::settle();
    REQUIRE(dialog.isBuilt(dest::backup));
    auto* page = dynamic_cast<BackupSettingsPage*>(dialog.pageContent(dest::backup));
    REQUIRE(page != nullptr);
    CHECK(dialog.pageTitle(dest::backup) == "Backup & restore");
    CHECK(dialog.pageTitleLabel()->text() == "Backup & restore");
    CHECK(dialog.rail()->value(QStringLiteral("backup")) == "Off");
    for (QWidget* control : std::vector<QWidget*>{page->createButton(), page->inspectButton(),
                                                  page->saveScheduleButton()}) {
        INFO(control->objectName().toStdString());
        CHECK(control->width() > 0);
        CHECK(control->isVisibleTo(&dialog));
        CHECK(control->mapTo(page, QPoint(control->width(), 0)).x() <= page->width());
    }

    page->setBusy(true);
    CHECK_FALSE(dialog.rail()->isEnabled());
    CHECK_FALSE(dialog.searchButton()->isEnabled());
    CHECK_FALSE(dialog.pageMenuButton()->isEnabled());
    dialog.selectDestination(QStringLiteral("general"));
    CHECK(dialog.rail()->currentKey() == "backup");
    dialog.openSearch();
    CHECK_FALSE(rig.palette().isVisible());
    page->setBusy(false);
    CHECK(dialog.rail()->isEnabled());
    CHECK(dialog.searchButton()->isEnabled());
    dialog.close();
}

TEST_CASE("the page registers its rail value, controls and search copy", "[widget]") {
    BackupRig backup;
    fvtest::HomeRig rig(Json::object(), {dest::backup});
    fv::registerBackupPage(rig.context());
    const fv::SettingsPageSpec* spec = rig.context().settingsPages().page(dest::backup);
    REQUIRE(spec != nullptr);
    CHECK(spec->title == "Backup & restore");
    CHECK(spec->subtitle ==
          "Save a local copy of your data or inspect a backup before restoring it.");
    CHECK(spec->scroll);
    CHECK(spec->controls == std::vector<std::string>{"backup_page", "backup_create_tile",
                                                     "backup_restore_tile",
                                                     "backup_schedule_tile"});
    CHECK(rig.context().settingsPages().controlDestination("backup_schedule_tile") == dest::backup);
    REQUIRE(spec->searchFields.size() == 3);
    CHECK(spec->searchFields[0].label == "Back up local data");
    CHECK(spec->searchFields[1].label == "Inspect and restore");
    CHECK(spec->searchFields[2].label == "Schedule");

    // Without a coordinator the page is built all the same, and every action is off.
    fv::SettingsDialog& dialog = rig.dialog();
    dialog.ensurePage(dest::backup);
    auto* page = dynamic_cast<BackupSettingsPage*>(dialog.pageContent(dest::backup));
    REQUIRE(page != nullptr);
    CHECK_FALSE(page->createButton()->isEnabled());
    CHECK(dialog.control("backup_create_tile") == page->createTile());
    CHECK(dialog.control("backup_restore_tile") == page->restoreTile());
    CHECK(dialog.control("backup_schedule_tile") == page->scheduleTile());
    CHECK(dialog.control("backup_page") == page);
}

TEST_CASE("the dialog locks while the coordinator works, built page or not", "[widget]") {
    BackupRig backup;
    fvtest::HomeRig rig(Json::object(), {dest::backup});
    rig.context().provide<fv::backup::BackupCoordinator>(backup.coordinator);
    fv::registerBackupPage(rig.context());
    fv::SettingsDialog& dialog = rig.dialog();
    fv::followBackupBusy(rig.context(), dialog);
    CHECK_FALSE(dialog.isBackupBusy());
    CHECK_FALSE(dialog.isBuilt(dest::backup));

    backup.coordinator->create_backup(backup.temp.path() / "auto.fvbackup", true);
    BackupRig::settle();
    CHECK(dialog.isBackupBusy());
    CHECK(dialog.rail()->currentKey() == "backup");
    CHECK_FALSE(dialog.rail()->isEnabled());
    CHECK(dialog.isBuilt(dest::backup));
    backup.drain();
    CHECK_FALSE(dialog.isBackupBusy());
    CHECK(dialog.rail()->isEnabled());

    // A window opened in the middle of one starts locked.
    backup.coordinator->create_backup(backup.temp.path() / "second.fvbackup", true);
    fv::SettingsDialog& reopened = rig.reopen();
    fv::followBackupBusy(rig.context(), reopened);
    CHECK(reopened.isBackupBusy());
    backup.drain();
    CHECK_FALSE(reopened.isBackupBusy());
}
