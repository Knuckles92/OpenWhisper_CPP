#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <QObject>
#include <QString>
#include <QWidget>

#include "appkit/settings_page.h"
#include "backup/backup.h"
#include "base/clock.h"
#include "base/json.h"
#include "base/subscription.h"
#include "pyport/datetime.h"

class QBoxLayout;
class QCheckBox;
class QLineEdit;
class QProgressBar;
class QSpinBox;

namespace fv {

class AppContext;
class BackupCalendar;
class Button;
class ElidingComboBox;
class ElidingLabel;
class InfoTile;
class WrappedLabel;

namespace backup {
class BackupCoordinator;
}

// ui_qt/dialogs/settings_backup.py's helpers over the clock's zone. Both read the stored
// timestamp as Python does: "Z" is "+00:00", an aware value becomes local time, a naive one is
// local already.
namespace backup_page {

// _local_date: the value as naive local wall time, or nullopt when it is empty or not an ISO
// timestamp (or leaves years 1..9999 once converted).
std::optional<py::DateTime> local_date(const Json& value, const Clock& clock);
// _date_label: "Oct 4, 2026 at 12:30 PM". A value that is not a timestamp is shown as it is,
// an empty one as UNKNOWN_DATE.
std::string date_label(const Json& value, const Clock& clock);
// The label of a local wall time.
std::string date_label(const py::DateTime& local);

} // namespace backup_page

// The OpenWhisper import the Backup page offers. ui_setup, which owns the import window, depends
// on ui_settings, so the page reaches it through this interface: whoever builds the window
// provides it (ctx.service<BackupImportOffer>()). Affinity: GUI thread.
class BackupImportOffer : public QObject {
    Q_OBJECT
public:
    using QObject::QObject;

    // An OpenWhisper data folder was found by the last look.
    virtual bool available() const = 0;
    // Looks again, off the GUI thread; availabilityChanged follows when available() changes.
    virtual void refresh() = 0;
    // Shows the import window on `parent`.
    virtual void open(QWidget* parent) = 0;

Q_SIGNALS:
    void availabilityChanged(bool available);
};

// The Settings > Backup & restore page (BackupSettingsPage, object name "backupSettingsPage"):
// it collects choices, confirms replacement and renders the coordinator's signals; the
// application owns the coordinator and its worker. Without a coordinator every action is off and
// the status says so. Every coordinator signal reaches the page through qt::post, so the page
// changes on the GUI thread whatever thread emitted it.
// Affinity: GUI thread.
class BackupSettingsPage : public QWidget, public SettingsPageModule {
    Q_OBJECT
public:
    // The user's choices, replaceable by a test; the defaults are Qt's own dialogs. Each takes
    // the parent, the title and the starting folder or file; the file ones also the filter.
    struct Dialogs {
        std::function<QString(QWidget*, const QString&, const QString&, const QString&)> saveFile;
        std::function<QString(QWidget*, const QString&, const QString&, const QString&)> openFile;
        std::function<QString(QWidget*, const QString&, const QString&)> chooseFolder;
        // The restore confirmation (Yes or No, defaulting to No): the title and the text.
        std::function<bool(QWidget*, const QString&, const QString&)> confirm;
    };

    struct Options {
        // Today and the zone of every date shown; the system's when null. Outlives the page.
        const Clock* clock = nullptr;
        // The data folder whose recordings, meetings and captures folders an automatic backup
        // may not be saved in; empty to skip that check.
        std::filesystem::path dataRoot;
        Dialogs dialogs;
        // The OpenWhisper import (null: no import button). Outlives the page.
        BackupImportOffer* import = nullptr;
    };

    // Two constructors, not a default argument: GCC cannot value-initialise Options, whose
    // members have initialisers, before the enclosing class is complete.
    explicit BackupSettingsPage(backup::BackupCoordinator* coordinator);
    BackupSettingsPage(backup::BackupCoordinator* coordinator, Options options,
                       QWidget* parent = nullptr);
    ~BackupSettingsPage() override;

    // Renders the coordinator's latest persisted status and schedule.
    void refresh();
    // set_busy: locks the controls, and tells Settings through operationBusyChanged when the
    // state changed.
    void setBusy(bool busy);
    // set_progress: phase with done of total as a percentage; no total is indeterminate.
    void setProgress(const std::string& phase, int done, int total);
    void showBackupResult(const std::optional<backup::BackupInfo>& info,
                          const std::string& error = {});
    void showInspectionResult(const std::optional<backup::BackupInfo>& info,
                              const std::string& error = {});
    void showRestoreResult(const std::string& stagingPath, const std::string& error = {});

    bool isBusy() const { return busy_; }
    // The frequency chosen in the combo ("off", "daily" or "weekly").
    QString frequency() const;
    // The inspected backup a restore would use; "" before a successful inspection.
    const QString& inspectedPath() const { return inspectedPath_; }

    // SettingsPageModule
    void refreshPage() override { refresh(); }
    // "backup_page" and the three tiles ("backup_create_tile", "backup_restore_tile",
    // "backup_schedule_tile").
    QWidget* namedControl(std::string_view name) override;

    InfoTile* createTile() const { return createTile_; }
    InfoTile* restoreTile() const { return restoreTile_; }
    InfoTile* scheduleTile() const { return scheduleTile_; }
    WrappedLabel* lastBackupLabel() const { return lastBackupLabel_; }
    ElidingLabel* lastLocationLabel() const { return lastLocationLabel_; }
    WrappedLabel* lastSizeLabel() const { return lastSizeLabel_; }
    WrappedLabel* privacyLabel() const { return privacyLabel_; }
    QCheckBox* includeRecordingsCheck() const { return includeRecordings_; }
    QWidget* createRow() const { return createRow_; }
    QBoxLayout* createActions() const { return createActions_; }
    Button* createButton() const { return createButton_; }
    QWidget* restoreRow() const { return restoreRow_; }
    QBoxLayout* restoreActions() const { return restoreActions_; }
    Button* inspectButton() const { return inspectButton_; }
    Button* restoreButton() const { return restoreButton_; }
    Button* importButton() const { return importButton_; }
    WrappedLabel* inspectionLabel() const { return inspectionLabel_; }
    ElidingLabel* inspectionLocationLabel() const { return inspectionLocationLabel_; }
    ElidingLabel* recoveryLocationLabel() const { return recoveryLocationLabel_; }
    QBoxLayout* scheduleLayout() const { return scheduleLayout_; }
    WrappedLabel* scheduleSummaryLabel() const { return scheduleSummaryLabel_; }
    ElidingComboBox* frequencyCombo() const { return frequencyCombo_; }
    QLineEdit* destinationEdit() const { return destinationEdit_; }
    Button* destinationButton() const { return destinationButton_; }
    QSpinBox* retentionSpin() const { return retentionSpin_; }
    Button* saveScheduleButton() const { return saveScheduleButton_; }
    BackupCalendar* calendar() const { return calendar_; }
    QProgressBar* progressBar() const { return progressBar_; }
    Button* cancelButton() const { return cancelButton_; }
    WrappedLabel* statusLabel() const { return statusLabel_; }

Q_SIGNALS:
    // The state of Settings' backup lock: a backup, inspection or restore started or ended.
    void operationBusyChanged(bool busy);

protected:
    void resizeEvent(QResizeEvent* event) override;

private:
    // What a BackupInfo or the coordinator's last_backup object says (_field with its defaults).
    struct Facts {
        std::string path;
        Json createdAt;
        std::string appVersion = "?";
        bool includeRecordings = false;
        std::uint64_t fileCount = 0;
        std::uint64_t totalBytes = 0;
        std::uint64_t archiveBytes = 0;
        std::vector<std::string> warnings;

        static Facts from(const backup::BackupInfo& info);
        static Facts from(const Json& info);
    };

    void connectCoordinator();
    // resizeEvent's rules: the schedule stacks below 680 px, both action rows once either cannot
    // hold its visible controls side by side.
    void updateDirections();
    void showImportButton(bool shown);
    void updateCalendar();
    void showLastBackup(const std::optional<Facts>& info);
    void updateScheduleControls();
    void updateActions();
    void chooseBackupDestination();
    void chooseRestoreSource();
    void confirmRestore();
    void chooseScheduleDestination();
    void saveSchedule();
    void start(const QString& operation, const std::function<void()>& request);
    void cancel();
    void setStatus(const std::string& text);

    backup::BackupCoordinator* coordinator_;
    Options options_;
    const Clock* clock_;

    QString inspectedPath_;
    QString pendingInspectionPath_;
    bool busy_ = false;
    std::string operation_;
    std::optional<Facts> lastBackup_;
    std::optional<double> nextBackupAt_;
    std::string savedFrequency_ = "off";

    InfoTile* createTile_ = nullptr;
    InfoTile* restoreTile_ = nullptr;
    InfoTile* scheduleTile_ = nullptr;
    WrappedLabel* lastBackupLabel_ = nullptr;
    ElidingLabel* lastLocationLabel_ = nullptr;
    WrappedLabel* lastSizeLabel_ = nullptr;
    WrappedLabel* privacyLabel_ = nullptr;
    QCheckBox* includeRecordings_ = nullptr;
    QWidget* createRow_ = nullptr;
    QBoxLayout* createActions_ = nullptr;
    Button* createButton_ = nullptr;
    QWidget* restoreRow_ = nullptr;
    QBoxLayout* restoreActions_ = nullptr;
    Button* inspectButton_ = nullptr;
    Button* restoreButton_ = nullptr;
    Button* importButton_ = nullptr;
    WrappedLabel* inspectionLabel_ = nullptr;
    ElidingLabel* inspectionLocationLabel_ = nullptr;
    ElidingLabel* recoveryLocationLabel_ = nullptr;
    QBoxLayout* scheduleLayout_ = nullptr;
    WrappedLabel* scheduleSummaryLabel_ = nullptr;
    ElidingComboBox* frequencyCombo_ = nullptr;
    QLineEdit* destinationEdit_ = nullptr;
    Button* destinationButton_ = nullptr;
    QSpinBox* retentionSpin_ = nullptr;
    Button* saveScheduleButton_ = nullptr;
    BackupCalendar* calendar_ = nullptr;
    QProgressBar* progressBar_ = nullptr;
    Button* cancelButton_ = nullptr;
    WrappedLabel* statusLabel_ = nullptr;

    std::vector<Subscription> subscriptions_;
};

// Registers the Backup & restore page with context.settingsPages() under destinations::backup:
// its rail value ("Local backups" until the page is built, then "Off", "Daily" or "Weekly"),
// search fields, controls and the lock of Settings' navigation while an operation runs. The page
// uses the context's backup::BackupCoordinator and BackupImportOffer when they are provided
// (checked when the page is built).
void registerBackupPage(AppContext& context);

// Keeps `dialog`'s backup lock in step with the context's backup::BackupCoordinator, as the
// window does when it is built with one: Backup is selected and navigation locked while a backup
// or restore runs, even when the page is not built or the schedule started it. Does nothing when
// the context provides no coordinator.
class SettingsDialog;
void followBackupBusy(AppContext& context, SettingsDialog& dialog);

} // namespace fv
