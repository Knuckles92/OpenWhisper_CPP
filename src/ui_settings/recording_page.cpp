#include "ui_settings/recording_page.h"

#include <algorithm>
#include <exception>
#include <utility>

#include <QEvent>
#include <QFormLayout>
#include <QLabel>
#include <QMessageBox>
#include <QScopeGuard>
#include <QTimer>
#include <QVBoxLayout>

#include "appcore/retention_policy.h"
#include "appkit/app_context.h"
#include "base/executor.h"
#include "base/log.h"
#include "history/manager.h"
#include "settings/keys.h"
#include "settings/store.h"
#include "ui_settings/app_pages.h"
#include "ui_settings/detail/mailbox.h"
#include "ui_settings/settings_dialog.h"
#include "ui_settings/settings_fields.h"
#include "ui_settings/settings_page.h"
#include "ui_theme/font_scale.h"
#include "ui_theme/icons.h"
#include "ui_widgets/no_wheel.h"
#include "ui_widgets/setting_tile.h"
#include "ui_widgets/settings_switch.h"
#include "ui_widgets/wrapped_label.h"

namespace fv {
namespace {

constexpr log::Logger kLog{"ui_qt.dialogs.settings_dialog"};

QString q(std::string_view text) {
    return qt::toQString(text);
}

// settings_dialog.py _spin_form: one right-aligned label beside a spin box at its size hint.
QFormLayout* spinForm(QLabel* label, QWidget* spinbox) {
    auto* form = new QFormLayout;
    form->setContentsMargins(0, 0, 0, 0);
    form->setLabelAlignment(Qt::AlignRight | Qt::AlignVCenter);
    form->setFormAlignment(Qt::AlignLeft | Qt::AlignTop);
    form->setHorizontalSpacing(16);
    form->setFieldGrowthPolicy(QFormLayout::FieldsStayAtSizeHint);
    label->setObjectName(QStringLiteral("settingsTileFieldLabel"));
    form->addRow(label, spinbox);
    return form;
}

Json comboData(const ElidingComboBox* combo) {
    const QVariant data = combo->currentData();
    return data.isValid() ? Json(qt::toUtf8(data.toString())) : Json();
}

} // namespace

RecordingPage::RecordingPage(SettingsPage& page, SettingsDialog& dialog,
                             SettingsPagesSignals& signals, Ports ports)
    : QWidget(&page), dialog_(dialog), signals_(signals), ports_(std::move(ports)),
      mailbox_(std::make_shared<detail::Mailbox<RecordingPage>>(this)) {
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(8);

    retentionTimer_ = new QTimer(this);
    retentionTimer_->setSingleShot(true);
    retentionTimer_->setInterval(kRetentionSettleMs);
    connect(retentionTimer_, &QTimer::timeout, this, [this] { commitRetention(); });

    microphones_ = new MicrophoneSection(dialog_, ports_.microphones, this);
    microphones_->build(page, layout);
    connect(microphones_, &MicrophoneSection::audioDeviceChanged, &signals_,
            &SettingsPagesSignals::audioDeviceChanged);

    buildRetention(page, layout);

    streamingTile_ = new SettingTile(
        QStringLiteral("Real-time transcription preview"),
        QStringLiteral("Shows text as you speak on the near-cursor overlay. Parakeet transcribes "
                       "short audio chunks with the loaded model. The final transcript uses your "
                       "selected model and follows the General paste and clipboard settings. Its "
                       "text size is under Appearance."),
        ui::designIcon(QStringLiteral("typography-blue.svg")));
    connect(streamingTile_->checkbox(), &QAbstractButton::toggled, this,
            [this](bool checked) { onStreamingToggled(checked); });
    page.addTileGroup(layout, QStringLiteral("Live preview"), {streamingTile_});

    // The window rescans the recordings folder on each opening.
    connect(&dialog_, &SettingsDialog::windowShown, this, [this] { refreshRecordingsUsage(); });
    // A burst of steps still settling is committed when the window closes.
    dialog_.installEventFilter(this);
}

RecordingPage::~RecordingPage() {
    mailbox_->close();
}

void RecordingPage::buildRetention(SettingsPage& page, QVBoxLayout* layout) {
    retentionCombo_ = new ElidingComboBox;
    retentionCombo_->addItem(QStringLiteral("Keep all"), QStringLiteral("keep_all"));
    retentionCombo_->addItem(QStringLiteral("By count"), QStringLiteral("custom"));
    retentionCombo_->addItem(QStringLiteral("By folder size"), QStringLiteral("size_limit"));
    retentionCombo_->setMinimumHeight(40);
    connect(retentionCombo_, &QComboBox::currentIndexChanged, this,
            [this] { onRetentionModeChanged(); });
    retentionTile_ = new FieldTile(
        QStringLiteral("Keep recordings"),
        QStringLiteral("Keep a set number of recordings or cap the recordings folder size. The "
                       "oldest audio files are deleted automatically once the limit is passed; "
                       "the newest recording is always kept. Transcription history text is kept "
                       "separately."),
        retentionCombo_, ui::designIcon(QStringLiteral("file-music-blue.svg")));

    maxRecordingsLabel_ = new QLabel(QStringLiteral("Number to keep"));
    maxRecordingsSpin_ = new NoWheelSpinBox;
    maxRecordingsSpin_->setMinimum(1);
    maxRecordingsSpin_->setMaximum(1000);
    maxRecordingsSpin_->setValue(20);
    maxRecordingsSpin_->setMinimumHeight(40);
    // Retention deletes audio, so typed digits commit on Enter or focus-out: tracking every
    // keystroke once applied "1" on the way from 20 to 100 and pruned all but one recording.
    maxRecordingsSpin_->setKeyboardTracking(false);
    connect(maxRecordingsSpin_, &QSpinBox::valueChanged, this,
            [this] { scheduleRetentionCommit(); });
    connect(maxRecordingsSpin_, &QAbstractSpinBox::editingFinished, this,
            [this] { commitRetention(); });
    maxRecordingsMbLabel_ = new QLabel(QStringLiteral("Folder size limit"));
    maxRecordingsMbLabel_->setObjectName(QStringLiteral("settingsTileFieldLabel"));
    maxRecordingsMbSpin_ = new NoWheelSpinBox;
    maxRecordingsMbSpin_->setMinimum(10);
    maxRecordingsMbSpin_->setMaximum(1024 * 1024);
    maxRecordingsMbSpin_->setSingleStep(100);
    maxRecordingsMbSpin_->setSuffix(QStringLiteral(" MB"));
    maxRecordingsMbSpin_->setValue(1024);
    maxRecordingsMbSpin_->setMinimumHeight(40);
    // One width for both limits, wide enough for the largest folder size.
    const int width = ui::scaled(156);
    maxRecordingsSpin_->setFixedWidth(width);
    maxRecordingsMbSpin_->setFixedWidth(width);
    maxRecordingsMbSpin_->setKeyboardTracking(false);
    connect(maxRecordingsMbSpin_, &QSpinBox::valueChanged, this,
            [this] { scheduleRetentionCommit(); });
    connect(maxRecordingsMbSpin_, &QAbstractSpinBox::editingFinished, this,
            [this] { commitRetention(); });
    QFormLayout* form = spinForm(maxRecordingsLabel_, maxRecordingsSpin_);
    form->addRow(maxRecordingsMbLabel_, maxRecordingsMbSpin_);
    retentionTile_->addBodyLayout(form);
    usageLabel_ = settingsCaption(QString());
    retentionTile_->addBody(usageLabel_);
    page.addTileGroup(layout, QStringLiteral("Saved recordings"), {retentionTile_});
}

SettingsSwitch* RecordingPage::streamingCheck() const {
    return streamingTile_->checkbox();
}

bool RecordingPage::loading() const {
    return loading_ || dialog_.isLoading();
}

QWidget* RecordingPage::namedControl(std::string_view name) {
    const std::pair<std::string_view, QWidget*> controls[] = {
        {"audio_device_combo", microphones_->combo()},
        {"audio_device_tile", microphones_->preferredTile()},
        {"microphone_backup_tile", microphones_->backupTile()},
        {"recording_retention_combo", retentionCombo_},
        {"recording_retention_tile", retentionTile_},
        {"max_recordings_label", maxRecordingsLabel_},
        {"max_recordings_spinbox", maxRecordingsSpin_},
        {"max_recordings_mb_label", maxRecordingsMbLabel_},
        {"max_recordings_mb_spinbox", maxRecordingsMbSpin_},
        {"recordings_usage_label", usageLabel_},
        {"streaming_enabled_tile", streamingTile_},
        {"streaming_enabled_check", streamingCheck()},
    };
    for (const auto& [control, widget] : controls) {
        if (control == name) {
            return widget;
        }
    }
    return nullptr;
}

void RecordingPage::loadSettings(const Json& settings) {
    loadRetentionSettings(settings);
    streamingCheck()->setChecked(keys::streaming_enabled.read(settings));
    microphones_->load(settings);
}

// --- retention ---

void RecordingPage::onRetentionModeChanged() {
    updateRetentionUi();
    commitRetention();
}

void RecordingPage::scheduleRetentionCommit() {
    // Typed values arrive only on Enter or focus-out, but chevron and arrow-key steps arrive one
    // at a time; let a burst settle so it asks and prunes once.
    if (!loading()) {
        retentionTimer_->start();
    }
}

void RecordingPage::commitRetention() {
    retentionTimer_->stop();
    if (loading()) {
        return;
    }
    if (confirming_) {
        // The modal takes focus from the spin box, whose editingFinished would otherwise
        // re-enter; anything asked for meanwhile is looked at again once the answer is in.
        recommit_ = true;
        return;
    }
    const Json mode = comboData(retentionCombo_);
    const Json count = maxRecordingsSpin_->value();
    const Json mb = maxRecordingsMbSpin_->value();
    const Json edits = appcore::retention_edits(mode, count, mb);
    const Json saved = *dialog_.context().settings().snapshot();
    const appcore::RetentionLimits savedLimits = appcore::retention_limits(saved);
    const appcore::RetentionLimits limits = appcore::retention_limits(mode, count, mb);
    if (!appcore::retention_changed(savedLimits, limits)) {
        return;
    }
    if (!appcore::shrinks(savedLimits, limits)) {
        finishCommit(edits, true);
        return;
    }
    // _confirm_recording_removal: History lists what the limits delete, on the worker.
    confirming_ = true;
    recommit_ = false;
    auto overLimit = ports_.overLimit;
    ports_.worker->post([mailbox = mailbox_, overLimit, limits, edits] {
        std::vector<RecordingInfo> removed;
        std::optional<std::string> failure;
        try {
            removed = overLimit(limits.max_recordings, limits.max_bytes);
        } catch (const std::exception& error) {
            kLog.error("Couldn't preview recording retention: {}", error.what());
            failure = error.what();
        }
        mailbox->post([removed = std::move(removed), failure, edits](RecordingPage& page) mutable {
            bool apply = false;
            if (failure) {
                page.dialog_.showMessage(q(appcore::retention_check_failed(*failure)));
            } else if (removed.empty()) {
                apply = true;
            } else {
                std::int64_t bytes = 0;
                for (const auto& recording : removed) {
                    bytes += recording.size_bytes;
                }
                const auto count = static_cast<std::int64_t>(removed.size());
                const QString title = q(appcore::delete_confirm_title(count));
                const QString text = q(appcore::delete_confirm_text(count, bytes));
                if (page.ports_.confirm) {
                    apply = page.ports_.confirm(&page.dialog_, title, text);
                } else {
                    apply = QMessageBox::question(&page.dialog_, title, text,
                                                  QMessageBox::Yes | QMessageBox::Cancel,
                                                  QMessageBox::Cancel) == QMessageBox::Yes;
                }
            }
            page.confirming_ = false;
            if (!apply) {
                page.loading_ = true;
                const auto reset = qScopeGuard([&page] { page.loading_ = false; });
                page.loadRetentionSettings(*page.dialog_.context().settings().snapshot());
            } else {
                page.finishCommit(edits, true);
            }
            if (page.recommit_) {
                page.recommit_ = false;
                page.commitRetention();
            }
        });
    });
}

void RecordingPage::finishCommit(const Json& edits, bool apply) {
    if (apply && dialog_.persistMany(edits)) {
        applyRetentionLimit();
    }
}

void RecordingPage::applyRetentionLimit() {
    const appcore::RetentionLimits limits =
        appcore::retention_limits(*dialog_.context().settings().snapshot());
    auto setRetention = ports_.setRetention;
    ports_.worker->post([mailbox = mailbox_, setRetention, limits] {
        try {
            setRetention(limits.max_recordings, limits.max_bytes);
        } catch (const std::exception& error) {
            kLog.error("Couldn't apply recording retention: {}", error.what());
        }
        mailbox->post([](RecordingPage& page) { page.refreshRecordingsUsage(); });
    });
}

void RecordingPage::updateRetentionUi() {
    const QString mode = retentionCombo_->currentData().toString();
    const bool isCount = mode == QStringLiteral("custom");
    const bool isSize = mode == QStringLiteral("size_limit");
    maxRecordingsLabel_->setEnabled(isCount);
    maxRecordingsSpin_->setEnabled(isCount);
    maxRecordingsMbLabel_->setEnabled(isSize);
    maxRecordingsMbSpin_->setEnabled(isSize);
}

void RecordingPage::loadRetentionSettings(const Json& settings) {
    // Each read is already valid: a known mode, and whole numbers of at least 1.
    const int index = retentionCombo_->findData(q(keys::recording_retention_mode.read(settings)));
    retentionCombo_->setCurrentIndex(std::max(0, index));
    maxRecordingsSpin_->setValue(static_cast<int>(keys::max_saved_recordings.read(settings)));
    maxRecordingsMbSpin_->setValue(static_cast<int>(keys::max_saved_recordings_mb.read(settings)));
    updateRetentionUi();
    refreshRecordingsUsage();
}

void RecordingPage::refreshRecordingsUsage() {
    if (!ports_.usage || ports_.worker == nullptr) {
        return;
    }
    const std::uint64_t token = usageGate_.next();
    auto usage = ports_.usage;
    ports_.worker->post([mailbox = mailbox_, gate = usageGate_, token, usage] {
        std::optional<std::pair<std::int64_t, std::int64_t>> measured;
        try {
            measured = usage();
        } catch (const std::exception& error) {
            kLog.warning("Couldn't measure saved recordings ({})", error.what());
        }
        if (!measured) {
            return;
        }
        mailbox->deliver(gate, token, [measured](RecordingPage& page) {
            page.usageLabel_->setText(
                q(appcore::recordings_usage_text(measured->first, measured->second)));
        });
    });
}

bool RecordingPage::eventFilter(QObject* watched, QEvent* event) {
    if (watched == &dialog_ && event->type() == QEvent::Close && retentionTimer_->isActive()) {
        commitRetention();
    }
    return QWidget::eventFilter(watched, event);
}

// --- live preview ---

void RecordingPage::onStreamingToggled(bool checked) {
    std::vector<std::string> drops;
    for (const std::string_view key : keys::legacy_streaming) {
        drops.emplace_back(key);
    }
    Json updates = Json::object();
    updates[std::string(keys::streaming_enabled.key())] = checked;
    if (!dialog_.persistMany(updates, std::move(drops))) {
        return;
    }
    Q_EMIT signals_.streamingSettingsChanged();
}

} // namespace fv
