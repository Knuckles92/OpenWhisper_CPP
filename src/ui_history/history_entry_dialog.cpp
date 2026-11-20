#include "ui_history/history_entry_dialog.h"

#include <exception>
#include <format>
#include <string>
#include <utility>
#include <vector>

#include <QApplication>
#include <QButtonGroup>
#include <QClipboard>
#include <QFont>
#include <QFrame>
#include <QGridLayout>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QHideEvent>
#include <QKeySequence>
#include <QLabel>
#include <QMessageBox>
#include <QPushButton>
#include <QShortcut>
#include <QStyle>
#include <QTextEdit>
#include <QVBoxLayout>

#include "agents/installed_agents.h"
#include "base/executor.h"
#include "base/log.h"
#include "base/utf8.h"
#include "history/format_utils.h"
#include "history/manager.h"
#include "pyport/text.h"
#include "ui_history/detail/background.h"
#include "ui_history/history_playback.h"
#include "ui_history/history_store.h"
#include "ui_history/history_text.h"
#include "ui_history/manual_cleanup_dialog.h"
#include "ui_theme/local_style.h"
#include "ui_widgets/buttons.h"

namespace fv {
namespace {

constexpr log::Logger kLog{"ui_qt.dialogs.history_entry_dialog"};

// _DIALOG_STYLE.
const QString kDialogStyle = QStringLiteral(R"qss(
    QFrame#historyEntrySection {
        background-color: rgba(@surface-rgb, 0.55);
        border: 1px solid rgba(@overlay-rgb, 0.06);
        border-radius: 10px;
    }
    QLabel#historyEntryTitle {
        color: @text;
        background-color: transparent;
        font-size: 18px;
        font-weight: 700;
    }
    QLabel#historyEntryChip {
        color: @accent-soft;
        background-color: rgba(@accent-rgb, 0.12);
        border: 1px solid rgba(@accent-rgb, 0.25);
        border-radius: 6px;
        padding: 3px 8px;
        font-size: 10px;
        font-weight: 600;
    }
    QLabel#historyEntryCleanupChip {
        color: @success-text;
        background-color: rgba(@success-rgb, 0.12);
        border: 1px solid rgba(@success-rgb, 0.28);
        border-radius: 6px;
        padding: 3px 8px;
        font-size: 10px;
        font-weight: 600;
    }
    QLabel#historyEntryQuietChip {
        color: @text-secondary-strong;
        background-color: rgba(@overlay-rgb, 0.06);
        border: 1px solid rgba(@overlay-rgb, 0.10);
        border-radius: 6px;
        padding: 3px 8px;
        font-size: 10px;
        font-weight: 600;
    }
    QLabel#historyEntryKindChip {
        color: @warning-text;
        background-color: rgba(@warning-rgb, 0.12);
        border: 1px solid rgba(@warning-rgb, 0.28);
        border-radius: 6px;
        padding: 3px 8px;
        font-size: 10px;
        font-weight: 600;
    }
    QLabel#historyPlayTime {
        color: @text-tertiary;
        background-color: transparent;
        border: none;
        font-size: 11px;
    }
    QLabel#historyEntrySectionTitle {
        color: @text;
        background-color: transparent;
        border: none;
        font-size: 12px;
        font-weight: 700;
    }
    QLabel#historyEntryFactLabel {
        color: @text-secondary;
        background-color: transparent;
        border: none;
        font-size: 10px;
        font-weight: 600;
    }
    QLabel#historyEntryFactValue {
        color: @text-body;
        background-color: transparent;
        border: none;
        font-size: 11px;
    }
)qss");

QString text(const std::string& utf8) {
    return qt::toQString(utf8);
}

bool copyToClipboard(const QString& value) {
    QClipboard* clipboard = QGuiApplication::clipboard();
    if (clipboard == nullptr) {
        return false;
    }
    clipboard->setText(value);
    return true;
}

} // namespace

HistoryEntryDialog::HistoryEntryDialog(HistoryEntry entry, std::shared_ptr<HistoryStore> store,
                                       Executor& io, QWidget* parent,
                                       std::optional<bool> agentServiceAvailable)
    : QDialog(parent), copyText(&copyToClipboard), cleanupRunner(&run_manual_cleanup),
      runCleanupDialog([](ManualCleanupDialog& dialog) { dialog.exec(); }),
      runMessageBox([](QMessageBox& box) { return box.exec(); }), entry_(std::move(entry)),
      store_(std::move(store)), io_(io), ui_(std::make_shared<qt::QtUiExecutor>(this)) {
    agentCleanupAvailable_ =
        agentServiceAvailable.value_or(agents::installed_agents_if_set() != nullptr);
    failedRecording_ = entry_.entry_kind == FAILED_RECORDING_KIND;
    readVersions(entry_);
    showingRaw_ = version_ == ORIGINAL_VERSION;
    if (entry_.audio_file && !entry_.audio_file->empty() &&
        !(entry_.stored_on && !entry_.stored_on->empty())) {
        audioPath_ = store_->get_recording_path(*entry_.audio_file);
    }

    setWindowTitle(failedRecording_ ? QStringLiteral("Transcription failed")
                                    : QStringLiteral("Transcription"));
    setModal(true);
    setMinimumSize(640, 600);
    resize(720, 700);
    ui::setLocalStyleSheet(this, kDialogStyle);

    setupUi();

    // Ctrl+C copies the version shown.
    auto* shortcut = new QShortcut(QKeySequence::Copy, this);
    shortcut->setContext(Qt::WindowShortcut);
    connect(shortcut, &QShortcut::activated, this, &HistoryEntryDialog::copyShownText);
}

HistoryEntryDialog::~HistoryEntryDialog() = default;

void HistoryEntryDialog::setupUi() {
    auto* outer = new QVBoxLayout(this);
    outer->setContentsMargins(20, 18, 20, 16);
    outer->setSpacing(12);

    auto* header = new QHBoxLayout;
    header->setSpacing(10);
    auto* title = new QLabel(text(entry_.formatted_timestamp()));
    title->setObjectName(QStringLiteral("historyEntryTitle"));
    title->setAccessibleName(QStringLiteral("Transcription timestamp"));
    header->addWidget(title);
    header->addStretch();

    auto* modelChip = new QLabel(text(format_model_name(entry_.model)));
    modelChip->setObjectName(QStringLiteral("historyEntryChip"));
    modelChip->setToolTip(text(entry_.model));
    modelChip->setAlignment(Qt::AlignCenter);
    header->addWidget(modelChip);

    const std::string kindText = kind_label(entry_);
    if (!kindText.empty()) {
        auto* kindChip = new QLabel(text(kindText));
        kindChip->setObjectName(QStringLiteral("historyEntryKindChip"));
        kindChip->setAlignment(Qt::AlignCenter);
        header->addWidget(kindChip);
    }

    cleanupChip_ = new QLabel;
    cleanupChip_->setAlignment(Qt::AlignCenter);
    header->addWidget(cleanupChip_);
    refreshCleanupChip();

    const LocationChip location = location_chip(entry_);
    if (!location.text.empty()) {
        auto* locationChip = new QLabel(text(location.text));
        locationChip->setObjectName(QStringLiteral("historyEntryCleanupChip"));
        locationChip->setAlignment(Qt::AlignCenter);
        locationChip->setToolTip(text(location.tooltip));
        header->addWidget(locationChip);
    }

    outer->addLayout(header);

    if (QFrame* metadata = buildMetadataSection()) {
        outer->addWidget(metadata);
    }

    auto* body = new QFrame;
    body->setObjectName(QStringLiteral("historyEntrySection"));
    auto* bodyLayout = new QVBoxLayout(body);
    bodyLayout->setContentsMargins(14, 12, 14, 12);
    bodyLayout->setSpacing(8);

    auto* bodyHeader = new QHBoxLayout;
    bodyHeader->setContentsMargins(0, 0, 0, 0);
    bodyHeader->setSpacing(8);
    auto* sectionTitle = new QLabel(QStringLiteral("Transcript"));
    sectionTitle->setObjectName(QStringLiteral("historyEntrySectionTitle"));
    bodyHeader->addWidget(sectionTitle);
    bodyHeader->addStretch();

    versionToggle_ = new QWidget;
    auto* versionRow = new QHBoxLayout(versionToggle_);
    versionRow->setContentsMargins(0, 0, 0, 0);
    versionRow->setSpacing(6);
    versionGroup_ = new QButtonGroup(this);
    rawButton_ = new QPushButton(QStringLiteral("Original"));
    rawButton_->setToolTip(QStringLiteral("What you said, before AI cleanup"));
    fixedButton_ = new QPushButton(QStringLiteral("AI"));
    fixedButton_->setToolTip(QStringLiteral("The text after AI cleanup"));
    for (QPushButton* button : {rawButton_, fixedButton_}) {
        button->setCheckable(true);
        button->setCursor(Qt::PointingHandCursor);
        button->setObjectName(QStringLiteral("transcriptVersionBtn"));
        button->setMinimumHeight(28);
        versionGroup_->addButton(button);
        versionRow->addWidget(button);
    }
    (showingRaw_ ? rawButton_ : fixedButton_)->setChecked(true);
    connect(fixedButton_, &QPushButton::toggled, this, &HistoryEntryDialog::onVersionToggled);
    connect(rawButton_, &QPushButton::toggled, this, &HistoryEntryDialog::onVersionToggled);
    versionToggle_->setVisible(rawText_.has_value());

    useVersionButton_ = neutralButton(new Button(QStringLiteral("Use this version")));
    useVersionButton_->setBaseMinimumSize(0, 28);
    useVersionButton_->setToolTip(
        QStringLiteral("Show this version in History, search and exports"));
    connect(useVersionButton_, &QPushButton::clicked, this, &HistoryEntryDialog::useShownVersion);
    bodyHeader->addWidget(useVersionButton_);
    bodyHeader->addWidget(versionToggle_);
    bodyLayout->addLayout(bodyHeader);

    transcriptText_ = new QTextEdit;
    transcriptText_->setObjectName(QStringLiteral("historyEntryTranscript"));
    transcriptText_->setReadOnly(true);
    transcriptText_->setFont(QFont(QStringLiteral("Segoe UI"), 13));
    transcriptText_->setText(failedRecording_ ? text(failed_recording_hint(audioPath_.has_value()))
                                              : shownText());
    transcriptText_->setMinimumHeight(240);
    bodyLayout->addWidget(transcriptText_, 1);

    playButton_ = neutralButton(new Button(QStringLiteral("Play")));
    playback_ = new PlaybackControl(entry_, audioPath_, playButton_, this);
    auto* playbackRow = new QHBoxLayout;
    playbackRow->setContentsMargins(0, 2, 0, 0);
    playbackRow->setSpacing(10);
    playbackRow->addWidget(playButton_);
    playbackRow->addWidget(playback_->progress(), 1, Qt::AlignVCenter);
    playbackRow->addWidget(playback_->timeLabel());
    playbackRow->addStretch(0);
    playButton_->setVisible(hadRecording());
    bodyLayout->addLayout(playbackRow);
    outer->addWidget(body, 1);
    refreshUseVersion();

    // Two rows so long labels never collide at the default width. Copy is primary, Transcribe
    // Again a warning, Delete danger, the rest neutral.
    auto* primaryActions = new QHBoxLayout;
    primaryActions->setSpacing(8);

    copyButton_ = new PrimaryButton(QStringLiteral("Copy"));
    copyButton_->setToolTip(QStringLiteral("Copy the currently shown transcript (Ctrl+C)"));
    copyButton_->setBaseMinimumSize(96, 40);
    connect(copyButton_, &QPushButton::clicked, this, &HistoryEntryDialog::copyShownText);
    copyButton_->setVisible(!failedRecording_);
    primaryActions->addWidget(copyButton_);

    copyRawButton_ = new Button(QStringLiteral("Copy Raw"));
    copyRawButton_->setToolTip(QStringLiteral("Copy what you said, before AI cleanup"));
    copyRawButton_->setBaseMinimumSize(96, 40);
    connect(copyRawButton_, &QPushButton::clicked, this, &HistoryEntryDialog::copyRawText);
    copyRawButton_->setVisible(rawText_.has_value());
    primaryActions->addWidget(copyRawButton_);

    if (!failedRecording_ && agentCleanupAvailable_) {
        agentCleanupButton_ = new Button(QStringLiteral("Clean with agent"));
        agentCleanupButton_->setVisible(true);
        agentCleanupButton_->setToolTip(
            QStringLiteral("Choose an agent and review the transcript before sending"));
        connect(agentCleanupButton_, &QPushButton::clicked, this, [this] {
            ManualCleanupDialog dialog(shownText(), io_, cleanupRunner, this);
            dialog.copyText = copyText;
            runCleanupDialog(dialog);
        });
        primaryActions->addWidget(agentCleanupButton_);
    }

    retranscribeButton_ = new Button(failedRecording_ ? QStringLiteral("Retry transcription")
                                                      : QStringLiteral("Transcribe Again"));
    retranscribeButton_->setObjectName(QStringLiteral("warningButton"));
    retranscribeButton_->setToolTip(
        QStringLiteral("Run transcription again using the current AI cleanup setting"));
    retranscribeButton_->setBaseMinimumSize(96, 40);
    connect(retranscribeButton_, &QPushButton::clicked, this, &HistoryEntryDialog::onRetranscribe);
    retranscribeButton_->setVisible(audioPath_.has_value());
    primaryActions->addWidget(retranscribeButton_);
    primaryActions->addStretch();
    outer->addLayout(primaryActions);

    auto* dismissActions = new QHBoxLayout;
    dismissActions->setSpacing(8);
    dismissActions->addStretch();

    deleteButton_ = new DangerButton(QStringLiteral("Delete"));
    deleteButton_->setBaseMinimumSize(96, 40);
    connect(deleteButton_, &QPushButton::clicked, this, &HistoryEntryDialog::onDelete);
    dismissActions->addWidget(deleteButton_);

    auto* closeButton = new Button(QStringLiteral("Close"));
    closeButton->setBaseMinimumSize(96, 40);
    connect(closeButton, &QPushButton::clicked, this, &QDialog::accept);
    dismissActions->addWidget(closeButton);
    outer->addLayout(dismissActions);
}

QFrame* HistoryEntryDialog::buildMetadataSection() {
    std::vector<std::pair<QString, QString>> facts;
    if (entry_.audio_duration) {
        facts.emplace_back(QStringLiteral("Audio duration"),
                           text(format_audio_duration(*entry_.audio_duration)));
    }
    if (entry_.transcription_time) {
        facts.emplace_back(QStringLiteral("Transcription time"),
                           text(format_audio_duration(*entry_.transcription_time)));
    }
    if (entry_.file_size) {
        facts.emplace_back(QStringLiteral("File size"),
                           text(format_file_size(static_cast<double>(*entry_.file_size))));
    }
    if (!entry_.model.empty()) {
        facts.emplace_back(QStringLiteral("Model"), text(entry_.model));
    }
    const std::string appName = py::strip(entry_.app_name.value_or(""));
    if (!appName.empty()) {
        facts.emplace_back(QStringLiteral("App"), text(appName));
    }
    if (facts.empty()) {
        return nullptr;
    }

    auto* frame = new QFrame;
    frame->setObjectName(QStringLiteral("historyEntrySection"));
    auto* layout = new QVBoxLayout(frame);
    layout->setContentsMargins(14, 12, 14, 12);
    layout->setSpacing(8);

    auto* heading = new QLabel(QStringLiteral("Details"));
    heading->setObjectName(QStringLiteral("historyEntrySectionTitle"));
    layout->addWidget(heading);

    auto* grid = new QGridLayout;
    grid->setContentsMargins(0, 0, 0, 0);
    grid->setHorizontalSpacing(16);
    grid->setVerticalSpacing(8);
    grid->setColumnStretch(1, 1);

    int row = 0;
    for (const auto& [caption, value] : facts) {
        auto* captionLabel = new QLabel(caption);
        captionLabel->setObjectName(QStringLiteral("historyEntryFactLabel"));
        auto* valueLabel = new QLabel(value);
        valueLabel->setObjectName(QStringLiteral("historyEntryFactValue"));
        valueLabel->setWordWrap(true);
        valueLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
        grid->addWidget(captionLabel, row, 0);
        grid->addWidget(valueLabel, row, 1);
        factLabels_[caption] = valueLabel;
        ++row;
    }

    layout->addLayout(grid);
    return frame;
}

void HistoryEntryDialog::readVersions(const HistoryEntry& entry) {
    version_ = entry_version(entry);
    if (!version_.empty()) {
        fixedText_ = text(ai_text(entry).value_or(""));
        rawText_ = entry.raw_text ? std::optional<QString>(text(*entry.raw_text)) : std::nullopt;
    } else {
        fixedText_ = text(entry.text);
        rawText_.reset();
    }
}

bool HistoryEntryDialog::hadRecording() const {
    if (entry_.stored_on && !entry_.stored_on->empty()) {
        return entry_.remote_audio;
    }
    return entry_.audio_file && !entry_.audio_file->empty();
}

void HistoryEntryDialog::refreshCleanupChip() {
    QLabel* chip = cleanupChip_;
    if (entry_was_cleaned(entry_)) {
        chip->setObjectName(QStringLiteral("historyEntryCleanupChip"));
        chip->setText(QStringLiteral("✦ ") + text(format_cleanup_info(entry_)));
        chip->setToolTip(text(cleanup_tooltip(entry_)));
        chip->show();
    } else if (version_ == ORIGINAL_VERSION) {
        chip->setObjectName(QStringLiteral("historyEntryQuietChip"));
        chip->setText(QStringLiteral("Original"));
        chip->setToolTip(QStringLiteral("AI edit undone: this is what you said"));
        chip->show();
    } else {
        chip->hide();
    }
    // The object name picks the chip's rule in the sheet.
    chip->style()->unpolish(chip);
    chip->style()->polish(chip);
}

QString HistoryEntryDialog::shownVersion() const {
    return text(std::string(showingRaw_ ? ORIGINAL_VERSION : AI_VERSION));
}

void HistoryEntryDialog::refreshUseVersion() {
    useVersionButton_->setVisible(!version_.empty() && is_local_entry(entry_) &&
                                  qt::toUtf8(shownVersion()) != version_);
}

void HistoryEntryDialog::onVersionToggled(bool checked) {
    if (!checked) {
        return;
    }
    const bool showRaw = rawButton_->isChecked();
    showingRaw_ = showRaw;
    if (showRaw && rawText_) {
        transcriptText_->setText(*rawText_);
    } else {
        transcriptText_->setText(fixedText_);
    }
    refreshUseVersion();
}

void HistoryEntryDialog::useShownVersion() {
    history_detail::runInBackground(
        io_, ui_,
        [store = store_, id = entry_.id, saved = onVersionSaved,
         version = qt::toUtf8(shownVersion())]() -> std::optional<HistoryEntry> {
            try {
                auto updated = store->use_version(id, version);
                if (updated && saved) {
                    saved();
                }
                return updated;
            } catch (const std::exception& error) {
                kLog.error_with(error, std::format("Could not change a History entry's "
                                                   "version: {}",
                                                   error.what()));
                return std::nullopt;
            }
        },
        [this](std::optional<HistoryEntry> updated) {
            if (!updated) {
                useVersionButton_->hide();
                return;
            }
            entry_ = std::move(*updated);
            readVersions(entry_);
            refreshCleanupChip();
            refreshUseVersion();
            Q_EMIT versionChanged(text(entry_.id));
        });
}

QString HistoryEntryDialog::shownText() const {
    if (showingRaw_ && rawText_) {
        return *rawText_;
    }
    return fixedText_;
}

void HistoryEntryDialog::copyShownText() {
    if (failedRecording_) {
        return;
    }
    if (copyText(shownText())) {
        Q_EMIT copied();
        kLog.info("Copied history entry transcript from dialog");
    } else {
        kLog.error("Failed to copy transcript from dialog: the clipboard is not available");
    }
}

void HistoryEntryDialog::copyRawText() {
    if (!rawText_ || rawText_->isEmpty()) {
        return;
    }
    if (copyText(*rawText_)) {
        Q_EMIT copied();
        kLog.info("Copied raw history entry transcript from dialog");
    } else {
        kLog.error("Failed to copy raw transcript from dialog: the clipboard is not available");
    }
}

void HistoryEntryDialog::done(int result) {
    playback_->stop();
    QDialog::done(result);
}

void HistoryEntryDialog::hideEvent(QHideEvent* event) {
    if (!event->spontaneous()) {
        playback_->stop();
    }
    QDialog::hideEvent(event);
}

void HistoryEntryDialog::onRetranscribe() {
    if (!audioPath_) {
        return;
    }
    Q_EMIT retranscribeRequested(text(path_to_utf8(*audioPath_)));
    accept();
}

void HistoryEntryDialog::onDelete() {
    const QString message =
        entry_.stored_on && !entry_.stored_on->empty()
            ? tr("Delete this transcription from %1?\n\nIts recording there will also be deleted.")
                  .arg(text(*entry_.stored_on))
            : tr("Delete this transcription from history?\n\nThe saved recording file (if any) "
                 "will be kept.");
    QMessageBox question(QMessageBox::Question, tr("Delete Entry"), message,
                         QMessageBox::Yes | QMessageBox::No, this);
    question.setTextFormat(Qt::PlainText);
    question.setDefaultButton(QMessageBox::No);
    if (runMessageBox(question) != QMessageBox::Yes) {
        return;
    }
    Q_EMIT deleteRequested(text(entry_.id));
    accept();
}

} // namespace fv
