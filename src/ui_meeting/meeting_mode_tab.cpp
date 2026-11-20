#include "ui_meeting/meeting_mode_tab.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <exception>
#include <string>
#include <string_view>
#include <utility>

#include <QEvent>
#include <QFont>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QScrollArea>
#include <QSizePolicy>
#include <QTimer>
#include <QVBoxLayout>

#include "base/log.h"
#include "history/format_utils.h"
#include "meeting/finalization_view.h"
#include "meeting_state/patches.h"
#include "meeting_state/time_utils.h"
#include "pyport/number.h"
#include "pyport/text.h"
#include "settings/keys.h"
#include "ui_meeting/brief_edit.h"
#include "ui_meeting/detail/payload.h"
#include "ui_meeting/finalization_card.h"
#include "ui_meeting/live_dot.h"
#include "ui_meeting/recent_meeting_row.h"
#include "ui_theme/font_scale.h"
#include "ui_theme/local_style.h"
#include "ui_theme/style_props.h"
#include "ui_widgets/buttons.h"
#include "ui_widgets/settings_switch.h"
#include "ui_widgets/studio.h"
#include "ui_widgets/wrapped_label.h"

namespace fv {

namespace {

constexpr log::Logger kLog{"ui_qt.widgets.meeting_mode_tab"};

using ui_meeting::detail::get;
using ui_meeting::detail::pyStr;
using ui_meeting::detail::strOrEmpty;
using ui_meeting::detail::truthy;

// Identity fields a meeting-state payload may update, kept for the result's header.
constexpr std::array<std::string_view, 5> kIdentityKeys{"title", "display_title", "started_at",
                                                        "ended_at", "paused_total_s"};

const QString kPauseDescription = QStringLiteral("Pause meeting capture.");

QWidget* plain(const QString& name = QStringLiteral("meetingPlain")) {
    auto* widget = new QWidget();
    widget->setObjectName(name);
    return widget;
}

QFrame* pane() {
    auto* frame = new QFrame();
    frame->setObjectName(QStringLiteral("meetingStagePane"));
    return frame;
}

QLabel* text(const QString& value, const QString& name, bool wrap = false) {
    QLabel* label = wrap ? new WrappedLabel(value) : new QLabel(value);
    label->setObjectName(name);
    if (name == QStringLiteral("meetingOverline")) {
        QFont font = label->font();
        font.setLetterSpacing(QFont::PercentageSpacing, 108);
        label->setFont(font);
    }
    return label;
}

QString utf8(const std::string& value) {
    return qt::toQString(value);
}

} // namespace

QString aiInsightsTooltip() {
    return QStringLiteral("On: send transcript text to the AI model chosen\n"
                          "in Settings for live insights and a final report.\n"
                          "Off: keep the recording and transcript only.\n"
                          "Audio routing is set separately in Voice & speakers.");
}

MeetingModeTab::MeetingModeTab(QWidget* parent) : MeetingModeTab(MeetingModeTabOptions{}, parent) {}

MeetingModeTab::MeetingModeTab(const MeetingModeTabOptions& options, QWidget* parent)
    : QWidget(parent), clock_(options.clock != nullptr ? *options.clock : system_clock()),
      platform_(options.platform), ui_(std::make_shared<qt::QtUiExecutor>(this)) {
    setObjectName(QStringLiteral("meetingModeTab"));
    setAccessibleName(QStringLiteral("Meeting Mode"));
    setAccessibleDescription(
        QStringLiteral("Start, monitor, and finish a meeting, then open its browser dashboard."));

    elapsedTimer_ = new QTimer(this);
    elapsedTimer_->setInterval(1000);
    connect(elapsedTimer_, &QTimer::timeout, this, &MeetingModeTab::refreshElapsed);

    setupUi(options);
    setDeveloperMode(keys::developer_mode.read(options.settings));
    applyLayoutState();
}

MeetingModeTab::~MeetingModeTab() = default;

// ---- building ----

void MeetingModeTab::setupUi(const MeetingModeTabOptions& options) {
    auto* mainLayout = new QVBoxLayout(this);
    mainLayout->setContentsMargins(0, 0, 0, 0);
    mainLayout->setSpacing(0);

    scrollArea_ = new QScrollArea();
    scrollArea_->setObjectName(QStringLiteral("meetingModeScrollArea"));
    scrollArea_->setAccessibleName(QStringLiteral("Meeting Mode controls"));
    scrollArea_->setWidgetResizable(true);
    scrollArea_->setFrameShape(QFrame::NoFrame);
    scrollArea_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    ui::setLocalStyleSheet(scrollArea_, QStringLiteral("QScrollArea#meetingModeScrollArea { "
                                                       "border: none; background: transparent; }"));
    content_ = new QWidget();
    content_->setObjectName(QStringLiteral("meetingModeContent"));
    scrollArea_->setWidget(content_);
    mainLayout->addWidget(scrollArea_);
    auto* page = new QVBoxLayout(content_);
    page->setContentsMargins(24, 6, 24, 16);
    page->setSpacing(0);

    stage_ = new Stage(QStringLiteral("meetingStage"), meeting_tab::kStageWidth,
                       meeting_tab::kStageHeight);
    split_ =
        new StudioSplit(buildColumn(options), stage_, nullptr, QStringLiteral("meetingStudio"));
    page->addWidget(split_, 1);
    emptyPane_ = static_cast<QFrame*>(stage_->addPane(buildEmptyPane()));
    livePane_ = static_cast<QFrame*>(stage_->addPane(buildLivePane()));
    resultPane_ = static_cast<QFrame*>(stage_->addPane(buildResultPane()));
    built_ = true;
    applySizes();
}

void MeetingModeTab::addSection(QVBoxLayout* body, const QString& title) {
    aiRule_ = new QFrame();
    aiRule_->setObjectName(QStringLiteral("meetingRule"));
    aiRule_->setFixedHeight(1);
    body->addSpacing(12);
    body->addWidget(aiRule_);
    body->addSpacing(10);
    aiOverline_ = text(title, QStringLiteral("meetingOverline"));
    body->addWidget(aiOverline_);
    body->addSpacing(4);
}

// The next meeting: Start, then what it will do, read top to bottom. The column is the same in
// every state but a live meeting, so Start is always found in one place.
QFrame* MeetingModeTab::buildColumn(const MeetingModeTabOptions& options) {
    column_ = new QFrame();
    column_->setObjectName(QStringLiteral("meetingColumn"));
    auto* body = new QVBoxLayout(column_);
    body->setContentsMargins(16, 16, 16, 12);
    body->setSpacing(0);

    // The beta mark belongs to the feature, so it sits on the title rather than beside a note
    // about where speech is sent.
    auto* heading = new QHBoxLayout();
    heading->setContentsMargins(0, 0, 0, 12);
    heading->setSpacing(8);
    heading->addWidget(text(QStringLiteral("Meeting Mode"), QStringLiteral("meetingColumnTitle")));
    betaTag_ = new QLabel(QStringLiteral("Beta"));
    betaTag_->setObjectName(QStringLiteral("meetingColumnBetaTag"));
    betaTag_->setAlignment(Qt::AlignCenter);
    betaTag_->setAccessibleName(QStringLiteral("Meeting Mode is in beta"));
    const QString betaNotice = QStringLiteral("Transcripts and insights may be inaccurate.");
    betaTag_->setAccessibleDescription(betaNotice);
    betaTag_->setToolTip(betaNotice);
    heading->addWidget(betaTag_, 0, Qt::AlignVCenter);
    heading->addStretch(1);
    body->addLayout(heading);

    // One slot holds Start, or the dashboard actions while a meeting runs (Pause and End are on
    // the stage), so the rows below stay put.
    topSlot_ = plain();
    auto* slot = new QVBoxLayout(topSlot_);
    slot->setContentsMargins(0, 0, 0, 0);
    slot->setSpacing(8);

    idleCard_ = plain(QStringLiteral("meetingIdleCard"));
    auto* idleLayout = new QVBoxLayout(idleCard_);
    idleLayout->setContentsMargins(0, 0, 0, 0);
    idleLayout->setSpacing(8);
    startButton_ = new SuccessButton(QStringLiteral("Start Meeting"));
    ui::setStyleProperty(startButton_, ui::prop::buttonId, QStringLiteral("meetingStart"));
    startButton_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    startButton_->setAccessibleDescription(
        QStringLiteral("Begin microphone and supported system-audio capture."));
    connect(startButton_, &QPushButton::clicked, this, &MeetingModeTab::onStartClicked);
    idleLayout->addWidget(startButton_);

    demoButton_ = new Button(QStringLiteral("Load demo meeting"));
    demoButton_->setObjectName(QStringLiteral("meetingDemoButton"));
    demoButton_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    demoButton_->setToolTip(QStringLiteral(
        "Open the dashboard with a fake transcript so you can test end-of-meeting cleanup and "
        "the final report without recording."));
    demoButton_->setAccessibleDescription(
        QStringLiteral("Load a fake meeting and open its dashboard without recording audio."));
    connect(demoButton_, &QPushButton::clicked, this, &MeetingModeTab::onDemoClicked);
    idleLayout->addWidget(demoButton_);
    demoHint_ = new WrappedLabel(QStringLiteral("Loads a fake transcript and opens the dashboard. "
                                                "Turn on AI insights, then End, to test cleanup "
                                                "and the report."));
    demoHint_->setObjectName(QStringLiteral("meetingDemoHint"));
    idleLayout->addWidget(demoHint_);
    slot->addWidget(idleCard_);

    liveActions_ = plain();
    auto* liveLayout = new QHBoxLayout(liveActions_);
    liveLayout->setContentsMargins(0, 0, 0, 0);
    liveLayout->setSpacing(8);
    dashboardButton_ = new Button(QStringLiteral("Open dashboard"));
    dashboardButton_->setObjectName(QStringLiteral("meetingDashboardButton"));
    dashboardButton_->setAccessibleDescription(
        QStringLiteral("Open the live transcript and meeting insights in your web browser."));
    connect(dashboardButton_, &QPushButton::clicked, this,
            [this] { Q_EMIT openDashboardRequested(); });
    guestLinkButton_ = new Button(QStringLiteral("Copy guest link"));
    guestLinkButton_->setObjectName(QStringLiteral("meetingGuestLinkButton"));
    guestLinkButton_->setAccessibleDescription(
        QStringLiteral("Copy the guest dashboard link to the clipboard."));
    connect(guestLinkButton_, &QPushButton::clicked, this,
            [this] { Q_EMIT copyGuestLinkRequested(); });
    for (Button* button : {dashboardButton_, guestLinkButton_}) {
        button->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        button->setMinimumWidth(0);
        liveLayout->addWidget(button, 1);
    }
    slot->addWidget(liveActions_);

    // Under Start while the last meeting is still on the stage: what starting the next one does
    // to it.
    slotNote_ = new WrappedLabel(QString());
    slotNote_->setObjectName(QStringLiteral("meetingColumnNote"));
    slot->addWidget(slotNote_);
    slot->addStretch(1);
    body->addWidget(topSlot_);

    speechDestination_ = new WrappedLabel(QString());
    speechDestination_->setObjectName(QStringLiteral("meetingColumnNote"));
    speechDestination_->setContentsMargins(0, 10, 0, 0);
    body->addWidget(speechDestination_);

    addSection(body, QStringLiteral("AI INSIGHTS"));
    body->addWidget(buildAiPanel(options));
    body->addWidget(buildBriefPanel());

    body->addStretch(1);
    backgroundNotice_ = new WrappedLabel(QString());
    backgroundNotice_->setObjectName(QStringLiteral("meetingColumnNote"));
    backgroundNotice_->setAccessibleName(QStringLiteral("Background meeting processing"));
    backgroundNotice_->hide();
    body->addWidget(backgroundNotice_);
    if (meeting::meeting_setup_available(platform_)) {
        auto* foot = new QHBoxLayout();
        foot->setContentsMargins(0, 10, 0, 0);
        setupLink_ = meetingLinkButton(
            QStringLiteral("Meeting setup  →"), QStringLiteral("meetingSetupLink"),
            QStringLiteral("Check the microphone, system audio and speech model"));
        connect(setupLink_, &QPushButton::clicked, this, [this] { Q_EMIT setupRequested(); });
        foot->addWidget(setupLink_);
        foot->addStretch(1);
        body->addLayout(foot);
    }
    return column_;
}

// The AI insights switch with what it does and where it runs. It stays visible during a meeting,
// where the switch turns insights on or off live, but shows its details only while the next
// meeting is being set up.
QWidget* MeetingModeTab::buildAiPanel(const MeetingModeTabOptions& initial) {
    aiPanel_ = plain(QStringLiteral("meetingAiPanel"));
    auto* layout = new QVBoxLayout(aiPanel_);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(2);

    auto* titleRow = new QHBoxLayout();
    titleRow->setContentsMargins(0, 0, 0, 0);
    titleRow->setSpacing(8);
    aiLabel_ = text(QStringLiteral("AI insights"), QStringLiteral("meetingRowName"));
    aiLabel_->setToolTip(aiInsightsTooltip());
    titleRow->addWidget(aiLabel_);
    titleRow->addStretch(1);
    // The switch carries the row's name for screen readers; it paints no text.
    cloudCheckbox_ = new SettingsSwitch();
    cloudCheckbox_->setText(QStringLiteral("AI insights"));
    cloudCheckbox_->setAccessibleName(QStringLiteral("AI insights"));
    cloudCheckbox_->setChecked(keys::meeting_cloud_last_enabled.read(initial.settings));
    cloudCheckbox_->setAccessibleDescription(QStringLiteral(
        "When on, transcript text goes to the AI model chosen in Settings for live insights and "
        "a final report. Speech audio routing is configured separately in Voice & speakers."));
    cloudCheckbox_->setToolTip(aiInsightsTooltip());
    titleRow->addWidget(cloudCheckbox_);
    layout->addLayout(titleRow);

    aiCompactDestination_ = new WrappedLabel(QString(), nullptr, true);
    aiCompactDestination_->setObjectName(QStringLiteral("meetingAiCompactDestination"));
    aiCompactDestination_->hide();
    layout->addWidget(aiCompactDestination_);

    aiDetails_ = plain(QStringLiteral("meetingAiDetails"));
    auto* detailsLayout = new QVBoxLayout(aiDetails_);
    detailsLayout->setContentsMargins(0, 0, 0, 0);
    detailsLayout->setSpacing(4);
    aiDescription_ = new WrappedLabel(QStringLiteral("Live notes, insights, and a final report."));
    aiDescription_->setObjectName(QStringLiteral("meetingAiDescription"));
    detailsLayout->addWidget(aiDescription_);
    auto* modelRow = new QHBoxLayout();
    modelRow->setContentsMargins(0, 0, 0, 0);
    modelRow->setSpacing(10);
    aiDestination_ = new WrappedLabel(QString(), nullptr, true);
    aiDestination_->setObjectName(QStringLiteral("meetingAiDestination"));
    aiDestination_->setAccessibleName(QStringLiteral("AI insights model"));
    modelRow->addWidget(aiDestination_, 1);
    aiSettingsButton_ = meetingLinkButton(
        QStringLiteral("Change…"), QStringLiteral("meetingAiSettingsButton"),
        QStringLiteral("Choose the AI model in Settings → Meeting Mode → Intelligence"));
    aiSettingsButton_->setAccessibleName(QStringLiteral("Change the AI insights model"));
    connect(aiSettingsButton_, &QPushButton::clicked, this,
            [this] { Q_EMIT aiSettingsRequested(); });
    modelRow->addWidget(aiSettingsButton_, 0, Qt::AlignTop);
    detailsLayout->addLayout(modelRow);
    layout->addWidget(aiDetails_);

    aiPanel_->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Maximum);
    // Where Python's refresh_ai_destination first reads them: the lines are laid out from here.
    setSpeechNote(initial.speechNote);
    setAiDestination(initial.aiDestination, initial.aiCompactDestination);
    refreshAiDestination();
    return aiPanel_;
}

// The optional brief, as a labelled field with its own hint. It shows only while the next
// meeting is being set up with AI insights on; the dashboard owns the brief once it starts.
QWidget* MeetingModeTab::buildBriefPanel() {
    briefPanel_ = plain(QStringLiteral("meetingBriefPanel"));
    auto* layout = new QVBoxLayout(briefPanel_);
    layout->setContentsMargins(0, 12, 0, 0);
    layout->setSpacing(6);

    auto* header = new QHBoxLayout();
    header->setContentsMargins(0, 0, 0, 0);
    header->setSpacing(8);
    briefLabel_ = new QLabel(QStringLiteral("Meeting brief"));
    briefLabel_->setObjectName(QStringLiteral("meetingRowName"));
    header->addWidget(briefLabel_);
    header->addStretch();
    auto* optional = new QLabel(QStringLiteral("Optional"));
    optional->setObjectName(QStringLiteral("meetingBriefOptional"));
    header->addWidget(optional);
    layout->addLayout(header);

    // The brief is stated before Start so the agent works to it from the first checkpoint. It
    // stays editable on the dashboard afterwards.
    briefInput_ = new BriefEdit();
    briefInput_->setObjectName(QStringLiteral("meetingBriefInput"));
    briefInput_->setPlaceholderText(
        QStringLiteral("What do you want out of this meeting? e.g. “We decide the vendor today — "
                       "capture who objected and why.”"));
    briefInput_->setAccessibleName(QStringLiteral("Meeting brief"));
    briefInput_->setAccessibleDescription(QStringLiteral(
        "Optional. What you want out of this meeting's notes, including anything the AI note "
        "taker should watch for. It is read on every pass and can be changed from the dashboard "
        "during the meeting."));
    // Tab leaves the field like any other form control; a brief has no use for literal tabs.
    briefInput_->setTabChangesFocus(true);
    briefInput_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    briefInput_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    briefLabel_->setBuddy(briefInput_);
    layout->addWidget(briefInput_);

    briefHint_ = new WrappedLabel(
        QStringLiteral("AI insights read this on every pass. You can change it from the "
                       "dashboard."));
    briefHint_->setObjectName(QStringLiteral("meetingBriefHint"));
    layout->addWidget(briefHint_);
    // Show or hide the brief first, so it has already changed by the time turning the switch on
    // opens the consent dialog.
    connect(cloudCheckbox_, &QAbstractButton::toggled, this,
            [this](bool) { syncBriefAvailability(); });
    connect(cloudCheckbox_, &QAbstractButton::toggled, this,
            [this](bool checked) { Q_EMIT cloudToggled(checked); });
    briefPanel_->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Maximum);
    return briefPanel_;
}

// No meeting on the stage: what Meeting Mode does, and the newest few.
QFrame* MeetingModeTab::buildEmptyPane() {
    QFrame* frame = pane();
    auto* outer = new QHBoxLayout(frame);
    outer->setContentsMargins(28, 24, 28, 24);
    outer->addStretch(1);
    QWidget* column = plain();
    column->setMaximumWidth(460);
    column->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    auto* body = new QVBoxLayout(column);
    body->setContentsMargins(0, 0, 0, 0);
    body->setSpacing(0);
    body->addStretch(2);

    intro_ = plain(QStringLiteral("meetingIntro"));
    auto* introLayout = new QVBoxLayout(intro_);
    introLayout->setContentsMargins(0, 0, 0, 0);
    introLayout->setSpacing(4);
    introLayout->addWidget(
        text(QStringLiteral("No meeting running"), QStringLiteral("meetingHeading")));
    const auto [subtitleCopy, platformCopy] = meeting::meeting_audio_support_copy(platform_);
    subtitle_ = new WrappedLabel(utf8(subtitleCopy));
    subtitle_->setObjectName(QStringLiteral("meetingHint"));
    subtitle_->setToolTip(utf8(platformCopy));
    introLayout->addWidget(subtitle_);
    platformHint_ = new WrappedLabel(utf8(platformCopy), intro_);
    platformHint_->setObjectName(QStringLiteral("meetingModePlatformHint"));
    ui::setStyleProperty(platformHint_, ui::prop::tone,
                         utf8(meeting::meeting_audio_platform_tone(platform_)));
    platformHint_->setAccessibleName(QStringLiteral("Meeting platform notice"));
    platformHint_->setVisible(meeting::meeting_audio_shows_platform_warning(platform_));
    introLayout->addSpacing(6);
    introLayout->addWidget(platformHint_);
    body->addWidget(intro_);
    body->addSpacing(22);

    const std::array<std::pair<QString, QString>, 3> steps{{
        {QStringLiteral("Start and talk"),
         QStringLiteral("Press Start Meeting when the call begins.")},
        {QStringLiteral("Follow along"),
         QStringLiteral("Open the dashboard for the live transcript and notes.")},
        {QStringLiteral("Find it later"),
         QStringLiteral("The transcript and audio stay in Past Meetings, with a final report "
                        "when AI insights are on.")},
    }};
    int number = 0;
    for (const auto& [title, words] : steps) {
        ++number;
        auto* step = new QHBoxLayout();
        step->setSpacing(12);
        QLabel* badge = text(QString::number(number), QStringLiteral("meetingStep"));
        badge->setAlignment(Qt::AlignCenter);
        step->addWidget(badge, 0, Qt::AlignTop);
        auto* lines = new QVBoxLayout();
        lines->setSpacing(1);
        lines->addWidget(text(title, QStringLiteral("meetingTitle")));
        lines->addWidget(text(words, QStringLiteral("meetingHint"), true));
        step->addLayout(lines, 1);
        body->addLayout(step);
        body->addSpacing(14);
    }

    recentBox_ = plain();
    auto* recent = new QVBoxLayout(recentBox_);
    recent->setContentsMargins(0, 10, 0, 0);
    recent->setSpacing(4);
    auto* rule = new QFrame();
    rule->setObjectName(QStringLiteral("meetingRule"));
    rule->setFixedHeight(1);
    recent->addWidget(rule);
    recent->addSpacing(12);
    recent->addWidget(text(QStringLiteral("RECENT"), QStringLiteral("meetingOverline")));
    recent->addSpacing(2);
    recentLayout_ = new QVBoxLayout();
    recentLayout_->setSpacing(2);
    recent->addLayout(recentLayout_);
    recentBox_->hide();
    body->addWidget(recentBox_);
    body->addStretch(3);
    outer->addWidget(column, 4);
    outer->addStretch(1);
    return frame;
}

// A meeting on the stage: the on-air card with its clock, Pause and End.
QFrame* MeetingModeTab::buildLivePane() {
    QFrame* frame = pane();
    auto* outer = new QVBoxLayout(frame);
    outer->setContentsMargins(28, 24, 28, 24);
    outer->setSpacing(0);
    outer->addStretch(2);

    sessionCard_ = new QFrame();
    sessionCard_->setObjectName(QStringLiteral("meetingOnAir"));
    ui::setStyleProperty(sessionCard_, ui::prop::tone, QStringLiteral("starting"));
    auto* card = new QVBoxLayout(sessionCard_);
    card->setContentsMargins(24, 26, 24, 24);
    card->setSpacing(0);

    auto* statusRow = new QHBoxLayout();
    statusRow->setSpacing(10);
    statusRow->addStretch(1);
    liveDot_ = new LiveDot();
    statusRow->addWidget(liveDot_, 0, Qt::AlignVCenter);
    statusPill_ = new QLabel(QStringLiteral("Meeting"));
    statusPill_->setObjectName(QStringLiteral("meetingStatusPill"));
    QFont font = statusPill_->font();
    font.setLetterSpacing(QFont::PercentageSpacing, 114);
    statusPill_->setFont(font);
    // After the font: the first polish keeps the font it finds as the one the stylesheet
    // resolves against from then on, letter spacing included.
    ui::setStyleProperty(statusPill_, ui::prop::tone, QStringLiteral("starting"));
    statusPill_->setAccessibleName(QStringLiteral("Meeting status: Meeting"));
    statusRow->addWidget(statusPill_, 0, Qt::AlignVCenter);
    statusRow->addStretch(1);
    card->addLayout(statusRow);

    elapsedLabel_ = new QLabel(QStringLiteral("00:00"));
    elapsedLabel_->setObjectName(QStringLiteral("meetingElapsedLabel"));
    elapsedLabel_->setAlignment(Qt::AlignCenter);
    elapsedLabel_->setAccessibleName(QStringLiteral("Meeting elapsed time: 00:00"));
    card->addWidget(elapsedLabel_);
    card->addSpacing(14);

    auto* controls = new QHBoxLayout();
    controls->setSpacing(12);
    controls->addStretch(1);
    pauseButton_ = new Button(QStringLiteral("Pause"));
    pauseButton_->setObjectName(QStringLiteral("meetingPauseButton"));
    pauseButton_->setAccessibleDescription(kPauseDescription);
    connect(pauseButton_, &QPushButton::clicked, this, &MeetingModeTab::onPauseClicked);
    controls->addWidget(pauseButton_);
    endButton_ = new DangerButton(QStringLiteral("End"));
    endButton_->setObjectName(QStringLiteral("meetingEndButton"));
    endButton_->setAccessibleDescription(
        QStringLiteral("Stop capture and begin post-meeting finalization."));
    connect(endButton_, &QPushButton::clicked, this, [this] { Q_EMIT endRequested(); });
    controls->addWidget(endButton_);
    controls->addStretch(1);
    card->addLayout(controls);

    auto* centre = new QHBoxLayout();
    centre->addStretch(1);
    centre->addWidget(sessionCard_, 10);
    centre->addStretch(1);
    outer->addLayout(centre);
    outer->addSpacing(16);
    liveHint_ = static_cast<WrappedLabel*>(
        text(QStringLiteral("The wrap-up opens here when you end the meeting."),
             QStringLiteral("meetingHint"), true));
    liveHint_->setAlignment(Qt::AlignCenter);
    outer->addWidget(liveHint_);
    outer->addStretch(3);
    return frame;
}

// The meeting that just ended: its wrap-up steps and what to do next.
QFrame* MeetingModeTab::buildResultPane() {
    QFrame* frame = pane();
    auto* outer = new QVBoxLayout(frame);
    outer->setContentsMargins(0, 0, 0, 0);
    // Its own scrolling: a long checklist never grows the window.
    resultScroll_ = new QScrollArea();
    resultScroll_->setObjectName(QStringLiteral("meetingResultScroll"));
    resultScroll_->setWidgetResizable(true);
    resultScroll_->setFrameShape(QFrame::NoFrame);
    resultScroll_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    QWidget* host = plain(QStringLiteral("meetingResultHost"));
    auto* hostLayout = new QVBoxLayout(host);
    hostLayout->setContentsMargins(20, 20, 20, 20);
    finalizationCard_ = new FinalizationCard();
    connect(finalizationCard_, &FinalizationCard::openReportRequested, this,
            &MeetingModeTab::openReportRequested);
    connect(finalizationCard_, &FinalizationCard::retrySpeakersRequested, this,
            &MeetingModeTab::retrySpeakersRequested);
    connect(finalizationCard_, &FinalizationCard::retryInsightsRequested, this,
            &MeetingModeTab::retryInsightsRequested);
    connect(finalizationCard_, &FinalizationCard::retryStepRequested, this,
            &MeetingModeTab::retryStepRequested);
    connect(finalizationCard_, &FinalizationCard::backgroundRequested, this,
            &MeetingModeTab::backgroundRequested);
    connect(finalizationCard_, &FinalizationCard::deferInsightsRequested, this,
            &MeetingModeTab::deferInsightsRequested);
    hostLayout->addWidget(finalizationCard_);
    hostLayout->addStretch(1);
    resultScroll_->setWidget(host);
    outer->addWidget(resultScroll_);
    return frame;
}

// ---- the column ----

// The brief exists to steer AI insights. Without them it would only label the meeting on the
// dashboard and in the export, so with the switch off the panel is hidden and nothing typed
// there is sent; the text stays for when the switch comes back on. It is a pre-meeting control:
// once a meeting starts, the dashboard owns the brief.
void MeetingModeTab::syncBriefAvailability() {
    briefPanel_->setVisible(aiAvailable_ && cloudCheckbox_->isChecked() && settingUp());
}

bool MeetingModeTab::wrappingUp() const {
    return truthy(finalization_) && strOrEmpty(get(finalization_, "status")) == "running";
}

bool MeetingModeTab::aiShown() const {
    return aiAvailable_ && cloudCheckbox_->isChecked();
}

// A running meeting keeps the endpoint it started with, so the lines are left alone until it
// ends rather than showing a later Settings change.
void MeetingModeTab::refreshAiDestination() {
    if (active_) {
        return;
    }
    Q_EMIT aiDestinationWanted();
}

void MeetingModeTab::setSpeechNote(const QString& note) {
    if (active_) {
        return;
    }
    // The note says why this isn't the dictation engine.
    const std::string line = qt::toUtf8(note) + " Speech recognition runs on this computer.";
    speechDestination_->setText(utf8(py::strip(line)));
}

void MeetingModeTab::setRemoteSpeechHost(const std::optional<QString>& host) {
    if (active_) {
        return;
    }
    speechDestination_->setText(
        QStringLiteral("Remote speech · microphone and system audio sent to %1. Recordings saved "
                       "here.")
            .arg(host.value_or(QStringLiteral("a host (not paired yet)"))));
}

void MeetingModeTab::setAiDestination(const QString& full, const QString& compact) {
    if (active_ || full == aiDestination_->text()) {
        return;
    }
    aiDestination_->setText(full);
    aiCompactDestination_->setText(compact);
    aiCompactDestination_->setToolTip(full);
}

void MeetingModeTab::setAiInsightsAvailable(bool available) {
    aiAvailable_ = available;
    aiRule_->setVisible(available);
    aiOverline_->setVisible(available);
    aiPanel_->setVisible(available);
    syncBriefAvailability();
}

void MeetingModeTab::setRecentMeetingsReader(Executor& io, RecentReader reader) {
    recentIo_ = &io;
    recentReader_ = std::make_shared<RecentReader>(std::move(reader));
}

QString MeetingModeTab::meetingBrief() const {
    if (!aiShown()) {
        return {};
    }
    const QString brief = utf8(py::strip(qt::toUtf8(briefInput_->toPlainText())));
    // Capped in code points, as Python slices a str; the validation layer holds the cap.
    qsizetype units = 0;
    std::size_t points = 0;
    while (units < brief.size() && points < meeting_state::MAX_INTENT_LEN) {
        const bool pair = brief.at(units).isHighSurrogate() && units + 1 < brief.size() &&
                          brief.at(units + 1).isLowSurrogate();
        units += pair ? 2 : 1;
        ++points;
    }
    return brief.left(units);
}

// A last meeting that is still wrapping up is sent to the background first. A finished or
// incomplete one needs nothing here: the runtime files it in Past Meetings as part of starting.
void MeetingModeTab::onStartClicked() {
    if (wrappingUp() && backgroundAvailable_) {
        Q_EMIT backgroundRequested();
    }
    Q_EMIT startRequested(aiShown(), meetingBrief());
}

void MeetingModeTab::onDemoClicked() {
    Q_EMIT demoRequested(aiShown());
}

void MeetingModeTab::onPauseClicked() {
    if (paused_) {
        Q_EMIT resumeRequested();
    } else {
        Q_EMIT pauseRequested();
    }
}

void MeetingModeTab::setDeveloperMode(bool enabled) {
    developerMode_ = enabled;
    applyLayoutState();
}

// ---- state ----

void MeetingModeTab::setMeetingState(const QJsonObject& patch) {
    setMeetingState(ui_meeting::detail::toJson(patch));
}

void MeetingModeTab::setMeetingState(const Json& payload) {
    if (!payload.is_object()) {
        return;
    }
    applyBackground(payload);
    applyCloudChoice(payload);
    applySpeechDestination(get(payload, "speech"));

    // Paused first: the elapsed value then anchors on the state it describes, not the one being
    // left.
    if (payload.contains("paused")) {
        setPaused(truthy(payload.at("paused")));
    }
    if (payload.contains("elapsed_s")) {
        restartElapsed(payload.at("elapsed_s"), get(payload, "elapsed_at"));
    }
    if (payload.contains("status")) {
        applyStatus(pyStr(payload.at("status")), payload);
    }
    if (payload.contains("dashboard_available")) {
        hasDashboard_ = truthy(payload.at("dashboard_available"));
    }
    if (payload.contains("meeting_id")) {
        const Json& id = payload.at("meeting_id");
        meetingId_ = truthy(id) ? std::optional<QString>(utf8(pyStr(id))) : std::nullopt;
    }
    applyIdentity(payload);
    if (payload.contains("finalization")) {
        setFinalization(payload.at("finalization"));
    }
    if (payload.contains("active")) {
        setActive(truthy(payload.at("active")));
    } else {
        applyLayoutState();
    }
}

void MeetingModeTab::applyBackground(const Json& payload) {
    if (payload.contains("background_available")) {
        backgroundAvailable_ = truthy(payload.at("background_available"));
    }
    if (payload.contains("background_message")) {
        const QString message = utf8(strOrEmpty(payload.at("background_message")));
        backgroundNotice_->setText(message);
        backgroundNotice_->setVisible(!message.isEmpty());
    }
}

void MeetingModeTab::applyCloudChoice(const Json& payload) {
    if (!payload.contains("cloud_enabled")) {
        return;
    }
    cloudCheckbox_->blockSignals(true);
    cloudCheckbox_->setChecked(truthy(payload.at("cloud_enabled")));
    cloudCheckbox_->blockSignals(false);
    // Blocked signals skip the toggled slots, so the brief (for instance after a declined
    // consent dialog) syncs here instead.
    syncBriefAvailability();
}

// Where remote meeting speech goes; local speech keeps the note.
void MeetingModeTab::applySpeechDestination(const Json& speech) {
    if (!(speech.is_object() && get(speech, "source") == Json("remote"))) {
        return;
    }
    std::string state;
    if (truthy(get(speech, "connected"))) {
        state = "Audio sent to this host; recordings saved here.";
    } else {
        const Json& message = get(speech, "message");
        state = truthy(message) ? pyStr(message) : std::string("Reconnecting…");
    }
    const auto field = [&speech](std::string_view key) {
        return speech.contains(key) ? pyStr(speech.at(key)) : std::string();
    };
    speechDestination_->setText(
        utf8("Remote speech · " + field("host") + " · " + field("model") + ". " + state));
}

double MeetingModeTab::now() const {
    return std::chrono::duration<double>(clock_.steady().time_since_epoch()).count();
}

// Sets the clock to the runtime's meeting time, anchored where elapsed_at says it was read.
void MeetingModeTab::restartElapsed(const Json& elapsedS, const Json& elapsedAt) {
    elapsedBaseS_ = py::py_float(elapsedS).value_or(0.0);
    if (paused_) {
        // A paused meeting's clock stays where the runtime put it.
        runningSince_.reset();
    } else if (const auto at = ui_meeting::detail::pythonNumber(elapsedAt)) {
        runningSince_ = *at;
    } else {
        runningSince_ = now();
    }
    refreshElapsed();
}

// Engine status may arrive before the runtime confirms startup: the session card stays with
// its controls disabled until then, and "starting" never flashes the idle column.
void MeetingModeTab::applyStatus(const std::string& status, const Json& payload) {
    const bool live = status == "active" || status == "paused";
    const bool confirmed = payload.contains("active") ? truthy(payload.at("active")) : active_;
    starting_ = status == "starting" || (starting_ && live && !confirmed);
    setStatusText(starting_ ? QStringLiteral("Starting") : utf8(py::capitalize(status)));
    // A new meeting start clears any previous finalization result.
    if (status == "starting") {
        backgroundAvailable_ = false;
        finalization_ = nullptr;
        identity_ = Json::object();
        // The engine snapshots its endpoint now; show that one.
        refreshAiDestination();
    }
}

void MeetingModeTab::applyIdentity(const Json& payload) {
    for (const std::string_view key : kIdentityKeys) {
        if (payload.contains(key)) {
            identity_[std::string(key)] = payload.at(key);
        }
    }
}

void MeetingModeTab::setRecordingActive(bool active) {
    recordingActive_ = active;
    applyLayoutState();
}

void MeetingModeTab::setStatusText(const QString& value) {
    if (!value.isEmpty()) {
        statusPill_->setText(value);
        statusPill_->setAccessibleName(QStringLiteral("Meeting status: %1").arg(value));
    }
}

void MeetingModeTab::setDashboardAvailable(bool available) {
    hasDashboard_ = available;
    applyLayoutState();
}

// A failed, unavailable or partly failed result: the card offers Retry and Keep for later.
bool MeetingModeTab::incompleteFinalization() const {
    if (!truthy(finalization_)) {
        return false;
    }
    const meeting::FinalizationView view = meeting::FinalizationView::read(finalization_);
    for (const Json& step : view.steps) {
        if (strOrEmpty(get(step, "status")) == "failed") {
            return true;
        }
    }
    return view.status == "failed" || view.status == "unavailable";
}

// Stores a finalization payload (null clears it and the identity) the way Python normalizes it.
void MeetingModeTab::setFinalization(const Json& value) {
    if (value.is_null()) {
        finalization_ = nullptr;
        identity_ = Json::object();
        return;
    }
    if (!value.is_object()) {
        return;
    }
    const std::string status = py::strip(strOrEmpty(get(value, "status")));
    if (status.empty()) {
        finalization_ = nullptr;
        return;
    }
    // Built whole before it is kept: a value int(), list() or dict() refuses raises here and
    // leaves the previous finalization in place, as Python's dict display does, rather than
    // storing a payload every later layout pass would refuse again.
    Json normalized = Json::object();
    normalized["status"] = status;
    normalized["message"] = strOrEmpty(get(value, "message"));
    normalized["stage"] = strOrEmpty(get(value, "stage"));
    normalized["current_step"] = ui_meeting::detail::intOrZero(get(value, "current_step"));
    normalized["total_steps"] = ui_meeting::detail::intOrZero(get(value, "total_steps"));
    normalized["step_details"] = strOrEmpty(get(value, "step_details"));
    normalized["steps"] = ui_meeting::detail::listOrEmpty(get(value, "steps"));
    normalized["summary_stats"] = ui_meeting::detail::dictOrEmpty(get(value, "summary_stats"));
    normalized["content_summary"] = ui_meeting::detail::dictOrEmpty(get(value, "content_summary"));
    finalization_ = std::move(normalized);
}

// Switches between the idle and in-meeting layouts.
void MeetingModeTab::setActive(bool active) {
    if (active == active_) {
        applyLayoutState();
        return;
    }
    active_ = active;
    if (active) {
        starting_ = false;
        // Starting a live session replaces any prior finalization result.
        finalization_ = nullptr;
        identity_ = Json::object();
        if (!runningSince_) {
            elapsedBaseS_ = 0.0;
            runningSince_ = now();
        }
        elapsedTimer_->start();
    } else {
        elapsedTimer_->stop();
        paused_ = false;
        elapsedBaseS_ = 0.0;
        runningSince_.reset();
        pauseButton_->setText(QStringLiteral("Pause"));
        pauseButton_->setAccessibleDescription(kPauseDescription);
        elapsedLabel_->setText(QStringLiteral("00:00"));
        elapsedLabel_->setAccessibleName(QStringLiteral("Meeting elapsed time: 00:00"));
    }
    applyLayoutState();
}

// Freezes or resumes the elapsed clock to mirror the meeting's.
void MeetingModeTab::setPaused(bool paused) {
    if (paused == paused_) {
        return;
    }
    paused_ = paused;
    const double at = now();
    if (paused) {
        if (runningSince_) {
            elapsedBaseS_ += at - *runningSince_;
            runningSince_.reset();
        }
        pauseButton_->setText(QStringLiteral("Resume"));
        pauseButton_->setAccessibleDescription(QStringLiteral("Resume meeting capture."));
    } else {
        runningSince_ = at;
        pauseButton_->setText(QStringLiteral("Pause"));
        pauseButton_->setAccessibleDescription(kPauseDescription);
    }
}

void MeetingModeTab::applyLayoutState() {
    const bool hasFinalization = truthy(finalization_);
    const bool runningFinalization = wrappingUp();
    const bool showFinalization = hasFinalization && !active_;
    const bool showingSession = active_ || starting_;

    intro_->setVisible(!showFinalization);
    sessionCard_->setVisible(showingSession);
    const bool setting = settingUp();
    idleCard_->setVisible(setting);
    liveActions_->setVisible(showingSession);
    // Start never leaves the column. It only waits while the last meeting still holds the
    // speech model; after that the wrap-up can finish in the background.
    const bool startBlocked = runningFinalization && !backgroundAvailable_;
    startButton_->setEnabled(!recordingActive_ && !startBlocked);
    demoButton_->setEnabled(!recordingActive_);
    QString note;
    if (startBlocked) {
        note = QStringLiteral("Your last meeting is still wrapping up. Start unlocks in a moment.");
    } else if (runningFinalization) {
        note = QStringLiteral("Your last meeting finishes wrapping up in the background.");
    } else if (incompleteFinalization()) {
        note = QStringLiteral(
            "Your last meeting stays in Past Meetings, where you can pick it up later.");
    }
    slotNote_->setText(note);
    slotNote_->setVisible(setting && !note.isEmpty());
    startButton_->setToolTip(startBlocked ? note : QString());
    syncBriefAvailability();
    showStage(showingSession     ? QStringLiteral("live")
              : showFinalization ? QStringLiteral("result")
                                 : QStringLiteral("empty"));
    syncLiveTone();
    // The switch always stays (it acts on a live meeting), but what it does and "Change…" only
    // matter while the next meeting is being set up.
    aiDetails_->setVisible(setting);
    aiSettingsButton_->setVisible(setting);
    aiCompactDestination_->setVisible(!setting);
    const QString aiName = showFinalization ? QStringLiteral("AI insights for next meeting")
                                            : QStringLiteral("AI insights");
    cloudCheckbox_->setText(aiName);
    cloudCheckbox_->setAccessibleName(aiName);
    aiLabel_->setText(aiName);
    const bool showDemo = developerMode_ && !active_ && !runningFinalization;
    demoButton_->setVisible(showDemo);
    demoHint_->setVisible(showDemo);
    finalizationCard_->setVisible(showFinalization);
    finalizationCard_->setBackgroundOffered(showFinalization && runningFinalization &&
                                            backgroundAvailable_);
    pauseButton_->setEnabled(active_);
    endButton_->setEnabled(active_);
    guestLinkButton_->setEnabled(active_ && hasDashboard_);

    if (showFinalization) {
        renderIdentity();
        finalizationCard_->render(meeting::FinalizationView::read(finalization_));
    }

    dashboardButton_->setEnabled(active_ || hasDashboard_);
    finalizationCard_->reportButton()->setVisible(showFinalization);
    finalizationCard_->syncFooter(showFinalization);
}

// ---- the stage ----

QString MeetingModeTab::stageState() const {
    return stageState_.value_or(QStringLiteral("empty"));
}

void MeetingModeTab::showStage(const QString& state) {
    const bool changed = state != stageState_;
    stageState_ = state;
    stage_->showPane(state == QStringLiteral("live")     ? livePane_
                     : state == QStringLiteral("result") ? resultPane_
                                                         : emptyPane_);
    if (changed && state == QStringLiteral("empty") && isVisible()) {
        loadRecent();
    }
}

// Colours the on-air card for what capture is doing right now.
void MeetingModeTab::syncLiveTone() {
    const QString tone = !active_  ? QStringLiteral("starting")
                         : paused_ ? QStringLiteral("paused")
                                   : QStringLiteral("live");
    for (QWidget* widget :
         {static_cast<QWidget*>(sessionCard_), static_cast<QWidget*>(statusPill_)}) {
        ui::setStyleProperty(widget, ui::prop::tone, tone);
    }
    liveDot_->setLive(tone == QStringLiteral("live"));
    // Pause becomes Resume, and a button sizes itself to its new label.
    for (QPushButton* button :
         {static_cast<QPushButton*>(pauseButton_), static_cast<QPushButton*>(endButton_)}) {
        button->setFixedWidth(ui::scaled(meeting_tab::kOnAirButtonWidth));
    }
}

// Sizes the column and the on-air card for the type size in use.
void MeetingModeTab::applySizes() {
    if (!built_) {
        return;
    }
    split_->setColumnWidth(ui::scaled(studio::kColumnWidth));
    topSlot_->setMinimumHeight(ui::scaled(meeting_tab::kTopSlotHeight));
    for (Button* button : {dashboardButton_, guestLinkButton_}) {
        button->setMinimumHeight(ui::scaled(meeting_tab::kTopSlotHeight));
    }
    sessionCard_->setMaximumWidth(ui::scaled(meeting_tab::kOnAirWidth));
    syncLiveTone();
}

void MeetingModeTab::changeEvent(QEvent* event) {
    QWidget::changeEvent(event);
    if (event->type() == QEvent::StyleChange) {
        // A new type size restyles every widget; the column follows it.
        applySizes();
    }
}

void MeetingModeTab::showEvent(QShowEvent* event) {
    QWidget::showEvent(event);
    refreshAiDestination();
    if (stageState() == QStringLiteral("empty")) {
        loadRecent();
    }
}

bool MeetingModeTab::event(QEvent* event) {
    // Coming back from Settings is how the model usually changes, and Past Meetings can delete
    // what the empty stage lists.
    if (event->type() == QEvent::WindowActivate) {
        refreshAiDestination();
        if (stageState() == QStringLiteral("empty") && isVisible()) {
            loadRecent();
        }
    }
    return QWidget::event(event);
}

int MeetingModeTab::preferredWidth() const {
    return split_->columnWidth() + studio::kGap + ui::scaled(meeting_tab::kStageWidth);
}

void MeetingModeTab::prepareForWidthChange(int grow) {
    // Measure the side-by-side layout a wider window will show, not the stacked one a narrow
    // window still holds, which is far taller.
    if (split_->width() > 0) {
        split_->placeForWidth(split_->width() + grow);
    }
}

std::optional<QString> MeetingModeTab::finalizationStatus() const {
    if (!truthy(finalization_)) {
        return std::nullopt;
    }
    return utf8(strOrEmpty(get(finalization_, "status")));
}

// ---- recent meetings (the empty stage) ----

// Reads the newest few meetings for the empty stage off the GUI thread.
void MeetingModeTab::loadRecent() {
    Q_EMIT recentMeetingsWanted();
    if (recentIo_ == nullptr || !recentReader_) {
        return;
    }
    const std::uint64_t token = recentGate_.next();
    std::shared_ptr<RecentReader> reader = recentReader_;
    std::shared_ptr<qt::QtUiExecutor> ui = ui_;
    recentIo_->post([this, reader, ui, token] {
        std::vector<Json> meetings;
        try {
            meetings = (*reader)();
        } catch (const std::exception& failure) {
            kLog.warning("Could not read recent meetings: {}", failure.what());
            meetings.clear();
        } catch (...) {
            // Python catches every Exception here: nothing a reader throws may end the worker.
            kLog.warning("Could not read recent meetings: unknown error");
            meetings.clear();
        }
        // `this` is used only on the GUI thread, by a task the tab's executor drops once the tab
        // is gone.
        ui->post([this, token, meetings = std::move(meetings)] {
            if (recentGate_.isCurrent(token)) {
                showRecentMeetings(meetings);
            }
        });
    });
}

void MeetingModeTab::setRecentMeetings(const QList<QJsonObject>& meetings) {
    std::vector<Json> rows;
    rows.reserve(static_cast<std::size_t>(meetings.size()));
    for (const QJsonObject& meeting : meetings) {
        rows.push_back(ui_meeting::detail::toJson(meeting));
    }
    showRecentMeetings(rows);
}

void MeetingModeTab::showRecentMeetings(const std::vector<Json>& meetings) {
    for (RecentMeetingRow* row : recentRows_) {
        row->hide();
        row->setParent(nullptr);
        row->deleteLater();
    }
    recentRows_.clear();
    for (const Json& meeting : meetings) {
        if (recentRows_.size() >= static_cast<std::size_t>(kRecentMeetingsShown)) {
            break;
        }
        auto* row = new RecentMeetingRow(meeting, clock_);
        connect(row, &RecentMeetingRow::clicked, this, &MeetingModeTab::pastMeetingRequested);
        recentLayout_->addWidget(row);
        recentRows_.push_back(row);
    }
    recentBox_->setVisible(!recentRows_.empty());
}

// Shows the meeting name, date and duration above its result.
void MeetingModeTab::renderIdentity() {
    const std::string title = py::strip(strOrEmpty(get(identity_, "display_title")).empty()
                                            ? strOrEmpty(get(identity_, "title"))
                                            : strOrEmpty(get(identity_, "display_title")));
    std::string meta;
    if (!identity_.empty()) {
        meta = meeting_state::format_meeting_identity_meta(identity_, clock_);
    }
    if (meta == UNKNOWN_DATE && !truthy(get(identity_, "started_at"))) {
        meta.clear();
    }
    finalizationCard_->showIdentity(utf8(title), utf8(meta));
}

// The elapsed label from the local pause-aware clock.
void MeetingModeTab::refreshElapsed() {
    double elapsed = elapsedBaseS_;
    if (runningSince_) {
        elapsed += now() - *runningSince_;
    }
    const QString shown = utf8(meeting::elapsed_text(elapsed));
    elapsedLabel_->setText(shown);
    elapsedLabel_->setAccessibleName(QStringLiteral("Meeting elapsed time: %1").arg(shown));
}

} // namespace fv
