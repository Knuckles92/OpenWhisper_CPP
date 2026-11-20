#include "ui_record/transcription_tab_base.h"

#include <algorithm>
#include <exception>
#include <string>
#include <string_view>
#include <vector>

#include <chrono>
#include <cmath>
#include <format>

#include <QButtonGroup>
#include <QCheckBox>
#include <QComboBox>
#include <QFont>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QPropertyAnimation>
#include <QPushButton>
#include <QScrollArea>
#include <QTextEdit>
#include <QTimer>
#include <QVBoxLayout>

#include "base/identity.h"
#include "base/log.h"
#include "models/speech_catalog.h"
#include "remote/pairing_settings.h"
#include "settings/keys.h"
#include "settings/model_selection.h"
#include "settings/store.h"
#include "speech/preview_status.h"
#include "ui_record/stats_display.h"
#include "ui_record/transcript_pane.h"
#include "ui_theme/font_scale.h"
#include "ui_theme/local_style.h"
#include "ui_theme/markdown_render.h"
#include "ui_theme/style_props.h"
#include "ui_widgets/cards.h"
#include "ui_widgets/downloads_label.h"
#include "ui_widgets/remote_engine_controls.h"
#include "ui_widgets/remote_link_glyph.h"
#include "ui_widgets/remote_model_notice.h"
#include "ui_widgets/speech_backend_picker.h"
#include "ui_widgets/speech_server_controls.h"
#include "ui_widgets/wrapped_label.h"

namespace fv {
namespace {

constexpr log::Logger kLog{"ui_qt.widgets.transcription_tab_base"};

const QString kLivePreviewTip =
    QStringLiteral("Show draft text near your cursor as you speak.\n"
                   "The final transcript may change after you stop.\n"
                   "Uses extra processing; requires a supported backend.");

const QString kBackendHelp = QStringLiteral(
    "Choose which speech recognition engine turns your audio into text. "
    "Local engines process audio on this computer after download. "
    "API sends audio to the server you choose: OpenAI, Groq, or a speech server you run. "
    "This choice determines the available models.");

// Engine-wide busy lines the Remote engine's link replaces with its own.
const QString kLoadingEngine = QStringLiteral("Loading speech engine...");
const QString kReloadingEngine = QStringLiteral("Reloading speech engine...");

QString fromStd(const std::string& text) {
    return QString::fromStdString(text);
}

// What the host's engine status adds to the link tooltip's first line. A loaded engine reports
// "model | device (compute)", which that line already says; only a note after it, such as a GPU
// fallback, is news.
QString hostStatusNote(const QString& status) {
    const QString separator = QStringLiteral(u" — ");
    const qsizetype at = status.indexOf(separator);
    const QString summary = at < 0 ? status : status.left(at);
    const QString note = at < 0 ? QString() : status.mid(at + separator.size()).trimmed();
    return summary.contains(QLatin1String(" | ")) ? note : status;
}

const QString kCleanupTip = QStringLiteral("On-demand dictation only; not walkthroughs.\n"
                                           "Use your AI model to fix punctuation, remove\n"
                                           "filler words, and apply your cleanup rules.\n"
                                           "May change wording and adds processing time.");

// The status messages an overlay state shows, if it shows one.
std::optional<QString> activityMessage(appcore::OverlayState state) {
    using appcore::OverlayState;
    switch (state) {
    case OverlayState::Recording:
        return QStringLiteral("Recording...");
    case OverlayState::CommandListening:
        return QStringLiteral(u"Listening for an edit…");
    case OverlayState::Processing:
        return QStringLiteral("Processing...");
    case OverlayState::Transcribing:
        return QStringLiteral("Transcribing...");
    case OverlayState::Cleaning:
        return QStringLiteral("Cleaning up...");
    case OverlayState::Rewriting:
        return QStringLiteral(u"Rewriting…");
    case OverlayState::Canceling:
        return QStringLiteral("Canceling...");
    case OverlayState::None:
    case OverlayState::SttEnabled:
    case OverlayState::SttDisabled:
        break;
    }
    return std::nullopt;
}

// _has_local_fields: whether the backend runs here, so its model, device and language apply. The
// API and Remote backends (M2, M9) have fields of their own instead.
bool hasLocalFields(const QString& backend) {
    return backend != QLatin1String("api") && backend != QLatin1String("remote");
}

bool isBusyActivity(appcore::OverlayState state) {
    using appcore::OverlayState;
    return state == OverlayState::Processing || state == OverlayState::Transcribing ||
           state == OverlayState::Cleaning || state == OverlayState::Rewriting ||
           state == OverlayState::Canceling;
}

} // namespace

const QString kRemoteModelHelp = QStringLiteral(
    "The model the paired computer transcribes with. Choosing another switches that computer to "
    "it, as if it were chosen there. Only models already downloaded there are listed.");

QString livePreviewUnavailableReason(const QString& backend) {
    if (backend == QLatin1String("api") || backend == QLatin1String("remote")) {
        return QStringLiteral("Live preview needs Parakeet");
    }
    return {};
}

TranscriptionTabBase::TranscriptionTabBase(SettingsStore& settings, LocalEngineCatalog catalog,
                                           TranscriptionTabKnobs knobs, QWidget* parent)
    : QWidget(parent), settings_(settings), catalog_(std::move(catalog)), knobs_(std::move(knobs)) {
    const auto choices = models::backend_choices();
    currentModel_ = QString::fromUtf8(choices.front().label.data(), choices.front().label.size());
    statusMessage_ = knobs_.initialStatus;
}

void TranscriptionTabBase::initialize() {
    setupUi();
    connectSignals();
    loadCleanupSetting();
    loadLivePreviewSetting();
}

int TranscriptionTabBase::backendIndex(const QString& displayName) const {
    const QString value = backendValue(displayName);
    return value.isEmpty() ? -1 : modelCombo_->findData(value);
}

// config.MODEL_VALUE_MAP: the selected_model value of a display name, "" for none.
QString TranscriptionTabBase::backendValue(const QString& displayName) const {
    for (const std::string_view id : model_values()) {
        if (backendDisplayName(id) == displayName) {
            return QString::fromUtf8(id.data(), static_cast<qsizetype>(id.size()));
        }
    }
    return {};
}

void TranscriptionTabBase::setupUi() {
    auto* mainLayout = new QVBoxLayout(this);
    mainLayout->setContentsMargins(0, 0, 0, 0);
    mainLayout->setSpacing(0);

    scrollArea_ = new QScrollArea;
    scrollArea_->setObjectName(QStringLiteral("transcriptionTabScrollArea"));
    scrollArea_->setWidgetResizable(true);
    scrollArea_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    ui::setLocalStyleSheet(scrollArea_,
                           QStringLiteral("QScrollArea#transcriptionTabScrollArea { border: none; "
                                          "background: transparent; }"));

    auto* scrollHost = new QWidget;
    scrollHost->setObjectName(QStringLiteral("transcriptionTabScrollHost"));
    auto* scrollHostLayout = new QVBoxLayout(scrollHost);
    scrollHostLayout->setContentsMargins(0, 0, 0, 0);
    scrollHostLayout->setSpacing(0);

    auto* contentContainer = new QWidget;
    contentContainer->setObjectName(knobs_.contentObjectName);
    contentLayout_ = new QVBoxLayout(contentContainer);
    contentLayout_->setContentsMargins(24, 14, 24, 16);
    contentLayout_->setSpacing(12);

    // No vertical alignment: the transcription card is the elastic element (stretch 1).
    // Horizontal centering is the wrapper's.
    auto* centerWrapper = new QHBoxLayout;
    centerWrapper->setContentsMargins(0, 0, 0, 0);
    centerWrapper->setSpacing(0);
    centerWrapper->addStretch();
    centerWrapper->addWidget(contentContainer, 1);
    centerWrapper->addStretch();

    contentContainer->setMaximumWidth(700);
    contentContainer->setMinimumWidth(500);

    scrollHostLayout->addLayout(centerWrapper);
    scrollArea_->setWidget(scrollHost);
    mainLayout->addWidget(scrollArea_);

    // Engine card: labeled fields across the full width, then a footer line carrying the
    // resolved engine and the two controls that apply to every backend. A QFrame rather than
    // Card because a plain QWidget will not paint a theme surface.
    auto* engineCard = new QFrame;
    engineCard->setObjectName(QStringLiteral("engineCard"));
    engineCard_ = engineCard;
    auto* engineLayout = new QVBoxLayout(engineCard);
    engineLayout->setContentsMargins(14, 12, 14, 12);
    engineLayout->setSpacing(10);

    modelCombo_ = static_cast<StatusFieldCombo*>(engineCombo({}, true));
    populateBackendCombo(modelCombo_, model_values());
    modelCombo_->setCurrentIndex(backendIndex(currentModel_));
    applyBackendStatus(EngineStatus::Unknown);

    localEngine_ = new LocalEngineControls(settings_, catalog_, backendValue(currentModel_),
                                           knobs_.dictationLanguages);

    auto* fieldRow = new QHBoxLayout;
    fieldRow_ = fieldRow;
    fieldRow->setContentsMargins(0, 0, 0, 0);
    fieldRow->setSpacing(10);
    fieldRow->addWidget(
        engineField(QStringLiteral("Backend"), modelCombo_, kBackendHelp,
                    {{QStringLiteral(u"Open Settings → Voice model"), QStringLiteral("ondemand")}},
                    [this](const QString& destination) { Q_EMIT helpRequested(destination); }),
        2);
    fieldRow->addWidget(localEngine_, 4);

    // The paired computer's own models. Choosing one switches that computer to it, just as
    // choosing it there would.
    remoteModelCombo_ = engineCombo({});
    remoteModelField_ = engineField(
        QStringLiteral("Model"), remoteModelCombo_, kRemoteModelHelp,
        {{QStringLiteral(u"Open Settings → Remote engine"), QStringLiteral("remote_engine")}},
        [this](const QString& destination) { Q_EMIT helpRequested(destination); });
    remoteModelField_->hide();
    remoteEngine_ = new RemoteEngineControls;
    remoteEngine_->hide();
    fieldRow->addWidget(remoteModelField_, 2);
    fieldRow->addWidget(remoteEngine_, 2);
    engineLayout->addLayout(fieldRow);

    remoteRuntimeLabel_ = new WrappedLabel(QString());
    remoteRuntimeLabel_->setObjectName(QStringLiteral("engineResolvedLabel"));
    remoteRuntimeLabel_->setTextFormat(Qt::PlainText);
    remoteRuntimeLabel_->hide();
    engineLayout->addWidget(remoteRuntimeLabel_);

    // On the footer line, so the Remote card is as tall as the others.
    manageHostModels_ = new RemoteModelNotice;
    connect(manageHostModels_, &QPushButton::clicked, this,
            [this] { Q_EMIT helpRequested(QStringLiteral("remote_models")); });
    manageHostModels_->hide();

    statusDot_ = new StatusDot(nullptr, 16);
    // Stands in for the dot while Remote is selected.
    linkGlyph_ = new RemoteLinkGlyph;
    linkGlyph_->hide();
    connect(linkGlyph_, &RemoteLinkGlyph::clicked, this, &TranscriptionTabBase::onLinkClicked);
    linkCountdown_ = new QTimer(this);
    linkCountdown_->setInterval(1000);
    connect(linkCountdown_, &QTimer::timeout, this, &TranscriptionTabBase::refreshEngineStatus);

    resolvedLabel_ = new DownloadsLabel(knobs_.initialStatus);
    resolvedLabel_->setObjectName(QStringLiteral("engineResolvedLabel"));
    connect(resolvedLabel_, &DownloadsLabel::downloadsRequested, this,
            &TranscriptionTabBase::engineDownloadsRequested);

    cleanupCheck_ = new QCheckBox(QStringLiteral("AI cleanup"));
    cleanupCheck_->setObjectName(QStringLiteral("engineCleanupCheck"));
    cleanupCheck_->setToolTip(kCleanupTip);

    livePreviewCheck_ = new QCheckBox(QStringLiteral("Live preview"));
    livePreviewCheck_->setObjectName(QStringLiteral("engineLivePreviewCheck"));
    livePreviewCheck_->setToolTip(kLivePreviewTip);
    if (!knobs_.livePreviewControl) {
        livePreviewCheck_->hide();
    }

    footerRow_ = new QHBoxLayout;
    footerRow_->setContentsMargins(0, 0, 0, 0);
    footerRow_->setSpacing(6);
    footerRow_->addWidget(statusDot_);
    footerRow_->addWidget(linkGlyph_);
    footerRow_->addWidget(resolvedLabel_, 1);
    footerRow_->addWidget(manageHostModels_);
    footerRow_->addSpacing(6);
    footerRow_->addWidget(cleanupCheck_);
    footerRow_->addSpacing(6);
    footerRow_->addWidget(livePreviewCheck_);
    engineLayout->addLayout(footerRow_);

    // Host side: which paired computers use this one's engine, slid in below the footer while
    // any is connected.
    servingRow_ = new QWidget;
    servingRow_->setObjectName(QStringLiteral("remoteServingRow"));
    auto* servingLayout = new QHBoxLayout(servingRow_);
    servingLayout->setContentsMargins(0, 2, 0, 0);
    servingLayout->setSpacing(6);
    servingGlyph_ = new RemoteLinkGlyph(nullptr, true);
    servingLabel_ = new QLabel;
    servingLabel_->setObjectName(QStringLiteral("remoteServingLabel"));
    servingLayout->addWidget(servingGlyph_);
    servingLayout->addWidget(servingLabel_, 1);
    servingRow_->setMaximumHeight(0);
    servingRow_->hide();
    servingAnim_ = createMaxHeightAnimation(servingRow_, this);
    engineLayout->addWidget(servingRow_);

    contentLayout_->addWidget(engineCard);

    buildContentBeforeStatus(contentLayout_);
    buildContentAfterStatus(contentLayout_);

    transcriptionCard_ = new HeaderCard(QStringLiteral("Transcription"), true);

    // One painted surface holds the Fixed / Raw switch and the text, and the text edit inside
    // it is borderless, so the switch reads as the corner of the box rather than a row of
    // buttons floating above it.
    transcriptPane_ = new TranscriptPane;
    transcriptPane_->setObjectName(QStringLiteral("transcriptPane"));
    auto* paneLayout = new QVBoxLayout(transcriptPane_);
    paneLayout->setContentsMargins(0, 0, 0, 0);
    paneLayout->setSpacing(0);

    versionToggle_ = new QWidget;
    versionToggle_->setObjectName(QStringLiteral("transcriptSwitchRow"));
    auto* versionRow = new QHBoxLayout(versionToggle_);
    versionRow->setContentsMargins(12, 10, 12, 0);
    versionRow->setSpacing(0);

    auto* versionSwitch = new QFrame;
    versionSwitch->setObjectName(QStringLiteral("transcriptSwitch"));
    auto* switchLayout = new QHBoxLayout(versionSwitch);
    switchLayout->setContentsMargins(2, 2, 2, 2);
    switchLayout->setSpacing(2);

    versionGroup_ = new QButtonGroup(this);
    fixedButton_ = new QPushButton(QStringLiteral("Fixed"));
    rawButton_ = new QPushButton(QStringLiteral("Raw"));
    for (QPushButton* button : {fixedButton_, rawButton_}) {
        button->setCheckable(true);
        button->setCursor(Qt::PointingHandCursor);
        button->setObjectName(QStringLiteral("transcriptSwitchBtn"));
        button->setFixedHeight(22);
        versionGroup_->addButton(button);
        switchLayout->addWidget(button);
    }
    versionRow->addWidget(versionSwitch);
    versionRow->addStretch();
    fixedButton_->setChecked(true);
    versionToggle_->hide();

    transcriptText_ = knobs_.makeTranscriptEditor ? knobs_.makeTranscriptEditor() : new QTextEdit;
    transcriptText_->setObjectName(QStringLiteral("transcriptText"));
    transcriptText_->setReadOnly(true);
    transcriptText_->setMinimumHeight(knobs_.transcriptMinHeight);
    transcriptText_->setFont(QFont(QStringLiteral("Segoe UI"), 13));
    transcriptText_->setPlaceholderText(knobs_.transcriptPlaceholder);

    paneLayout->addWidget(versionToggle_);
    paneLayout->addWidget(transcriptText_, 1);

    transcriptionCard_->addContentWidget(transcriptPane_);
    connect(transcriptionCard_, &HeaderCard::toggled, this,
            &TranscriptionTabBase::onTranscriptionToggled);

    // The transcription card is the elastic element: it expands to fill spare height and shrinks
    // first when the window gets smaller.
    contentLayout_->addWidget(transcriptionCard_, 1);

    statsWidget_ = new TranscriptionStatsWidget;
    contentLayout_->addWidget(statsWidget_);

    // Managed bottom stretch: 0 while expanded (the card fills), 1 while collapsed (pushes the
    // compact content to the top).
    contentLayout_->addStretch();
    bottomStretchIndex_ = contentLayout_->count() - 1;

    // Always start collapsed to keep the main window compact on launch.
    setTranscriptionCollapsed(true);
}

void TranscriptionTabBase::buildContentBeforeStatus(QVBoxLayout*) {}

void TranscriptionTabBase::buildContentAfterStatus(QVBoxLayout*) {}

void TranscriptionTabBase::connectSignals() {
    connect(modelCombo_, &QComboBox::currentTextChanged, this,
            [this](const QString&) { onBackendChanged(); });
    // activated, not currentIndexChanged: only a person's choice switches the paired computer,
    // never the field following what it reports.
    connect(remoteModelCombo_, &QComboBox::activated, this,
            &TranscriptionTabBase::onRemoteModelActivated);
    connect(remoteEngine_, &RemoteEngineControls::runtimeSelected, this,
            &TranscriptionTabBase::remoteRuntimeSelected);
    connect(remoteEngine_, &RemoteEngineControls::helpRequested, this,
            &TranscriptionTabBase::helpRequested);
    connect(localEngine_, &LocalEngineControls::engineSettingsChanged, this,
            &TranscriptionTabBase::engineSettingsChanged);
    connect(localEngine_, &LocalEngineControls::helpRequested, this,
            &TranscriptionTabBase::helpRequested);
    connect(cleanupCheck_, &QCheckBox::toggled, this, &TranscriptionTabBase::onCleanupToggled);
    connect(livePreviewCheck_, &QCheckBox::toggled, this,
            &TranscriptionTabBase::onLivePreviewToggled);
    connect(fixedButton_, &QPushButton::toggled, this, &TranscriptionTabBase::onVersionToggled);
    connect(rawButton_, &QPushButton::toggled, this, &TranscriptionTabBase::onVersionToggled);
}

void TranscriptionTabBase::onCleanupToggled(bool checked) {
    settings_.save_setting("transcript_cleanup_enabled", checked);
    Q_EMIT cleanupToggled();
}

void TranscriptionTabBase::loadCleanupSetting() {
    const bool enabled = settings_.get(keys::transcript_cleanup_enabled);
    cleanupCheck_->blockSignals(true);
    cleanupCheck_->setChecked(enabled);
    cleanupCheck_->blockSignals(false);
}

// Persists the preview toggle the same way the Settings page does. Writing the one key and
// dropping the legacy split switches in a single atomic update keeps the two entry points
// indistinguishable on disk. The signal tells the window to re-sync its other tabs and
// reconfigure the streaming runtime, which caches this value.
void TranscriptionTabBase::onLivePreviewToggled(bool checked) {
    try {
        Json patch = Json::object();
        patch["streaming_enabled"] = checked;
        settings_.update(patch, keys::legacy_streaming);
    } catch (const std::exception& error) {
        kLog.error_with(error, std::string("Couldn't save live preview setting: ") + error.what());
        loadLivePreviewSetting();
        return;
    }
    livePreviewWanted_ = checked;
    Q_EMIT livePreviewChanged();
}

void TranscriptionTabBase::loadLivePreviewSetting() {
    livePreviewWanted_ = settings_.get(keys::streaming_enabled);
    syncLivePreview();
}

void TranscriptionTabBase::setLivePreviewLocked(bool locked) {
    livePreviewLocked_ = locked;
    syncLivePreview();
}

// The Remote computer previews with whatever its host runs, which is unknown until it reports;
// until then the choice stands.
QString TranscriptionTabBase::livePreviewUnavailableReason() const {
    const std::string backend = backendValue(currentModel_).toStdString();
    std::string host;
    std::optional<std::string> family;
    if (remoteModels_) {
        host = remoteModels_->host;
        if (remoteModels_->current) {
            family = remoteModels_->current->family;
        }
    }
    return QString::fromStdString(speech::preview_unavailable_reason(backend, host, family));
}

// Shows the saved choice, or grayed out and unchecked with why. An engine that can't preview
// doesn't change the saved choice, so it comes back as it was when the engine switches to one
// that can.
void TranscriptionTabBase::syncLivePreview() {
    const QString reason = livePreviewUnavailableReason();
    QCheckBox* check = livePreviewCheck_;
    const bool blocked = check->blockSignals(true);
    check->setChecked(livePreviewWanted_ && reason.isEmpty());
    check->blockSignals(blocked);
    check->setEnabled(reason.isEmpty() && !livePreviewLocked_);
    if (reason.isEmpty()) {
        check->setToolTip(kLivePreviewTip);
    } else if (livePreviewWanted_) {
        check->setToolTip(reason + QStringLiteral(".\nIt turns back on when you switch to one."));
    } else {
        check->setToolTip(reason + QStringLiteral(".\nSwitch to one to use it."));
    }
}

void TranscriptionTabBase::onVersionToggled(bool checked) {
    if (!checked) {
        return;
    }
    const bool showRaw = rawButton_->isChecked();
    showingRaw_ = showRaw;
    if (showRaw && rawText_) {
        showTranscriptText(*rawText_);
    } else {
        showTranscriptText(fixedText_);
    }
}

void TranscriptionTabBase::redrawTranscript() {
    showTranscriptText(shownTranscript());
}

void TranscriptionTabBase::showTranscriptText(const QString& text) {
    if (knobs_.transcriptMarkdown) {
        ui::renderMarkdown(*transcriptText_->document(), text,
                           ui::MarkdownStyle::PREVIEW.scaled(ui::currentUiFontScale()));
    } else {
        transcriptText_->setPlainText(text);
    }
}

QString TranscriptionTabBase::shownTranscript() const {
    if (showingRaw_ && rawText_) {
        return *rawText_;
    }
    return fixedText_;
}

void TranscriptionTabBase::onBackendChanged() {
    // backend_display_name: a value no choice has reads as the default backend.
    const QString displayName =
        backendDisplayName(modelCombo_->currentData().toString().toStdString());
    currentModel_ = displayName;
    refreshApiModel();
    const QString backend = backendValue(displayName);
    localEngine_->setBackend(backend);
    setLocalEngineVisible(hasLocalFields(backend));
    Q_EMIT modelChanged(displayName);
}

void TranscriptionTabBase::chooseBackend(const QString& displayName) {
    if (displayName == currentModel_) {
        if (backendValue(displayName) == QLatin1String("api")) {
            refreshApiModel();
            Q_EMIT modelChanged(displayName);
        }
        return;
    }
    const int index = backendIndex(displayName);
    if (index >= 0) {
        modelCombo_->setCurrentIndex(index);
    }
}

void TranscriptionTabBase::setBackend(const QString& displayName) {
    const int index = backendIndex(displayName);
    if (index < 0) {
        return;
    }
    modelCombo_->blockSignals(true);
    modelCombo_->setCurrentIndex(index);
    modelCombo_->blockSignals(false);
    currentModel_ = displayName;
    refreshApiModel();
    const QString backend = backendValue(displayName);
    localEngine_->setBackend(backend);
    setLocalEngineVisible(hasLocalFields(backend));
}

void TranscriptionTabBase::syncEngineSelection() {
    setModelSelection(QString::fromStdString(load_model_selection(settings_)));
}

void TranscriptionTabBase::setBackendEnabled(bool enabled) {
    modelCombo_->setEnabled(enabled);
    if (apiServer_ != nullptr) {
        apiServer_->setEnabled(enabled);
    }
    backendEnabled_ = enabled;
    syncRemoteModelEnabled();
}

void TranscriptionTabBase::setApiServer(SpeechServerControls* controls) {
    if (controls == nullptr || apiServer_ != nullptr) {
        return;
    }
    apiServer_ = controls;
    controls->hide();
    // After the Backend field and the local engine's fields.
    fieldRow_->insertWidget(2, controls, 4);
    // Among the card's children where it would have been built, before the footer's, as
    // OpenWhisper's widget tree has it.
    controls->stackUnder(statusDot_);
    // And in the focus chain: reparenting appended the fields after the transcript, where Tab
    // would reach them last; OpenWhisper's come right after the local engine's fields.
    // setTabOrder ignores a container without a focus policy, so the tab stops move one by one.
    QWidget* previous = localEngine_;
    for (QWidget* w = localEngine_->nextInFocusChain(); localEngine_->isAncestorOf(w);
         w = w->nextInFocusChain()) {
        previous = w;
    }
    std::vector<QWidget*> stops;
    for (QWidget* w = controls->nextInFocusChain(); controls->isAncestorOf(w);
         w = w->nextInFocusChain()) {
        if ((w->focusPolicy() & Qt::TabFocus) != 0) {
            stops.push_back(w);
        }
    }
    for (QWidget* stop : stops) {
        QWidget::setTabOrder(previous, stop);
        previous = stop;
    }
    connect(controls, &SpeechServerControls::changed, this,
            &TranscriptionTabBase::onApiServerChanged);
    connect(controls, &SpeechServerControls::helpRequested, this,
            &TranscriptionTabBase::helpRequested);
    controls->setEnabled(backendEnabled_);
    refreshApiModel();
    setLocalEngineVisible(hasLocalFields(backendValue(currentModel_)));
}

void TranscriptionTabBase::refreshApiModel() {
    if (apiServer_ != nullptr) {
        apiServer_->refresh();
    }
}

void TranscriptionTabBase::onApiServerChanged() {
    Q_EMIT modelChanged(currentModel_);
}

void TranscriptionTabBase::setModelSelection(const QString& modelValue) {
    for (const std::string_view id : model_values()) {
        if (QString::fromUtf8(id.data(), static_cast<qsizetype>(id.size())) == modelValue) {
            setBackend(backendDisplayName(id));
            break;
        }
    }
}

void TranscriptionTabBase::setStatus(const QString& statusText) {
    setEngineMessage(statusText);
}

void TranscriptionTabBase::setEngineMessage(const QString& statusText) {
    statusMessage_ = statusText;
    if (statusText == QLatin1String("Ready") || statusText == QLatin1String("Ready to record") ||
        statusText == QLatin1String("Whisper engine ready")) {
        statusMessage_.clear();
    } else if (!statusText.isEmpty() && statusText == deviceInfo_) {
        // A reload reports its engine as status too. That is the idle readout already, and for
        // Remote the link says it better.
        statusMessage_.clear();
    }
    refreshEngineStatus();
}

void TranscriptionTabBase::setAmbientMessage(const QString& statusText) {
    if (statusMessage_.isEmpty()) {
        setEngineMessage(statusText);
    }
}

void TranscriptionTabBase::setDeviceInfo(const QString& deviceInfo, std::optional<bool> ready) {
    deviceInfo_ = deviceInfo;
    statusMessage_.clear();
    engineStatus_ =
        !ready ? EngineStatus::Unknown : (*ready ? EngineStatus::Ready : EngineStatus::Attention);
    refreshEngineStatus();
}

void TranscriptionTabBase::setEngineBusy(bool busy) {
    localEngine_->setBusy(busy);
    engineBusy_ = busy;
    if (!remoteModelCombo_->itemData(remoteModelCombo_->currentIndex()).isValid()) {
        // "Connecting..." and "Not connected" follow the reload.
        showRemoteModels();
    }
    syncRemoteModelEnabled();
    if (busy) {
        statusMessage_ = QStringLiteral("Loading speech engine...");
    } else if (statusMessage_ == QLatin1String("Loading speech engine...") ||
               statusMessage_ == QLatin1String("Reloading speech engine...")) {
        statusMessage_.clear();
    }
    refreshEngineStatus();
}

void TranscriptionTabBase::setModelDownloading(const QString& modelName, bool downloading) {
    const QString message = QStringLiteral("Downloading model '%1'...").arg(modelName);
    if (downloading) {
        modelDownloads_.insert(modelName);
        statusMessage_ = message;
    } else {
        modelDownloads_.erase(modelName);
        if (statusMessage_ == message) {
            statusMessage_.clear();
        }
    }
    refreshEngineStatus();
}

void TranscriptionTabBase::setActivityState(appcore::OverlayState state) {
    auto message = activityMessage(state);
    const QString host = connectedHost();
    if (state == appcore::OverlayState::Transcribing && !host.isEmpty()) {
        message = QStringLiteral("Transcribing on %1...").arg(host);
    }
    const appcore::OverlayState previous = activityState_;
    activityState_ = state;
    if (message) {
        statusMessage_ = *message;
    } else if ((activityMessage(previous) && statusMessage_ == *activityMessage(previous)) ||
               (previous == appcore::OverlayState::Transcribing &&
                statusMessage_.startsWith(QLatin1String("Transcribing")))) {
        statusMessage_.clear();
    }
    refreshEngineStatus();
}

QString TranscriptionTabBase::connectedHost() const {
    if (remoteShown_ && remoteLink_ && remoteLink_->state == "connected") {
        return fromStd(remoteLink_->host);
    }
    return {};
}

void TranscriptionTabBase::refreshEngineStatus() {
    const bool busy = engineLoading() || isBusyActivity(activityState_);
    QString idleMessage = deviceInfo_;
    const QString linkText = remoteShown_ ? remoteLinkText() : QString();
    if (!linkText.isEmpty() && !(engineBusy_ && remoteLink_->state != "connecting")) {
        // The link says what the busy state would: "Connecting to jed...".
        idleMessage = linkText;
    } else if (engineBusy_) {
        idleMessage = kLoadingEngine;
    } else if (!modelDownloads_.empty()) {
        idleMessage = QStringLiteral("Downloading model '%1'...").arg(*modelDownloads_.begin());
    }
    QString message = !statusMessage_.isEmpty() ? statusMessage_ : idleMessage;
    const bool showingLink = remoteShown_ && remoteLink_.has_value();
    if (!linkText.isEmpty() &&
        (statusMessage_ == kLoadingEngine || statusMessage_ == kReloadingEngine ||
         (!knobs_.initialStatus.isEmpty() && statusMessage_ == knobs_.initialStatus))) {
        // "Connecting to jed..." beats a generic loading line; a specific one ("Switching jed to
        // ...") still wins.
        message = linkText;
    }
    const QString shown = !message.isEmpty() ? message : knobs_.initialStatus;
    resolvedLabel_->setText(shown);
    resolvedLabel_->setAccessibleName(shown);
    statusDot_->setStatus(engineStatus_);
    statusDot_->setBusy(busy);
    statusDot_->setVisible(!showingLink && (engineDotVisible_ || busy));
    linkGlyph_->setVisible(showingLink);
    if (showingLink && message == linkText) {
        // Status messages keep their full text even while Remote is shown.
        resolvedLabel_->setToolTip(remoteLinkTooltip());
    }
    const bool counting = showingLink && remoteLink_->retry_at.has_value();
    if (counting && !linkCountdown_->isActive()) {
        linkCountdown_->start();
    } else if (!counting) {
        linkCountdown_->stop();
    }
    applyBackendStatus(engineBusy_ ? EngineStatus::Unknown : engineStatus_);
}

// ---- the Remote engine's link ----

void TranscriptionTabBase::setRemoteLink(const std::optional<remote::RemoteLink>& link) {
    remoteLink_ = link;
    if (link) {
        linkGlyph_->setLink(fromStd(link->state), link->busy, link->replies, link->beat);
        linkGlyph_->setToolTip(remoteLinkTooltip());
    }
    syncRemoteModelEnabled();
    showRemoteRuntime();
    refreshEngineStatus();
}

void TranscriptionTabBase::showRemoteRuntime() {
    const bool show = remoteShown_ && remoteLink_ && remoteLink_->state == "connected";
    remoteRuntimeLabel_->setVisible(show);
    if (!show) {
        return;
    }
    const remote::RemoteLink& link = *remoteLink_;
    const QString device = deviceLabel(fromStd(link.device));
    QStringList parts{QStringLiteral("Running on %1: %2")
                          .arg(fromStd(link.host),
                               device.isEmpty() ? QStringLiteral("device not reported") : device)};
    if (!link.compute_type.empty()) {
        parts << fromStd(link.compute_type);
    }
    if (!link.gpu_name.empty() && link.device == "cuda") {
        parts << fromStd(link.gpu_name);
        if (link.gpu_memory_mib > 0) {
            // Python's f"{mib / 1024:g} GB VRAM".
            parts << QStringLiteral("%1 GB VRAM")
                         .arg(QString::number(double(link.gpu_memory_mib) / 1024.0, 'g', 6));
        }
    }
    remoteRuntimeLabel_->setText(parts.join(QStringLiteral(u" · ")));
    remoteRuntimeLabel_->setToolTip(remoteLinkTooltip());
}

QString TranscriptionTabBase::remoteLinkText() const {
    if (!remoteLink_) {
        return {};
    }
    const remote::RemoteLink& link = *remoteLink_;
    const QString host =
        link.host.empty() ? QStringLiteral("the paired computer") : fromStd(link.host);
    if (link.state == "connected") {
        QStringList parts{QStringLiteral("Connected to %1").arg(host)};
        if (!link.route.empty()) {
            parts << fromStd(link.route);
        }
        if (link.latency_ms) {
            parts << (*link.latency_ms < 1 ? QStringLiteral("<1 ms")
                                           : QStringLiteral("%1 ms").arg(
                                                 std::nearbyint(*link.latency_ms), 0, 'f', 0));
        }
        return parts.join(QStringLiteral(u" · "));
    }
    if (link.state == "connecting") {
        return QStringLiteral("Connecting to %1...").arg(host);
    }
    if (link.state == "unpaired") {
        return link.detail.empty()
                   ? QStringLiteral(u"Pair with a host in Settings → Remote engine.")
                   : fromStd(link.detail);
    }
    // Offline: the first sentence of why, then when it tries again.
    QString reason =
        link.detail.empty() ? QStringLiteral("Can't reach %1.").arg(host) : fromStd(link.detail);
    reason = reason.section(QStringLiteral(". "), 0, 0);
    while (reason.endsWith(QLatin1Char('.'))) {
        reason.chop(1);
    }
    reason += QLatin1Char('.');
    if (link.retry_at) {
        const double left =
            std::chrono::duration<double>(*link.retry_at - std::chrono::steady_clock::now())
                .count();
        const auto seconds = static_cast<long long>(std::max(0.0, std::nearbyint(left)));
        return QStringLiteral("%1 Trying again in %2 s.").arg(reason).arg(seconds);
    }
    return reason + QStringLiteral(" Click to try again.");
}

QString TranscriptionTabBase::remoteLinkTooltip() const {
    if (!remoteLink_) {
        return {};
    }
    const remote::RemoteLink& link = *remoteLink_;
    if (link.state == "connected") {
        const QString engine =
            link.engine_label.empty() ? QStringLiteral("Its engine") : fromStd(link.engine_label);
        const QString where =
            link.address.empty() ? QString() : QStringLiteral(" at ") + fromStd(link.address);
        const QString device = link.device.empty()
                                   ? QString()
                                   : QStringLiteral(" on ") + fromStd(link.device).toUpper();
        const QString compute = link.compute_type.empty()
                                    ? QString()
                                    : QStringLiteral(" (%1)").arg(fromStd(link.compute_type));
        const QString note = hostStatusNote(fromStd(link.runtime_status));
        const QString detail = note.isEmpty() ? QString() : QStringLiteral("\n") + note;
        return QStringLiteral("%1%2%3, served by %4%5.%6")
            .arg(engine, device, compute, fromStd(link.host), where, detail);
    }
    if (link.state == "offline") {
        return (fromStd(link.detail) + QStringLiteral("\nClick the link to try again now."))
            .trimmed();
    }
    return fromStd(link.detail);
}

void TranscriptionTabBase::onLinkClicked() {
    if (remoteLink_ && remoteLink_->state == "offline") {
        Q_EMIT remoteRetryRequested();
    }
}

// ---- host side: the paired computers this one serves ----

void TranscriptionTabBase::setRemoteClients(const std::vector<ServedClient>& clients) {
    QStringList names;
    bool busy = false;
    for (const ServedClient& client : clients) {
        const QString name =
            client.name.isEmpty() ? QStringLiteral("a paired computer") : client.name;
        if (!names.contains(name)) {
            names << name;
        }
        busy = busy || client.busy;
    }
    if (servingBusy_ && !busy) {
        ++served_;
    }
    servingBusy_ = busy;
    if (!names.isEmpty()) {
        const QString who =
            names.size() == 1 ? names.front() : QStringLiteral("%1 computers").arg(names.size());
        servingLabel_->setText(busy ? QStringLiteral("Transcribing for %1").arg(who)
                                    : QStringLiteral("Sharing this engine with %1").arg(who));
        servingGlyph_->setLink(QStringLiteral("connected"), busy, served_);
        QStringList lines;
        for (const QString& name : names) {
            lines << QStringLiteral("%1 is connected").arg(name);
        }
        servingRow_->setToolTip(lines.join(QLatin1Char('\n')));
    }
    showServingRow(!names.isEmpty());
}

void TranscriptionTabBase::showServingRow(bool show) {
    if (show == servingShown_) {
        return;
    }
    servingShown_ = show;
    QWidget* row = servingRow_;
    if (show) {
        row->show();
        const int start = row->maximumHeight() < kUnlimitedHeight ? row->maximumHeight() : 0;
        runMaxHeightAnimation(servingAnim_, start, row->sizeHint().height(),
                              [row] { row->setMaximumHeight(kUnlimitedHeight); });
    } else {
        runMaxHeightAnimation(servingAnim_, row->height(), 0, [row] { row->hide(); });
    }
}

void TranscriptionTabBase::applyBackendStatus(EngineStatus status) {
    modelCombo_->setStatus(status);
}

// Switches between the local runtime fields and a single Model field. Without local fields the
// API backend offers its server and model and the Remote backend shows the model the paired
// computer runs, its runtime fields and the link to its model management.
void TranscriptionTabBase::setLocalEngineVisible(bool visible) {
    const bool remote = !visible && backendValue(currentModel_) == QLatin1String("remote");
    remoteShown_ = remote;
    localEngine_->setVisible(visible);
    if (apiServer_ != nullptr) {
        apiServer_->setVisible(!visible && !remote);
    }
    remoteModelField_->setVisible(remote);
    remoteEngine_->setVisible(remote);
    manageHostModels_->setVisible(remote);
    showRemoteRuntime();
    if (remote) {
        showRemoteModels();
    }
    // A remote engine is connected or it isn't; the API has no engine here.
    engineDotVisible_ = visible || remote;
    refreshEngineStatus();
    modelCombo_->setStatusVisible(engineDotVisible_);
    syncLivePreview();
}

void TranscriptionTabBase::setRemoteModels(const std::optional<remote::RemoteModels>& choices) {
    remoteModels_ = choices;
    remoteEngine_->setState(choices);
    QStringList missing;
    if (choices && choices->runtime) {
        const Json& runtime = *choices->runtime;
        const auto dependencies =
            runtime.is_object() ? runtime.find("dependencies") : runtime.end();
        if (runtime.is_object() && dependencies != runtime.end() && dependencies->is_array()) {
            for (const Json& item : *dependencies) {
                if (item.is_object() && item.value("installable", false) == true) {
                    missing << fromStd(item.value("label", std::string()));
                }
            }
        }
    }
    manageHostModels_->setAvailable(missing);
    showRemoteModels();
    // The host's engine decides whether the Remote engine can preview.
    syncLivePreview();
}

remote::RemoteModels TranscriptionTabBase::currentRemoteModels() const {
    if (remoteModels_) {
        return *remoteModels_;
    }
    // Before the controller reports: name the host, if there is one.
    remote::RemoteModels state;
    try {
        if (const auto pairing = remote::load_client_pairing(settings_)) {
            state.host = pairing->host_name;
        }
    } catch (const std::exception& error) {
        kLog.warning("Couldn't read the pairing: {}", error.what());
    }
    return state;
}

// Rebuilds the Model field: the host's models, grouped by engine.
void TranscriptionTabBase::showRemoteModels() {
    const remote::RemoteModels state = currentRemoteModels();
    const QString host = fromStd(state.host);
    const auto& current = state.current;
    QComboBox* combo = remoteModelCombo_;
    const bool blocked = combo->blockSignals(true);
    combo->clear();
    remoteEntries_.clear();
    bool selectable = false;
    QString tip;
    std::vector<remote::HostModel> others;
    if (state.models) {
        for (const auto& entry : *state.models) {
            if (!current || entry.key() != current->key()) {
                others.push_back(entry);
            }
        }
    }
    if (host.isEmpty()) {
        combo->addItem(QStringLiteral("Not paired"), QVariant());
        tip = QStringLiteral(u"Pair with a computer in Settings → Remote engine.");
    } else if (!others.empty()) {
        std::vector<remote::HostModel> entries = *state.models;
        if (current && std::ranges::none_of(entries, [&](const remote::HostModel& entry) {
                return entry.key() == current->key();
            })) {
            entries.insert(entries.begin(), *current);
        }
        if (!current) {
            combo->addItem(QStringLiteral("Choose a model"), QVariant());
        }
        std::optional<std::string> previous;
        int selected = -1;
        for (const auto& entry : entries) {
            if (previous && entry.family != *previous) {
                combo->insertSeparator(combo->count());
            }
            previous = entry.family;
            remoteEntries_.push_back(entry);
            combo->addItem(fromStd(entry.label), static_cast<int>(remoteEntries_.size() - 1));
            if (current && entry.key() == current->key()) {
                selected = combo->count() - 1;
            }
        }
        if (current) {
            combo->setCurrentIndex(selected);
            tip = QStringLiteral("%1 is running %2. Choosing another model switches %1 to it.")
                      .arg(host, fromStd(current->label));
        } else {
            tip = QStringLiteral("%1 has no model this computer can use right now. Choose one to "
                                 "switch %1 to it.")
                      .arg(host);
        }
        selectable = true;
    } else if (current) {
        remoteEntries_.push_back(*current);
        combo->addItem(fromStd(current->label), 0);
        tip =
            !state.models
                ? QStringLiteral("%1 is running %2. Update %3 there to choose its model from here.")
                      .arg(host, fromStd(current->label), QString::fromUtf8(identity().product))
                : QStringLiteral("%1 is running %2, its only downloaded model.")
                      .arg(host, fromStd(current->label));
    } else if (state.models) {
        combo->addItem(QStringLiteral("No models ready"), QVariant());
        tip = QStringLiteral("%1 has no models ready. Open Manage host models to download models "
                             "and install their runtimes.")
                  .arg(host);
    } else {
        combo->addItem(engineBusy_ ? QStringLiteral("Connecting...")
                                   : QStringLiteral("Not connected"),
                       QVariant());
        tip = QStringLiteral("%1's models show here once this computer connects.").arg(host);
    }
    combo->blockSignals(blocked);
    combo->setToolTip(tip);
    remoteSelectable_ = selectable;
    syncRemoteModelEnabled();
}

void TranscriptionTabBase::syncRemoteModelEnabled() {
    remoteModelCombo_->setEnabled(remoteSelectable_ && backendEnabled_ && !engineBusy_);
    const bool disconnected = remoteLink_ && remoteLink_->state != "connected";
    remoteEngine_->setLocked(!backendEnabled_ || engineBusy_ || disconnected);
}

void TranscriptionTabBase::onRemoteModelActivated(int index) {
    const QVariant item = remoteModelCombo_->itemData(index);
    if (!item.isValid()) {
        return;
    }
    const auto slot = static_cast<std::size_t>(item.toInt());
    if (slot >= remoteEntries_.size()) {
        return;
    }
    const remote::HostModel choice = remoteEntries_[slot];
    const auto current = currentRemoteModels().current;
    if (current && current->key() == choice.key()) {
        return;
    }
    Q_EMIT remoteModelSelected(fromStd(choice.family), fromStd(choice.model));
}

void TranscriptionTabBase::applyTranscriptionStretch(bool collapsed) {
    if (collapsed) {
        contentLayout_->setStretchFactor(transcriptionCard_, 0);
        contentLayout_->setStretch(bottomStretchIndex_, 1);
    } else {
        contentLayout_->setStretchFactor(transcriptionCard_, 1);
        contentLayout_->setStretch(bottomStretchIndex_, 0);
    }
}

void TranscriptionTabBase::onTranscriptionToggled(bool collapsed) {
    Q_EMIT transcriptionCollapsed(collapsed, transcriptionCard_->contentHeight());
    QTimer::singleShot(kCollapseStretchDelayMs, this,
                       [this, collapsed] { applyTranscriptionStretch(collapsed); });
}

void TranscriptionTabBase::setTranscriptionCollapsed(bool collapsed) {
    transcriptionCard_->setCollapsed(collapsed, false);
    applyTranscriptionStretch(collapsed);
}

bool TranscriptionTabBase::isTranscriptionCollapsed() const {
    return transcriptionCard_->isCollapsed();
}

void TranscriptionTabBase::expandTranscription() {
    if (isTranscriptionCollapsed()) {
        transcriptionCard_->setCollapsed(false, true);
    }
}

void TranscriptionTabBase::setTranscript(const QString& text, const std::optional<QString>& raw) {
    fixedText_ = text;
    rawText_ = (raw && !raw->isEmpty() && *raw != text) ? raw : std::nullopt;
    showingRaw_ = false;

    showVersionToggle(rawText_.has_value());
    fixedButton_->blockSignals(true);
    rawButton_->blockSignals(true);
    fixedButton_->setChecked(true);
    rawButton_->setChecked(false);
    fixedButton_->blockSignals(false);
    rawButton_->blockSignals(false);

    showTranscriptText(fixedText_);
}

void TranscriptionTabBase::clearTranscription() {
    fixedText_.clear();
    rawText_.reset();
    showingRaw_ = false;
    showVersionToggle(false);
    transcriptText_->clear();
}

// Shows or hides the Fixed / Raw switch in the pane's top corner. The text edit gives up most of
// its top padding while the switch is up, so the two do not stack a full margin each above the
// first line.
void TranscriptionTabBase::showVersionToggle(bool visible) {
    versionToggle_->setVisible(visible);
    if (transcriptText_->property(ui::prop::headed).toBool() != visible) {
        ui::setStyleProperty(transcriptText_, ui::prop::headed, visible);
    }
}

void TranscriptionTabBase::setTranscriptionStats(
    double transcriptionTime, double audioDuration, std::optional<std::int64_t> fileSize,
    double cleanupTime, const std::optional<remote::RemoteTiming>& remote) {
    statsWidget_->setStats(transcriptionTime, audioDuration, fileSize, cleanupTime, remote);
}

void TranscriptionTabBase::clearTranscriptionStats() {
    statsWidget_->clear();
}

} // namespace fv
