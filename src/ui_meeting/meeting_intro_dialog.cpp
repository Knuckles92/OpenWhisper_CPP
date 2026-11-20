#include "ui_meeting/meeting_intro_dialog.h"

#include <exception>

#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QStringList>
#include <QVBoxLayout>

#include "base/log.h"
#include "settings/keys.h"
#include "settings/store.h"
#include "ui_widgets/buttons.h"

namespace fv {
namespace {

constexpr log::Logger kLog{"ui_qt.dialogs.meeting_intro_dialog"};

const QStringList kFeatures = {
    QStringLiteral("Live transcript and a shareable browser dashboard"),
    QStringLiteral("Optional AI insights: topics, decisions, action items"),
    QStringLiteral("Speaker labels, playback, and export"),
    QStringLiteral("Past meetings stay in the sidebar"),
};

const QStringList kSettings = {
    QStringLiteral("Settings → Meeting Mode — voice model, language, and speakers"),
    QStringLiteral("Settings → Meeting Mode — intelligence, recall, dashboard, and after-meeting "
                   "steps"),
};

} // namespace

MeetingModeIntroDialog::MeetingModeIntroDialog(QWidget* parent) : QDialog(parent) {
    setObjectName(QStringLiteral("meetingIntroDialog"));
    setWindowTitle(QStringLiteral("Meeting Mode (Beta)"));
    setAccessibleName(QStringLiteral("Welcome to Meeting Mode"));
    setAccessibleDescription(QStringLiteral(
        "Overview of meeting capture, the browser dashboard, and Meeting settings."));
    setMinimumWidth(460);
    setMaximumWidth(520);
    setModal(true);
    setupUi();
}

void MeetingModeIntroDialog::setupUi() {
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(24, 24, 24, 24);
    layout->setSpacing(14);

    auto* title = new QLabel(QStringLiteral("Welcome to Meeting Mode"));
    title->setObjectName(QStringLiteral("headerLabel"));
    title->setWordWrap(true);
    layout->addWidget(title);

    auto* lead = new QLabel(
        QStringLiteral("Record a meeting — your mic and the other side — and follow a live "
                       "transcript in the browser.\n\n"
                       "Meeting Mode is in beta. Transcripts and insights may be inaccurate."));
    lead->setObjectName(QStringLiteral("meetingIntroLead"));
    lead->setWordWrap(true);
    layout->addWidget(lead);

    layout->addWidget(section(QStringLiteral("Features"), kFeatures));
    layout->addWidget(section(QStringLiteral("Settings"), kSettings));

    auto* skipHint = new QLabel(QStringLiteral("Skip anytime. This only appears once."));
    skipHint->setObjectName(QStringLiteral("infoLabel"));
    skipHint->setWordWrap(true);
    layout->addWidget(skipHint);

    layout->addSpacing(4);
    auto* buttonLayout = new QHBoxLayout;
    buttonLayout->setSpacing(8);
    buttonLayout->addStretch();

    Button* skip = neutralButton(new Button(QStringLiteral("Skip")));
    skip->setObjectName(QStringLiteral("meetingIntroSkipButton"));
    skip->setAutoDefault(false);
    skip->setDefault(false);
    connect(skip, &QPushButton::clicked, this, &QDialog::reject);
    buttonLayout->addWidget(skip);

    // The tone keeps the primary look under the lookup name; renaming a PrimaryButton dropped
    // it, which left "Got it" an underlined link.
    Button* gotIt = compactPrimaryButton(new Button(QStringLiteral("Got it")));
    gotIt->setObjectName(QStringLiteral("meetingIntroGotItButton"));
    gotIt->setDefault(true);
    connect(gotIt, &QPushButton::clicked, this, [this] { finish(kResultGotIt); });
    buttonLayout->addWidget(gotIt);

    layout->addLayout(buttonLayout);
}

QFrame* MeetingModeIntroDialog::section(const QString& heading, const QStringList& lines) {
    auto* frame = new QFrame;
    frame->setObjectName(QStringLiteral("meetingIntroSection"));
    frame->setAttribute(Qt::WA_StyledBackground, true);
    auto* sectionLayout = new QVBoxLayout(frame);
    sectionLayout->setContentsMargins(14, 12, 14, 12);
    sectionLayout->setSpacing(6);

    auto* header = new QLabel(heading);
    header->setObjectName(QStringLiteral("sectionLabel"));
    sectionLayout->addWidget(header);

    for (const QString& line : lines) {
        auto* row = new QHBoxLayout;
        row->setSpacing(8);
        row->setContentsMargins(0, 0, 0, 0);
        auto* mark = new QLabel(QStringLiteral("•"));
        mark->setObjectName(QStringLiteral("meetingIntroBullet"));
        mark->setAlignment(Qt::AlignTop);
        auto* body = new QLabel(line);
        body->setObjectName(QStringLiteral("meetingIntroItem"));
        body->setWordWrap(true);
        row->addWidget(mark, 0, Qt::AlignTop);
        row->addWidget(body, 1);
        sectionLayout->addLayout(row);
    }
    return frame;
}

void MeetingModeIntroDialog::finish(const QString& action) {
    resultAction_ = action;
    accept();
}

bool maybeShowMeetingModeIntro(SettingsStore& settings, QWidget* parent,
                               const std::function<void(MeetingModeIntroDialog&)>& run) {
    if (settings.get(keys::meeting_mode_intro_seen)) {
        return false;
    }
    MeetingModeIntroDialog dialog(parent);
    if (run) {
        run(dialog);
    } else {
        dialog.exec();
    }
    try {
        settings.save_setting(keys::meeting_mode_intro_seen.key(), true);
    } catch (const std::exception& error) {
        kLog.warning("Could not persist Meeting Mode intro: {}", error.what());
    }
    return true;
}

} // namespace fv
