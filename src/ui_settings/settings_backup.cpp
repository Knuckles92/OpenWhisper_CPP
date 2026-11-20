#include "ui_settings/settings_backup.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <exception>
#include <format>
#include <memory>
#include <utility>

#include <QBoxLayout>
#include <QCheckBox>
#include <QFileDialog>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPointer>
#include <QProgressBar>
#include <QSizePolicy>
#include <QSpinBox>
#include <QVBoxLayout>

#include "appkit/app_context.h"
#include "appkit/destinations.h"
#include "appkit/qt_bridge.h"
#include "appkit/registries.h"
#include "backup/coordinator.h"
#include "backup/names.h"
#include "base/identity.h"
#include "base/log.h"
#include "base/utf8.h"
#include "history/calendar.h"
#include "history/format_utils.h"
#include "pyport/errors.h"
#include "pyport/number.h"
#include "pyport/text.h"
#include "ui_settings/backup_calendar.h"
#include "ui_settings/settings_dialog.h"
#include "ui_settings/settings_fields.h"
#include "ui_settings/settings_page.h"
#include "ui_theme/icons.h"
#include "ui_theme/style_props.h"
#include "ui_widgets/buttons.h"
#include "ui_widgets/eliding_label.h"
#include "ui_widgets/no_wheel.h"
#include "ui_widgets/setting_tile.h"
#include "ui_widgets/wrapped_label.h"

namespace fv {
namespace {

constexpr log::Logger kLog{"ui_qt.dialogs.settings_backup"};

using history::Day;

QString q(std::string_view utf8) {
    return qt::toQString(utf8);
}

std::string product() {
    return std::string(identity().product);
}

// Runs the signal's callback where it was emitted, so that the page's own handler can queue
// itself onto the GUI thread (qt::post). Whatever thread emits, the callback never waits.
class InlineExecutor final : public Executor {
public:
    void post(Task task) override { task(); }
    TimerHandle post_after(std::chrono::milliseconds, Task) override { return {}; }
    bool is_current() const override { return true; }
};

Executor& inlineExecutor() {
    static InlineExecutor executor;
    return executor;
}

// Python truthiness of a loosely typed value.
bool truthy(const Json& value) {
    if (value.is_null()) {
        return false;
    }
    if (value.is_boolean()) {
        return value.get<bool>();
    }
    if (value.is_number()) {
        return value.get<double>() != 0;
    }
    if (value.is_string()) {
        return !value.get<std::string>().empty();
    }
    return !value.empty();
}

// str(value) of a value that is not text.
std::string pyStr(const Json& value) {
    if (value.is_string()) {
        return value.get<std::string>();
    }
    if (value.is_boolean()) {
        return value.get<bool>() ? "True" : "False";
    }
    if (value.is_null()) {
        return "None";
    }
    return value.dump();
}

std::string stringField(const Json& info, const char* name, std::string fallback) {
    const auto found = info.find(name);
    if (found == info.end() || found->is_null()) {
        return fallback;
    }
    return pyStr(*found);
}

std::uint64_t countField(const Json& info, const char* name) {
    const auto found = info.find(name);
    if (found == info.end() || !found->is_number() || found->get<double>() < 0) {
        return 0;
    }
    return found->is_number_unsigned() ? found->get<std::uint64_t>()
                                       : static_cast<std::uint64_t>(found->get<double>());
}

std::string joined(const std::vector<std::string>& items) {
    std::string text;
    for (const std::string& item : items) {
        text += text.empty() ? "" : "; ";
        text += item;
    }
    return text;
}

Day dayOf(const py::DateTime& when) {
    return Day{std::chrono::year{when.year} /
               std::chrono::month{static_cast<unsigned>(when.month)} /
               std::chrono::day{static_cast<unsigned>(when.day)}};
}

// datetime.fromtimestamp(seconds).astimezone(): the moment as local wall time, or nullopt where
// Python raises (a time before 1970 or past year 9999, which also covers NaN and infinities).
std::optional<py::DateTime> localFromEpoch(double seconds, const Clock& clock) {
    constexpr double kEndOfYear9999 = 253402300800.0;
    if (!(seconds >= 0) || !(seconds < kEndOfYear9999)) {
        return std::nullopt;
    }
    const std::chrono::sys_seconds instant{
        std::chrono::seconds{static_cast<std::int64_t>(std::floor(seconds))}};
    const auto local = std::chrono::zoned_time{clock.zone(), instant}.get_local_time();
    const auto day = std::chrono::floor<std::chrono::days>(local);
    const std::chrono::year_month_day date{day};
    const std::chrono::hh_mm_ss<std::chrono::seconds> time{
        std::chrono::duration_cast<std::chrono::seconds>(local - day)};
    py::DateTime moment;
    moment.year = static_cast<int>(date.year());
    moment.month = static_cast<int>(static_cast<unsigned>(date.month()));
    moment.day = static_cast<int>(static_cast<unsigned>(date.day()));
    moment.hour = static_cast<int>(time.hours().count());
    moment.minute = static_cast<int>(time.minutes().count());
    moment.second = static_cast<int>(time.seconds().count());
    return moment;
}

// _action_row: a holder for a row of controls that never holds the page wider. Its width comes
// from the tile, so a narrow window stacks the controls instead of growing a horizontal scroll
// bar.
QWidget* actionRow() {
    auto* row = new QWidget;
    row->setObjectName(QStringLiteral("backupActionRow"));
    row->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    return row;
}

// _fits_side_by_side: whether `row` is wide enough for its visible controls in one line.
bool fitsSideBySide(const QWidget& row, const QBoxLayout& layout) {
    int needed = 0;
    int count = 0;
    for (int index = 0; index < layout.count(); ++index) {
        const QWidget* widget = layout.itemAt(index)->widget();
        if (widget == nullptr || widget->isHidden()) {
            continue;
        }
        needed += std::max(widget->sizeHint().width(), widget->minimumWidth());
        ++count;
    }
    needed += layout.spacing() * std::max(0, count - 1);
    return row.width() >= needed;
}

// _field_row: a control under its caption, which is the control's buddy.
QWidget* fieldRow(const QString& caption, QWidget* control) {
    auto* row = new QWidget;
    row->setObjectName(QStringLiteral("backupFieldGroup"));
    auto* line = new QVBoxLayout(row);
    line->setContentsMargins(0, 0, 0, 0);
    line->setSpacing(5);
    auto* label = new QLabel(caption);
    label->setObjectName(QStringLiteral("settingsTileFieldLabel"));
    label->setBuddy(control);
    line->addWidget(label);
    line->addWidget(control, 1);
    return row;
}

// Whether `folder` is `owned` or lies below it (Path.is_relative_to), by whole components. Names
// differing only in case are the same folder on Windows.
bool isRelativeTo(const std::filesystem::path& folder, const std::filesystem::path& owned) {
    const auto same = [](const std::filesystem::path& a, const std::filesystem::path& b) {
#ifdef _WIN32
        return QString::compare(q(path_to_utf8(a)), q(path_to_utf8(b)), Qt::CaseInsensitive) == 0;
#else
        return a == b;
#endif
    };
    auto inside = folder.begin();
    for (auto base = owned.begin(); base != owned.end(); ++base, ++inside) {
        if (inside == folder.end() || !same(*inside, *base)) {
            return false;
        }
    }
    return true;
}

BackupSettingsPage::Dialogs withDefaults(BackupSettingsPage::Dialogs dialogs) {
    if (!dialogs.saveFile) {
        dialogs.saveFile = [](QWidget* parent, const QString& title, const QString& start,
                              const QString& filter) {
            return QFileDialog::getSaveFileName(parent, title, start, filter);
        };
    }
    if (!dialogs.openFile) {
        dialogs.openFile = [](QWidget* parent, const QString& title, const QString& start,
                              const QString& filter) {
            return QFileDialog::getOpenFileName(parent, title, start, filter);
        };
    }
    if (!dialogs.chooseFolder) {
        dialogs.chooseFolder = [](QWidget* parent, const QString& title, const QString& start) {
            return QFileDialog::getExistingDirectory(parent, title, start);
        };
    }
    if (!dialogs.confirm) {
        dialogs.confirm = [](QWidget* parent, const QString& title, const QString& text) {
            return QMessageBox::question(parent, title, text, QMessageBox::Yes | QMessageBox::No,
                                         QMessageBox::No) == QMessageBox::Yes;
        };
    }
    return dialogs;
}

} // namespace

namespace backup_page {

std::optional<py::DateTime> local_date(const Json& value, const Clock& clock) {
    if (!truthy(value)) {
        return std::nullopt;
    }
    std::string text = pyStr(value);
    for (std::size_t at = text.find('Z'); at != std::string::npos; at = text.find('Z', at)) {
        text.replace(at, 1, "+00:00");
        at += 6;
    }
    try {
        const py::DateTime moment = py::fromisoformat(text);
        if (!moment.utc_offset_us) {
            return moment;
        }
        return history::local_wall_time(moment, *clock.zone());
    } catch (const py::PyError&) {
        // Python returns None for a ValueError only and lets a moment it cannot convert reach the
        // slot; the page shows the stored text instead.
        return std::nullopt;
    }
}

std::string date_label(const py::DateTime& local) {
    return std::format("{} {}, {} at {}", calendar_view::month_abbr(local.month), local.day,
                       local.year, calendar_view::clock(local));
}

std::string date_label(const Json& value, const Clock& clock) {
    const std::optional<py::DateTime> date = local_date(value, clock);
    if (!date) {
        return truthy(value) ? pyStr(value) : std::string(UNKNOWN_DATE);
    }
    return date_label(*date);
}

} // namespace backup_page

BackupSettingsPage::Facts BackupSettingsPage::Facts::from(const backup::BackupInfo& info) {
    Facts facts;
    facts.path = path_to_utf8(info.path);
    facts.createdAt = info.created_at;
    facts.appVersion = info.app_version;
    facts.includeRecordings = info.include_recordings;
    facts.fileCount = info.file_count;
    facts.totalBytes = info.total_bytes;
    facts.archiveBytes = info.archive_bytes;
    facts.warnings = info.warnings;
    return facts;
}

BackupSettingsPage::Facts BackupSettingsPage::Facts::from(const Json& info) {
    Facts facts;
    if (!info.is_object()) {
        return facts;
    }
    facts.path = stringField(info, "path", "");
    if (const auto found = info.find("created_at"); found != info.end()) {
        facts.createdAt = *found;
    }
    facts.appVersion = stringField(info, "app_version", "?");
    if (const auto found = info.find("include_recordings"); found != info.end()) {
        facts.includeRecordings = truthy(*found);
    }
    facts.fileCount = countField(info, "file_count");
    facts.totalBytes = countField(info, "total_bytes");
    facts.archiveBytes = countField(info, "archive_bytes");
    if (const auto found = info.find("warnings"); found != info.end() && found->is_array()) {
        for (const Json& item : *found) {
            facts.warnings.push_back(pyStr(item));
        }
    }
    return facts;
}

BackupSettingsPage::BackupSettingsPage(backup::BackupCoordinator* coordinator)
    : BackupSettingsPage(coordinator, Options{}, nullptr) {}

BackupSettingsPage::BackupSettingsPage(backup::BackupCoordinator* coordinator, Options options,
                                       QWidget* parent)
    : QWidget(parent), coordinator_(coordinator), options_(std::move(options)),
      clock_(options_.clock != nullptr ? options_.clock : &system_clock()) {
    setObjectName(QStringLiteral("backupSettingsPage"));
    options_.dialogs = withDefaults(std::move(options_.dialogs));
    const QString name = q(product());

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(16);

    createTile_ = new InfoTile(
        QStringLiteral("Back up local data"),
        QStringLiteral("Keep a local copy of your settings, transcripts, meetings, and "
                       "walkthroughs."),
        ui::designIcon(QStringLiteral("box-blue.svg")));
    ui::setStyleProperty(createTile_, ui::prop::backupCard, QStringLiteral("create"));
    lastBackupLabel_ = settingsCaption(QStringLiteral("No backup created yet"));
    lastBackupLabel_->setObjectName(QStringLiteral("backupLastCreated"));
    lastLocationLabel_ = new ElidingLabel(QString(), Qt::ElideMiddle);
    lastLocationLabel_->setObjectName(QStringLiteral("backupLastLocation"));
    lastSizeLabel_ = settingsCaption(QString());
    lastSizeLabel_->setObjectName(QStringLiteral("backupLastSize"));
    // Nothing to say until a backup exists; start collapsed like refresh() does.
    lastLocationLabel_->hide();
    lastSizeLabel_->hide();
    includeRecordings_ = new QCheckBox(QStringLiteral("Include saved recordings and captures"));
    includeRecordings_->setObjectName(QStringLiteral("backupIncludeRecordings"));
    includeRecordings_->setChecked(true);
    createButton_ = compactPrimaryButton(new PrimaryButton(QStringLiteral(u"Create backup…")));
    createButton_->setObjectName(QStringLiteral("backupCreateButton"));
    connect(createButton_, &QPushButton::clicked, this, [this] { chooseBackupDestination(); });
    createTile_->addBody(lastBackupLabel_);
    createTile_->addBody(lastLocationLabel_);
    createTile_->addBody(lastSizeLabel_);
    createRow_ = actionRow();
    createActions_ = new QBoxLayout(QBoxLayout::LeftToRight, createRow_);
    createActions_->setContentsMargins(0, 0, 0, 0);
    createActions_->setSpacing(12);
    createActions_->addWidget(includeRecordings_);
    createActions_->addStretch();
    createActions_->addWidget(createButton_);
    createTile_->addBody(createRow_);
    createTile_->addBody(
        settingsCaption(QStringLiteral("Speech models, API keys, and paired devices are "
                                       "excluded.")));
    privacyLabel_ = settingsCaption(
        QStringLiteral("Backup files are not encrypted. Keep them in a private location."));
    privacyLabel_->setObjectName(QStringLiteral("backupPrivacyNotice"));
    createTile_->addBody(privacyLabel_);
    layout->addWidget(createTile_);

    restoreTile_ = new InfoTile(
        QStringLiteral("Inspect and restore"),
        QStringLiteral("Review a backup before restoring. Your current data is kept for "
                       "recovery."),
        ui::designIcon(QStringLiteral("refresh-blue.svg")));
    restoreRow_ = actionRow();
    restoreActions_ = new QBoxLayout(QBoxLayout::LeftToRight, restoreRow_);
    restoreActions_->setContentsMargins(0, 0, 0, 0);
    inspectButton_ = neutralButton(new Button(QStringLiteral(u"Choose backup…")));
    inspectButton_->setObjectName(QStringLiteral("backupInspectButton"));
    connect(inspectButton_, &QPushButton::clicked, this, [this] { chooseRestoreSource(); });
    restoreActions_->addWidget(inspectButton_);
    restoreButton_ = compactPrimaryButton(new PrimaryButton(QStringLiteral("Restore and restart")));
    restoreButton_->setObjectName(QStringLiteral("backupRestoreButton"));
    restoreButton_->setEnabled(false);
    connect(restoreButton_, &QPushButton::clicked, this, [this] { confirmRestore(); });
    restoreActions_->addWidget(restoreButton_);
    importButton_ = neutralButton(
        new Button(QStringLiteral(u"Import from %1…").arg(q(legacy_identity().product))));
    importButton_->setObjectName(QStringLiteral("backupImportButton"));
    importButton_->hide();
    connect(importButton_, &QPushButton::clicked, this, [this] {
        if (options_.import != nullptr && coordinator_ != nullptr && !busy_) {
            options_.import->open(this);
        }
    });
    restoreActions_->addWidget(importButton_);
    restoreActions_->addStretch();
    restoreTile_->addBody(restoreRow_);
    inspectionLabel_ = settingsCaption(QStringLiteral("No backup selected"));
    inspectionLabel_->setObjectName(QStringLiteral("backupInspectionSummary"));
    restoreTile_->addBody(inspectionLabel_);
    inspectionLocationLabel_ = new ElidingLabel(QString(), Qt::ElideMiddle);
    inspectionLocationLabel_->hide();
    restoreTile_->addBody(inspectionLocationLabel_);
    recoveryLocationLabel_ = new ElidingLabel(QString(), Qt::ElideMiddle);
    recoveryLocationLabel_->setObjectName(QStringLiteral("backupRecoveryLocation"));
    recoveryLocationLabel_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    recoveryLocationLabel_->hide();
    restoreTile_->addBody(recoveryLocationLabel_);

    scheduleTile_ =
        new InfoTile(QStringLiteral("Schedule"),
                     QStringLiteral("Plan automatic backups and see the dates at a glance."),
                     ui::designIcon(QStringLiteral("layout-grid-blue.svg")));
    ui::setStyleProperty(scheduleTile_, ui::prop::backupCard, QStringLiteral("schedule"));
    auto* scheduleBody = new QWidget;
    scheduleBody->setObjectName(QStringLiteral("backupScheduleBody"));
    scheduleLayout_ = new QBoxLayout(QBoxLayout::LeftToRight, scheduleBody);
    scheduleLayout_->setContentsMargins(0, 0, 0, 0);
    scheduleLayout_->setSpacing(24);
    auto* scheduleFields = new QWidget;
    scheduleFields->setObjectName(QStringLiteral("backupScheduleFields"));
    auto* fields = new QVBoxLayout(scheduleFields);
    fields->setContentsMargins(0, 0, 0, 0);
    fields->setSpacing(12);
    scheduleSummaryLabel_ = settingsCaption(QStringLiteral("Automatic backups are off"));
    scheduleSummaryLabel_->setObjectName(QStringLiteral("backupScheduleSummary"));
    fields->addWidget(scheduleSummaryLabel_);
    frequencyCombo_ = new ElidingComboBox;
    frequencyCombo_->setObjectName(QStringLiteral("backupFrequencyCombo"));
    frequencyCombo_->addItem(QStringLiteral("Off"), QStringLiteral("off"));
    frequencyCombo_->addItem(QStringLiteral("Daily"), QStringLiteral("daily"));
    frequencyCombo_->addItem(QStringLiteral("Weekly"), QStringLiteral("weekly"));
    connect(frequencyCombo_, &QComboBox::currentIndexChanged, this,
            [this] { updateScheduleControls(); });
    fields->addWidget(fieldRow(QStringLiteral("Frequency"), frequencyCombo_));

    destinationEdit_ = new QLineEdit;
    destinationEdit_->setObjectName(QStringLiteral("backupScheduleDestination"));
    destinationEdit_->setReadOnly(true);
    destinationEdit_->setPlaceholderText(QStringLiteral("Choose a local folder"));
    destinationButton_ = neutralButton(new Button(QStringLiteral(u"Browse…")));
    destinationButton_->setObjectName(QStringLiteral("backupScheduleBrowseButton"));
    connect(destinationButton_, &QPushButton::clicked, this,
            [this] { chooseScheduleDestination(); });
    auto* destinationRow = new QWidget;
    destinationRow->setObjectName(QStringLiteral("backupDestinationRow"));
    auto* destinationLayout = new QHBoxLayout(destinationRow);
    destinationLayout->setContentsMargins(0, 0, 0, 0);
    destinationLayout->addWidget(destinationEdit_, 1);
    destinationLayout->addWidget(destinationButton_);
    fields->addWidget(fieldRow(QStringLiteral("Save to"), destinationRow));

    retentionSpin_ = new QSpinBox;
    retentionSpin_->setObjectName(QStringLiteral("backupRetentionCount"));
    retentionSpin_->setRange(1, 100);
    retentionSpin_->setValue(5);
    retentionSpin_->setSuffix(QStringLiteral(" backups"));
    retentionSpin_->setKeyboardTracking(false);
    fields->addWidget(fieldRow(QStringLiteral("Keep latest"), retentionSpin_));
    saveScheduleButton_ = neutralButton(new Button(QStringLiteral("Save schedule")));
    saveScheduleButton_->setObjectName(QStringLiteral("backupSaveScheduleButton"));
    connect(saveScheduleButton_, &QPushButton::clicked, this, [this] { saveSchedule(); });
    fields->addWidget(saveScheduleButton_, 0, Qt::AlignLeft);
    fields->addWidget(settingsCaption(
        QStringLiteral("Runs while %1 is open and idle, with saved recordings included. Older "
                       "automatic backups are removed after the limit is reached. Planned dates "
                       "follow your saved schedule and may shift while the app is closed or "
                       "busy.")
            .arg(name)));
    fields->addStretch();
    calendar_ = new BackupCalendar(*clock_);
    scheduleLayout_->addWidget(scheduleFields, 1);
    scheduleLayout_->addWidget(calendar_, 1);
    scheduleTile_->addBody(scheduleBody);
    layout->addWidget(scheduleTile_);
    layout->addWidget(restoreTile_);

    for (InfoTile* tile : {createTile_, scheduleTile_, restoreTile_}) {
        tile->setBodyIndented(false);
        tile->layout()->setContentsMargins(20, 18, 20, 18);
        tile->bodyLayout()->setSpacing(10);
    }

    progressBar_ = new QProgressBar;
    progressBar_->setObjectName(QStringLiteral("backupProgressBar"));
    progressBar_->hide();
    layout->addWidget(progressBar_);
    cancelButton_ = neutralButton(new Button(QStringLiteral("Cancel")));
    cancelButton_->setObjectName(QStringLiteral("backupCancelButton"));
    connect(cancelButton_, &QPushButton::clicked, this, [this] { cancel(); });
    cancelButton_->hide();
    layout->addWidget(cancelButton_);
    statusLabel_ = settingsCaption(QString());
    statusLabel_->setObjectName(QStringLiteral("backupStatus"));
    statusLabel_->setAccessibleName(QStringLiteral("Backup status"));
    statusLabel_->setTextFormat(Qt::PlainText);
    layout->addWidget(statusLabel_);
    layout->addStretch();

    if (options_.import != nullptr) {
        connect(options_.import, &BackupImportOffer::availabilityChanged, this,
                [this](bool available) { showImportButton(available); });
        importButton_->setVisible(options_.import->available());
        options_.import->refresh();
    }
    connectCoordinator();
    refresh();
}

BackupSettingsPage::~BackupSettingsPage() {
    // Closing a connection waits for a callback still running on another thread: before the
    // widgets and this object go.
    subscriptions_.clear();
}

QString BackupSettingsPage::frequency() const {
    return frequencyCombo_->currentData().toString();
}

QWidget* BackupSettingsPage::namedControl(std::string_view name) {
    if (name == "backup_page") {
        return this;
    }
    if (name == "backup_create_tile") {
        return createTile_;
    }
    if (name == "backup_restore_tile") {
        return restoreTile_;
    }
    if (name == "backup_schedule_tile") {
        return scheduleTile_;
    }
    return nullptr;
}

void BackupSettingsPage::setStatus(const std::string& text) {
    statusLabel_->setText(q(text));
}

void BackupSettingsPage::resizeEvent(QResizeEvent* event) {
    QWidget::resizeEvent(event);
    updateDirections();
}

void BackupSettingsPage::showImportButton(bool shown) {
    importButton_->setVisible(shown);
    // The restore row is as wide as its tile, so a button that comes or goes can decide whether
    // the row still holds its controls side by side; OpenWhisper, without this button, decides on
    // resize only.
    updateDirections();
}

void BackupSettingsPage::updateDirections() {
    const bool narrow = width() < 680;
    scheduleLayout_->setDirection(narrow ? QBoxLayout::TopToBottom : QBoxLayout::LeftToRight);
    // Both rows stack together once either can't hold its controls side by side, measured in the
    // current font rather than at a set width.
    const bool stacked = !(fitsSideBySide(*createRow_, *createActions_) &&
                           fitsSideBySide(*restoreRow_, *restoreActions_));
    const auto direction = stacked ? QBoxLayout::TopToBottom : QBoxLayout::LeftToRight;
    createActions_->setDirection(direction);
    restoreActions_->setDirection(direction);
    createActions_->setAlignment(createButton_, stacked ? Qt::AlignLeft : Qt::AlignRight);
    for (Button* button : {inspectButton_, restoreButton_, importButton_}) {
        restoreActions_->setAlignment(button, Qt::AlignLeft);
    }
}

void BackupSettingsPage::updateCalendar() {
    std::optional<Day> latestDay;
    if (lastBackup_) {
        if (const auto latest = backup_page::local_date(lastBackup_->createdAt, *clock_)) {
            latestDay = dayOf(*latest);
        }
    }
    std::optional<Day> nextDue;
    std::optional<py::DateTime> due;
    const std::optional<std::int64_t> interval =
        savedFrequency_ == "daily"    ? std::optional<std::int64_t>(1)
        : savedFrequency_ == "weekly" ? std::optional<std::int64_t>(7)
                                      : std::nullopt;
    if (interval && nextBackupAt_) {
        due = localFromEpoch(*nextBackupAt_, *clock_);
        if (due) {
            nextDue = dayOf(*due);
        }
    }
    calendar_->setDates(latestDay, nextDue, interval ? static_cast<int>(*interval) : 0);
    std::string summary;
    if (!interval) {
        summary = "Automatic backups are off";
    } else if (!due) {
        summary = py::capitalize(savedFrequency_) + " backups enabled";
    } else if (*nextDue <= localToday(*clock_)) {
        summary = "Next backup: when the app is idle";
    } else {
        summary = "Next backup: " + backup_page::date_label(*due);
    }
    scheduleSummaryLabel_->setText(q(summary));
}

void BackupSettingsPage::connectCoordinator() {
    if (coordinator_ == nullptr) {
        return;
    }
    // Each handler queues itself onto this page's thread: `progress` comes from the worker, and
    // the others, though emitted on the UI actor, must not run inside the coordinator.
    Executor& inlined = inlineExecutor();
    subscriptions_.push_back(coordinator_->busy_changed.connect(
        inlined, [this](bool busy) { qt::post(this, [this, busy] { setBusy(busy); }); }));
    subscriptions_.push_back(
        coordinator_->progress.connect(inlined, [this](std::string phase, int done, int total) {
            qt::post(this, [this, phase = std::move(phase), done, total] {
                setProgress(phase, done, total);
            });
        }));
    subscriptions_.push_back(coordinator_->backup_finished.connect(
        inlined, [this](std::optional<backup::BackupInfo> info, std::string error) {
            qt::post(this, [this, info = std::move(info), error = std::move(error)] {
                showBackupResult(info, error);
            });
        }));
    subscriptions_.push_back(coordinator_->inspection_finished.connect(
        inlined, [this](std::optional<backup::BackupInfo> info, std::string error) {
            qt::post(this, [this, info = std::move(info), error = std::move(error)] {
                showInspectionResult(info, error);
            });
        }));
    subscriptions_.push_back(
        coordinator_->restore_staged.connect(inlined, [this](std::string stage, std::string error) {
            qt::post(this, [this, stage = std::move(stage), error = std::move(error)] {
                showRestoreResult(stage, error);
            });
        }));
    subscriptions_.push_back(coordinator_->schedule_changed.connect(
        inlined, [this] { qt::post(this, [this] { refresh(); }); }));
}

void BackupSettingsPage::refresh() {
    if (coordinator_ == nullptr) {
        setStatus("Backup and restore are unavailable in this session.");
        updateActions();
        return;
    }
    Json state;
    try {
        state = coordinator_->snapshot();
    } catch (const std::exception& error) {
        kLog.error_with(error, "Could not load backup settings");
        setStatus(std::string("Could not load backup settings: ") + error.what());
        updateActions();
        return;
    }
    if (!state.is_object()) {
        state = Json::object();
    }
    const Json last = state.value("last_backup", Json(nullptr));
    showLastBackup(last.is_null() ? std::nullopt : std::optional<Facts>(Facts::from(last)));
    const std::string previousDataDir = stringField(state, "previous_data_dir", "");
    recoveryLocationLabel_->setText(
        previousDataDir.empty() ? QString() : q("Previous data: " + previousDataDir));
    recoveryLocationLabel_->setVisible(!previousDataDir.empty());

    savedFrequency_ = stringField(state, "frequency", "off");
    nextBackupAt_.reset();
    if (const auto found = state.find("next_backup_at");
        found != state.end() && found->is_number()) {
        nextBackupAt_ = found->get<double>();
    }
    const int index = frequencyCombo_->findData(q(savedFrequency_));
    frequencyCombo_->setCurrentIndex(std::max(0, index));
    int retention = 5;
    if (const auto found = state.find("retention_count");
        found != state.end() && found->is_number()) {
        retention = static_cast<int>(found->get<double>());
    }
    retentionSpin_->setValue(retention);
    destinationEdit_->setText(q(stringField(state, "destination", "")));
    if (const auto found = state.find("busy"); found != state.end()) {
        busy_ = truthy(*found);
    }
    operation_ = stringField(state, "operation", "");
    updateScheduleControls();
    updateActions();
    updateCalendar();
    const Json error = state.value("last_error", Json(nullptr));
    if (truthy(error)) {
        setStatus(pyStr(error));
    }
}

void BackupSettingsPage::showLastBackup(const std::optional<Facts>& info) {
    lastBackup_ = info;
    lastLocationLabel_->setVisible(info.has_value());
    lastSizeLabel_->setVisible(info.has_value());
    if (!info) {
        lastBackupLabel_->setText(QStringLiteral("No backup created yet"));
        lastLocationLabel_->setText(QString());
        lastSizeLabel_->setText(QString());
        return;
    }
    lastBackupLabel_->setText(
        q("Last backup: " + backup_page::date_label(info->createdAt, *clock_)));
    lastLocationLabel_->setText(q("Location: " + info->path));
    lastSizeLabel_->setText(
        q("Size: " + format_file_size(static_cast<double>(info->archiveBytes))));
    updateCalendar();
}

void BackupSettingsPage::updateScheduleControls() {
    const bool enabled = frequency() != QLatin1String("off");
    const bool available = coordinator_ != nullptr && !busy_;
    destinationEdit_->setEnabled(enabled && available);
    destinationButton_->setEnabled(enabled && available);
    retentionSpin_->setEnabled(enabled && available);
}

void BackupSettingsPage::updateActions() {
    const bool available = coordinator_ != nullptr && !busy_;
    createButton_->setEnabled(available);
    includeRecordings_->setEnabled(available);
    inspectButton_->setEnabled(available);
    restoreButton_->setEnabled(available && !inspectedPath_.isEmpty());
    importButton_->setEnabled(available);
    frequencyCombo_->setEnabled(available);
    saveScheduleButton_->setEnabled(available);
    updateScheduleControls();
    const bool cancelable = busy_ && (operation_ == "backup" || operation_ == "Creating backup");
    cancelButton_->setVisible(cancelable);
    cancelButton_->setEnabled(cancelable && coordinator_ != nullptr);
    progressBar_->setVisible(busy_);
}

void BackupSettingsPage::chooseBackupDestination() {
    if (coordinator_ == nullptr || busy_) {
        return;
    }
    const std::string name =
        std::string(product()) + "-backup" + std::string(identity().backup_extension);
    const std::string folder = py::strip(qt::toUtf8(destinationEdit_->text()));
    const std::string suggested =
        folder.empty() ? name : path_to_utf8(path_from_utf8(folder) / path_from_utf8(name));
    const std::string extension(identity().backup_extension);
    const QString path =
        options_.dialogs.saveFile(this, q("Create " + product() + " backup"), q(suggested),
                                  q(product() + " backups (*" + extension + ");;All files (*)"));
    if (path.isEmpty()) {
        return;
    }
    const bool recordings = includeRecordings_->isChecked();
    start(QStringLiteral("Creating backup"), [this, path, recordings] {
        coordinator_->create_backup(path_from_utf8(qt::toUtf8(path)), recordings);
    });
}

void BackupSettingsPage::chooseRestoreSource() {
    if (coordinator_ == nullptr || busy_) {
        return;
    }
    const std::string extension(identity().backup_extension);
    const QString path = options_.dialogs.openFile(
        this, q("Inspect " + product() + " backup"), destinationEdit_->text().trimmed(),
        q(product() + " backups (*" + extension + " *" +
          std::string(legacy_identity().backup_extension) + " *.zip);;All files (*)"));
    if (path.isEmpty()) {
        return;
    }
    inspectedPath_.clear();
    pendingInspectionPath_ = path;
    inspectionLocationLabel_->setText(QString());
    inspectionLocationLabel_->hide();
    inspectionLabel_->setText(QStringLiteral(u"Inspecting backup…"));
    updateActions();
    start(QStringLiteral("Inspecting backup"),
          [this, path] { coordinator_->inspect_backup(path_from_utf8(qt::toUtf8(path))); });
}

void BackupSettingsPage::confirmRestore() {
    if (coordinator_ == nullptr || busy_ || inspectedPath_.isEmpty()) {
        return;
    }
    const std::string name = product();
    const bool yes = options_.dialogs.confirm(
        this, QStringLiteral("Restore local data"),
        q("Restore this backup and restart " + name +
          "?\n\nThis replaces local settings, transcription history, meeting data, and "
          "recordings with the contents shown above. Your current data is kept for recovery. "
          "API keys and paired devices are excluded."));
    if (yes) {
        const QString path = inspectedPath_;
        start(QStringLiteral("Preparing restore"),
              [this, path] { coordinator_->prepare_restore(path_from_utf8(qt::toUtf8(path))); });
    }
}

void BackupSettingsPage::chooseScheduleDestination() {
    const QString folder = options_.dialogs.chooseFolder(
        this, QStringLiteral("Choose automatic backup folder"), destinationEdit_->text().trimmed());
    if (!folder.isEmpty()) {
        destinationEdit_->setText(folder);
    }
}

void BackupSettingsPage::saveSchedule() {
    if (coordinator_ == nullptr || busy_) {
        return;
    }
    const std::string chosen = qt::toUtf8(frequency());
    const std::string destination = py::strip(qt::toUtf8(destinationEdit_->text()));
    if (chosen != "off" && destination.empty()) {
        setStatus("Choose a destination folder for automatic backups.");
        return;
    }
    if (chosen != "off") {
        try {
            const std::filesystem::path folder =
                std::filesystem::weakly_canonical(path_from_utf8(destination));
            std::error_code ignored;
            if (!std::filesystem::is_directory(folder, ignored)) {
                setStatus("The automatic backup destination must be an existing folder.");
                return;
            }
            if (!options_.dataRoot.empty()) {
                for (const char* owned : {"recordings", "meetings", "captures"}) {
                    if (isRelativeTo(
                            folder, std::filesystem::weakly_canonical(options_.dataRoot / owned))) {
                        setStatus("Choose a folder outside " + product() +
                                  "'s recordings, meetings, and captures folders.");
                        return;
                    }
                }
            }
        } catch (const std::filesystem::filesystem_error& error) {
            setStatus(std::string("Could not check backup destination: ") + error.what());
            return;
        }
    }
    try {
        const std::string refusal = coordinator_->set_schedule(chosen, retentionSpin_->value(),
                                                               path_from_utf8(destination));
        if (!refusal.empty()) {
            kLog.warning("Could not save automatic backup settings: {}", refusal);
            setStatus("Could not save automatic backup settings: " + refusal);
            return;
        }
    } catch (const std::exception& error) {
        kLog.error_with(error, "Could not save automatic backup settings");
        setStatus(std::string("Could not save automatic backup settings: ") + error.what());
        return;
    }
    setStatus(chosen == "off" ? "Automatic backups are off."
                              : "Automatic backups set to " + chosen + ".");
}

void BackupSettingsPage::start(const QString& operation, const std::function<void()>& request) {
    operation_ = qt::toUtf8(operation);
    setBusy(true);
    setStatus(operation_ + "\xE2\x80\xA6");
    try {
        request();
    } catch (const std::exception& error) {
        kLog.error_with(error, operation_ + " failed to start");
        setBusy(false);
        setStatus(operation_ + " failed: " + error.what());
    }
}

void BackupSettingsPage::cancel() {
    if (coordinator_ == nullptr) {
        return;
    }
    try {
        coordinator_->cancel();
    } catch (const std::exception& error) {
        kLog.error_with(error, "Could not cancel the backup operation");
        setStatus(std::string("Could not cancel: ") + error.what());
        return;
    }
    cancelButton_->setEnabled(false);
    setStatus("Cancelling\xE2\x80\xA6");
}

void BackupSettingsPage::setBusy(bool busy) {
    const bool changed = busy_ != busy;
    busy_ = busy;
    if (busy) {
        if (coordinator_ != nullptr) {
            const std::string running = coordinator_->operation();
            if (!running.empty()) {
                operation_ = running;
            }
        }
        progressBar_->setRange(0, 0);
    }
    updateActions();
    if (changed) {
        Q_EMIT operationBusyChanged(busy_);
    }
}

void BackupSettingsPage::setProgress(const std::string& phase, int done, int total) {
    if (total > 0) {
        progressBar_->setRange(0, 100);
        const double percent = py::round_half_even(100.0 * done / total);
        progressBar_->setValue(static_cast<int>(std::clamp(percent, 0.0, 100.0)));
        setStatus(std::format("{}: {}%", phase, progressBar_->value()));
    } else {
        progressBar_->setRange(0, 0);
        setStatus(phase);
    }
}

void BackupSettingsPage::showBackupResult(const std::optional<backup::BackupInfo>& info,
                                          const std::string& error) {
    setBusy(false);
    if (!info) {
        setStatus("Backup failed: " + (error.empty() ? std::string("Unknown error") : error));
        return;
    }
    const Facts facts = Facts::from(*info);
    showLastBackup(facts);
    std::string text = error.empty() ? "Backup created: " + facts.path
                                     : "Backup saved at " + facts.path + "\n" + error;
    if (!facts.warnings.empty()) {
        text += "\nWarnings: " + joined(facts.warnings);
    }
    setStatus(text);
}

void BackupSettingsPage::showInspectionResult(const std::optional<backup::BackupInfo>& info,
                                              const std::string& error) {
    setBusy(false);
    if (!error.empty() || !info) {
        inspectedPath_.clear();
        inspectionLocationLabel_->setText(QString());
        inspectionLocationLabel_->hide();
        inspectionLabel_->setText(q("Could not inspect backup: " +
                                    (error.empty() ? std::string("Unknown error") : error)));
        setStatus(qt::toUtf8(inspectionLabel_->text()));
        updateActions();
        return;
    }
    const Facts facts = Facts::from(*info);
    inspectedPath_ = facts.path.empty() ? pendingInspectionPath_ : q(facts.path);
    std::string text =
        std::format("Backup from {}\nVersion {} \xC2\xB7 {} files \xC2\xB7 {} archive\n"
                    "Contents: app settings, transcription history, and saved meetings (if "
                    "any).\nSaved recordings and captures: {} \xC2\xB7 {} before compression",
                    backup_page::date_label(facts.createdAt, *clock_), facts.appVersion,
                    facts.fileCount, format_file_size(static_cast<double>(facts.archiveBytes)),
                    facts.includeRecordings ? "included" : "not included",
                    format_file_size(static_cast<double>(facts.totalBytes)));
    if (!facts.warnings.empty()) {
        text += "\nWarnings: " + joined(facts.warnings);
    }
    inspectionLabel_->setText(q(text));
    inspectionLocationLabel_->setText(QStringLiteral("Location: ") + inspectedPath_);
    inspectionLocationLabel_->show();
    setStatus("Backup inspected. Review the contents before restoring.");
    updateActions();
}

void BackupSettingsPage::showRestoreResult(const std::string& stagingPath,
                                           const std::string& error) {
    setBusy(false);
    if (!error.empty() || stagingPath.empty()) {
        setStatus("Restore could not be prepared: " +
                  (error.empty() ? std::string("Unknown error") : error));
        return;
    }
    setStatus("Restore staged. " + product() + " will restart to apply it.");
}

void registerBackupPage(AppContext& context) {
    // The built page, for the rail's value; the dialog owns the page and may rebuild it.
    auto built = std::make_shared<QPointer<BackupSettingsPage>>();
    SettingsPageSpec spec;
    spec.destination = destinations::backup;
    spec.title = QStringLiteral("Backup & restore");
    spec.subtitle =
        QStringLiteral("Save a local copy of your data or inspect a backup before restoring it.");
    spec.create = [&context, built](QWidget* parent) -> QWidget* {
        BackupSettingsPage::Options options;
        try {
            options.clock = &context.clock();
        } catch (const ServiceUnavailable&) {
            // The system clock.
        }
        try {
            options.dataRoot = context.paths().data_root();
        } catch (const ServiceUnavailable&) {
            // No data folder to keep a schedule out of.
        }
        options.import = context.find<BackupImportOffer>();
        auto* page = new BackupSettingsPage(context.find<backup::BackupCoordinator>(),
                                            std::move(options), parent);
        *built = page;
        if (auto* settingsPage = qobject_cast<SettingsPage*>(parent);
            settingsPage != nullptr && settingsPage->dialog() != nullptr) {
            SettingsDialog* dialog = settingsPage->dialog();
            QObject::connect(page, &BackupSettingsPage::operationBusyChanged, dialog,
                             [dialog](bool busy) { dialog->setBackupBusy(busy); });
        }
        return page;
    };
    spec.railValue = [built](const Json&) {
        if (*built == nullptr) {
            return QStringLiteral("Local backups");
        }
        const QString chosen = (*built)->frequency();
        return chosen == QLatin1String("off") ? QStringLiteral("Off")
                                              : q(py::title(qt::toUtf8(chosen)));
    };
    spec.searchFields = {
        {"backup_create_tile", QStringLiteral("Back up local data"),
         QStringLiteral("Save local settings, transcription history, meeting data, and "
                        "optionally recordings.")},
        {"backup_restore_tile", QStringLiteral("Inspect and restore"),
         QStringLiteral("Review a backup archive before staging a restore and restart.")},
        {"backup_schedule_tile", QStringLiteral("Schedule"),
         QStringLiteral("Automatic local backups: off, daily, or weekly; set destination and "
                        "retention count.")},
    };
    spec.controls = {"backup_page", "backup_create_tile", "backup_restore_tile",
                     "backup_schedule_tile"};
    context.settingsPages().addPage(std::move(spec));
}

void followBackupBusy(AppContext& context, SettingsDialog& dialog) {
    backup::BackupCoordinator* coordinator = context.find<backup::BackupCoordinator>();
    if (coordinator == nullptr) {
        return;
    }
    // The holder is the callbacks' context: they stop with the dialog.
    auto* holder = new QObject(&dialog);
    auto subscription = std::make_shared<Subscription>();
    *subscription = coordinator->busy_changed.connect(
        inlineExecutor(), [holder, target = QPointer<SettingsDialog>(&dialog)](bool busy) {
            qt::post(holder, [target, busy] {
                if (target != nullptr) {
                    target->setBackupBusy(busy);
                }
            });
        });
    QObject::connect(holder, &QObject::destroyed, holder,
                     [subscription] { subscription->disconnect(); });
    try {
        dialog.setBackupBusy(truthy(coordinator->snapshot().value("busy", Json(false))));
    } catch (const std::exception& error) {
        kLog.error_with(error, "Could not read backup coordinator state");
    }
}

} // namespace fv
