#include "ui_record/quick_record_tab.h"

#include <algorithm>
#include <cmath>

#include <QAbstractTextDocumentLayout>
#include <QCheckBox>
#include <QComboBox>
#include <QEvent>
#include <QHBoxLayout>
#include <QPushButton>
#include <QTextCursor>
#include <QTextDocument>
#include <QTextEdit>
#include <QVBoxLayout>

#include "appkit/destinations.h"
#include "base/log.h"
#include "hotkeys/grammar.h"
#include "settings/keys.h"
#include "settings/store.h"
#include "ui_record/cleanup_profile_card.h"
#include "ui_record/transcript_pane.h"
#include "ui_theme/icons.h"
#include "ui_widgets/agent_lookup.h"
#include "ui_widgets/buttons.h"
#include "ui_widgets/cards.h"
#include "ui_widgets/wrapped_label.h"

namespace fv {
namespace {

constexpr log::Logger kLog{"ui_qt.widgets.quick_record_tab"};

// format_hotkey_display for a stored shortcut: "" when there is none.
QString displayHotkey(const std::string& hotkey) {
    if (hotkey.empty()) {
        return {};
    }
    return QString::fromStdString(
        hotkeys::format_hotkey_display(hotkey, hotkeys::platform_vocabulary()));
}

// Prefers the height of the wrapped text, with scrolling for long results. It declares no signal
// or property, so it stays a QTextEdit to the theme.
class QuickRecordTranscript : public QTextEdit {
public:
    static constexpr int kPreviewLines = 12;

    QuickRecordTranscript() {
        connect(document()->documentLayout(), &QAbstractTextDocumentLayout::documentSizeChanged,
                this, [this] { updateGeometry(); });
        connect(this, &QTextEdit::textChanged, this, [this] { updateGeometry(); });
    }

    QSize minimumSizeHint() const override {
        QSize size = QTextEdit::minimumSizeHint();
        size.setHeight(heightForLines(document()->isEmpty() ? 2 : 1));
        return size;
    }

    QSize sizeHint() const override {
        QSize size = QTextEdit::sizeHint();
        const int padding = height() - contentsRect().height();
        const int naturalHeight =
            static_cast<int>(std::ceil(document()->size().height())) + padding;
        size.setHeight(std::max(minimumSizeHint().height(),
                                std::min(naturalHeight, heightForLines(kPreviewLines))));
        return size;
    }

private:
    int heightForLines(int lines) const {
        const int padding = height() - contentsRect().height();
        const int textHeight = lines * fontMetrics().lineSpacing();
        const double margins = 2 * document()->documentMargin();
        return static_cast<int>(std::ceil(textHeight + margins)) + padding;
    }
};

TranscriptionTabKnobs quickRecordKnobs() {
    TranscriptionTabKnobs knobs;
    knobs.contentObjectName = QStringLiteral("quickRecordContent");
    knobs.initialStatus = QStringLiteral("Ready to record");
    knobs.transcriptMinHeight = 0;
    knobs.transcriptPlaceholder =
        QStringLiteral("Transcription will appear here...\nStart recording to begin.");
    knobs.makeTranscriptEditor = [] {
        return new QuickRecordTranscript;
    };
    return knobs;
}

} // namespace

QuickRecordTab::QuickRecordTab(SettingsStore& settings, LocalEngineCatalog catalog, QWidget* parent)
    : TranscriptionTabBase(settings, std::move(catalog), quickRecordKnobs(), parent),
      readiness_([](const Json&) { return std::optional<bool>(false); }) {
    initialize();
    transcriptionCard()->cardLayout()->setContentsMargins(16, 8, 16, 8);

    copyIcon_ = ui::tablerIcon(QStringLiteral("copy-gray.svg"));
    copiedIcon_ = ui::tablerIcon(QStringLiteral("check-green.svg"));
    copyButton_ = new QPushButton;
    collapsedCopyButton_ = new QPushButton(QStringLiteral("Copy"));
    for (QPushButton* button : {copyButton_, collapsedCopyButton_}) {
        button->setObjectName(QStringLiteral("transcriptCopyButton"));
        button->setIconSize(QSize(16, 16));
        button->setFixedHeight(26);
        button->setCursor(Qt::PointingHandCursor);
        button->setAccessibleName(QStringLiteral("Copy transcript"));
        connect(button, &QPushButton::clicked, this, [this] { requestCopy(); });
    }
    copyButton_->setFixedWidth(26);
    transcriptPane()->setCornerWidget(copyButton_);
    transcriptionCard()->headerLayout()->insertWidget(2, collapsedCopyButton_);
    copyFeedbackTimer_ = new QTimer(this);
    copyFeedbackTimer_->setSingleShot(true);
    copyFeedbackTimer_->setInterval(kCopyFeedbackMs);
    connect(copyFeedbackTimer_, &QTimer::timeout, this, [this] { resetCopyFeedback(); });
    connect(transcriptText(), &QTextEdit::textChanged, this, [this] { syncCopyActions(); });
    connect(transcriptionCard(), &HeaderCard::toggled, this, [this](bool) { syncCopyActions(); });
    syncCopyActions();
}

void QuickRecordTab::setTranscriptionCollapsed(bool collapsed) {
    TranscriptionTabBase::setTranscriptionCollapsed(collapsed);
    if (copyButton_ != nullptr) {
        syncCopyActions();
    }
}

void QuickRecordTab::syncCopyActions() {
    copyFeedbackTimer_->stop();
    resetCopyFeedback();
    const bool hasText = !shownTranscript().trimmed().isEmpty();
    copyButton_->setEnabled(hasText);
    collapsedCopyButton_->setVisible(hasText && isTranscriptionCollapsed());
    if (isTranscriptionCollapsed()) {
        transcriptionCard()->setMaximumHeight(transcriptionCard()->sizeHint().height());
    }
}

void QuickRecordTab::requestCopy() {
    const QString text = shownTranscript();
    if (!text.trimmed().isEmpty()) {
        Q_EMIT copyRequested(text);
    }
}

void QuickRecordTab::showCopyResult(bool succeeded) {
    for (QPushButton* button : {copyButton_, collapsedCopyButton_}) {
        button->setIcon(succeeded ? copiedIcon_ : copyIcon_);
        button->setToolTip(succeeded ? QStringLiteral("Copied to clipboard")
                                     : QStringLiteral("Copy failed. Try again."));
    }
    collapsedCopyButton_->setText(succeeded ? QStringLiteral("Copied")
                                            : QStringLiteral("Retry copy"));
    copyFeedbackTimer_->start();
}

void QuickRecordTab::resetCopyFeedback() {
    for (QPushButton* button : {copyButton_, collapsedCopyButton_}) {
        button->setIcon(copyIcon_);
        button->setToolTip(QStringLiteral("Copy transcript"));
    }
    collapsedCopyButton_->setText(QStringLiteral("Copy"));
}

void QuickRecordTab::buildContentBeforeStatus(QVBoxLayout* layout) {
    profileCard_ = new CleanupProfileCard;
    layout->addWidget(profileCard_);
    // Outside the profile card: Command Mode needs an AI model, not the cleanup switch the card
    // follows. Built here, so the hint refresh below finds it, and placed centred under the
    // record buttons.
    commandRow_ = new QWidget;
    auto* command = new QVBoxLayout(commandRow_);
    command->setContentsMargins(0, 0, 0, 0);
    command->setSpacing(0);
    commandHint_ = new WrappedLabel;
    commandHint_->setTextFormat(Qt::PlainText);
    commandHint_->setObjectName(QStringLiteral("infoLabel"));
    commandHint_->setAlignment(Qt::AlignCenter);
    command->addWidget(commandHint_);
    commandLink_ = new QPushButton;
    commandLink_->setObjectName(QStringLiteral("commandModeHintLink"));
    commandLink_->setCursor(Qt::PointingHandCursor);
    connect(commandLink_, &QPushButton::clicked, this, [this] {
        Q_EMIT settingsRequested(
            QString::fromUtf8(destinations::commands.data(), destinations::commands.size()));
    });
    command->addWidget(commandLink_, 0, Qt::AlignHCenter);
    connect(profileCard_, &CleanupProfileCard::profileChosen, this, [this] { onProfileChanged(); });
    connect(profileCard_, &CleanupProfileCard::manageRequested, this,
            [this] { Q_EMIT profilesRequested(); });
    // lookup_relay().looked_up; there is no relay until the installed agents are set.
    if (AgentLookupRelay* relay = agentLookupRelay()) {
        connect(relay, &AgentLookupRelay::lookedUp, this, &QuickRecordTab::onAgentLookedUp);
    }
    refreshCleanupProfiles();
}

CleanupProfileCard* QuickRecordTab::profileCard() const {
    return profileCard_;
}

QComboBox* QuickRecordTab::profileCombo() const {
    return profileCard_ != nullptr ? profileCard_->combo() : nullptr;
}

WrappedLabel* QuickRecordTab::profileHint() const {
    return profileCard_ != nullptr ? profileCard_->hint() : nullptr;
}

QString QuickRecordTab::selectedCleanupProfileId() const {
    return profileCard_ != nullptr ? profileCard_->selectedId() : QString();
}

const text::CleanupProfile* QuickRecordTab::profileById(const QString& id) const {
    const std::string wanted = id.toStdString();
    for (const text::CleanupProfile& profile : cleanupProfiles_) {
        if (profile.id == wanted) {
            return &profile;
        }
    }
    return nullptr;
}

void QuickRecordTab::refreshCleanupProfiles() {
    const Json snapshot = *settings().snapshot();
    QString selected = QString::fromStdString(keys::quick_record_profile.read(snapshot));
    if (isRecording_) {
        selected = activeProfile_ ? QString::fromStdString(activeProfile_->id) : QString();
    }
    cleanupProfiles_ = text::load_cleanup_profiles(snapshot);
    if (isRecording_ && activeProfile_) {
        std::erase_if(cleanupProfiles_, [this](const text::CleanupProfile& profile) {
            return profile.id == activeProfile_->id;
        });
        cleanupProfiles_.push_back(*activeProfile_);
    }
    if (profileCard_ != nullptr) {
        profileCard_->setProfiles(cleanupProfiles_, selected);
    }
    refreshProfileHint();
}

void QuickRecordTab::setRecordingProfile(const QString& id) {
    std::optional<text::CleanupProfile> profile;
    if (!id.isEmpty()) {
        profile = text::find_cleanup_profile(*settings().snapshot(), id.toStdString());
    }
    setRecordingProfile(std::move(profile));
}

void QuickRecordTab::setRecordingProfile(std::optional<text::CleanupProfile> profile) {
    activeProfile_ = std::move(profile);
}

void QuickRecordTab::onProfileChanged() {
    try {
        settings().save_setting(keys::quick_record_profile.key(),
                                selectedCleanupProfileId().toStdString());
    } catch (const std::exception& error) {
        kLog.error_with(error, "Couldn't save the Quick Record cleanup profile");
    }
    refreshProfileHint();
}

void QuickRecordTab::refreshCommandHint() {
    if (commandLink_ == nullptr) {
        return;
    }
    const Json snapshot = *settings().snapshot();
    Json saved = keys::hotkeys.read(snapshot);
    std::string commandKey;
    if (saved.is_object()) {
        const auto found = saved.find("command_mode");
        if (found != saved.end() && found->is_string()) {
            commandKey = hotkeys::format_hotkey_display(found->get<std::string>(),
                                                        hotkeys::platform_vocabulary());
        }
    }
    // Until the provider's lookup lands, assume it still works.
    const std::optional<bool> known = readiness_(snapshot);
    const bool ready = !(known && !*known);
    const QString key = QString::fromStdString(commandKey);
    commandHint_->setText(
        QStringLiteral(u"Command Mode · %1 · Select text, then say how to change it.").arg(key));
    commandHint_->setVisible(!key.isEmpty() && ready);
    commandLink_->setText(ready ? QStringLiteral(u"Set a Command Mode shortcut  →")
                                : QStringLiteral(u"Set up Command Mode  →"));
    commandLink_->setToolTip(ready ? QStringLiteral("Rewrite selected text by voice")
                                   : QStringLiteral("Command Mode needs an AI cleanup model, "
                                                    "even while cleanup is off"));
    commandLink_->setVisible(!(!key.isEmpty() && ready));
}

void QuickRecordTab::setCommandModeReadiness(CommandModeReadiness readiness) {
    readiness_ = std::move(readiness);
    refreshCommandHint();
}

void QuickRecordTab::onAgentLookedUp(const QString& /*agentId*/) {
    refreshCommandHint();
}

// The AI model is set up in Settings, a separate window; coming back to this one is when the
// hint can have gone stale.
bool QuickRecordTab::event(QEvent* event) {
    if (event->type() == QEvent::WindowActivate && commandLink_ != nullptr) {
        refreshCommandHint();
    }
    return TranscriptionTabBase::event(event);
}

// A chosen profile always cleans up: the checkbox shows it checked and cannot be changed, and
// the card follows the checkbox.
void QuickRecordTab::loadCleanupSetting() {
    TranscriptionTabBase::loadCleanupSetting();
    const bool selected = !selectedCleanupProfileId().isEmpty();
    if (selected) {
        const QSignalBlocker blocker(cleanupCheck());
        cleanupCheck()->setChecked(true);
    }
    cleanupCheck()->setEnabled(!selected);
    if (profileCard_ != nullptr) {
        profileCard_->setVisible(cleanupCheck()->isChecked());
    }
}

void QuickRecordTab::refreshProfileHint() {
    const text::CleanupProfile* profile = profileById(selectedCleanupProfileId());
    QString hint;
    if (profile != nullptr) {
        const QString shortcut = displayHotkey(profile->hotkey);
        hint = QStringLiteral(u"Always uses AI cleanup · ") +
               (shortcut.isEmpty() ? QStringLiteral("Click Start Recording")
                                   : QStringLiteral("Record with %1").arg(shortcut));
        if (QComboBox* combo = profileCombo()) {
            combo->setToolTip(QString::fromStdString(profile->instructions));
        }
    } else {
        hint = QStringLiteral("Uses your usual cleanup settings and standard recording shortcut.");
        if (QComboBox* combo = profileCombo()) {
            combo->setToolTip(QStringLiteral("Standard dictation"));
        }
    }
    if (isRecording_) {
        hint = QStringLiteral("Recording with %1")
                   .arg(profile != nullptr ? QString::fromStdString(profile->name)
                                           : QStringLiteral("Standard dictation"));
    }
    if (WrappedLabel* label = profileHint()) {
        label->setText(hint);
    }
    refreshCommandHint();
    loadCleanupSetting();
    if (recordButton_ != nullptr) {
        const QString shortcut =
            profile != nullptr ? displayHotkey(profile->hotkey) : standardRecordKey_;
        recordButton_->setHotkey(shortcut);
        stopButton_->setHotkey(shortcut);
    }
}

void QuickRecordTab::buildContentAfterStatus(QVBoxLayout* layout) {
    auto* controlPanel = new ControlPanel;
    // The content layout supplies vertical separation between the buttons and the transcript;
    // keep the minimum-height window usable.
    controlPanel->panelLayout()->setContentsMargins(16, 0, 16, 0);
    controlPanel->panelLayout()->setSpacing(12);

    recordButton_ = new SuccessButton(QStringLiteral("Start Recording"));
    cancelButton_ = new WarningButton(QStringLiteral("Cancel"));
    cancelButton_->setActive(false);
    stopButton_ = new DangerButton(QStringLiteral("Stop"));
    stopButton_->setActive(false);

    auto* buttonsWidget = new QWidget;
    auto* buttonsLayout = new QVBoxLayout(buttonsWidget);
    buttonsLayout->setContentsMargins(0, 0, 0, 0);
    buttonsLayout->setSpacing(12);

    auto* topRow = new QHBoxLayout;
    topRow->setSpacing(12);
    topRow->addWidget(recordButton_, 1);

    auto* bottomRow = new QHBoxLayout;
    bottomRow->setSpacing(12);
    bottomRow->addWidget(stopButton_, 1);
    bottomRow->addWidget(cancelButton_, 1);

    buttonsLayout->addLayout(topRow);
    buttonsLayout->addLayout(bottomRow);
    buttonsLayout->addWidget(commandRow_);
    fitStartWidth(buttonsWidget);

    controlPanel->panelLayout()->addStretch();
    controlPanel->panelLayout()->addWidget(buttonsWidget, 1);
    controlPanel->panelLayout()->addStretch();

    layout->addWidget(controlPanel);
}

void QuickRecordTab::connectSignals() {
    TranscriptionTabBase::connectSignals();
    connect(cleanupCheck(), &QCheckBox::toggled, profileCard_, &QWidget::setVisible);
    connect(recordButton_, &QPushButton::clicked, this, [this] { onRecordClicked(); });
    // Request a stop; the button state waits for the recording state change.
    connect(stopButton_, &QPushButton::clicked, this, [this] { Q_EMIT recordToggled(false); });
    // Request a cancel; the chrome is reset by the cancel path.
    connect(cancelButton_, &QPushButton::clicked, this, [this] { Q_EMIT recordCanceled(); });
}

// Request a start; the recording chrome waits for a successful stream.
void QuickRecordTab::onRecordClicked() {
    recordButton_->setActive(false);
    Q_EMIT recordToggled(true);
}

void QuickRecordTab::setRecordingActive(bool recording) {
    isRecording_ = recording;
    updateRecordingState();
}

void QuickRecordTab::updateRecordingState() {
    refreshCleanupProfiles();
    if (profileCard_ != nullptr) {
        profileCard_->combo()->setEnabled(!isRecording_);
    }
    if (isRecording_) {
        recordButton_->setActive(false);
        recordButton_->setText(QStringLiteral("Recording..."));
        stopButton_->setActive(true);
        cancelButton_->setActive(true);
        setBackendEnabled(false);
        localEngine()->setBusy(true);
        // The streaming runtime refuses to reconfigure mid-recording, so the toggle would
        // persist without taking effect until the next recording. Lock it with the backend
        // choice instead.
        setLivePreviewLocked(true);
        setStatus(QStringLiteral("Recording in progress..."));
    } else {
        recordButton_->setActive(true);
        recordButton_->setText(QStringLiteral("Start Recording"));
        stopButton_->setActive(false);
        cancelButton_->setActive(false);
        setBackendEnabled(true);
        localEngine()->setBusy(false);
        setLivePreviewLocked(false);
        setStatus(QStringLiteral("Ready to record"));
    }
}

void QuickRecordTab::setPartialTranscription(const QString& text, bool isFinal) {
    if (isFinal) {
        // The incremental preview emits the full accumulated preview each cycle, so the buffer
        // is replaced, not appended to.
        partialBuffer_.clear();
        if (!text.isEmpty()) {
            partialBuffer_.append(text);
        }
    }

    QString combined = partialBuffer_.join(QLatin1Char(' '));
    if (!isFinal) {
        if (!combined.isEmpty()) {
            combined += QLatin1Char(' ');
        }
        combined += text + QStringLiteral(" ...");
    }

    setTranscript(combined);

    QTextCursor cursor = transcriptText()->textCursor();
    cursor.movePosition(QTextCursor::End);
    transcriptText()->setTextCursor(cursor);
}

void QuickRecordTab::clearPartialTranscription() {
    partialBuffer_.clear();
}

void QuickRecordTab::updateHotkeys(const QString& recordKey, const QString& cancelKey,
                                   const QString& /*enableDisableKey*/) {
    standardRecordKey_ = recordKey;
    refreshProfileHint();
    cancelButton_->setHotkey(cancelKey);
}

} // namespace fv
