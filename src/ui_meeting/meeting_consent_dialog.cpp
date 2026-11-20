#include "ui_meeting/meeting_consent_dialog.h"

#include <QHBoxLayout>
#include <QLabel>
#include <QVBoxLayout>

#include "appkit/qt_bridge.h"
#include "base/identity.h"
#include "ui_theme/local_style.h"
#include "ui_theme/style_props.h"
#include "ui_widgets/buttons.h"
#include "ui_widgets/scrolling_body.h"
#include "ui_widgets/wrapped_label.h"

namespace fv {
namespace {

const QString kChosenIn = QStringLiteral("Settings → Meeting Mode → Intelligence");

// _location_text: where transcript text goes, for the chosen engine.
QString locationText(const MeetingConsentCopy& copy) {
    const QString destination = qt::toQString(copy.destination);
    if (!copy.agent_id.empty()) {
        return QStringLiteral(
                   "To do this, the meeting transcript text and the dashboard state are sent to "
                   "%1 on this computer, the coding agent chosen in %2. It runs them through its "
                   "own sign-in and model provider, so transcript text leaves this computer the "
                   "way its other requests do.\n\n%1 gets %3's meeting tools and nothing else: "
                   "no files, no shell.")
            .arg(destination, kChosenIn, qt::toQString(identity().product));
    }
    const QString sent = QStringLiteral("To do this, the meeting transcript text and the "
                                        "dashboard state are sent to %1, using the model chosen "
                                        "in %2.\n\n")
                             .arg(destination, kChosenIn);
    // OpenWhisper also names a Fast judgments judge that receives excerpts; OpenWhisper_CPP's
    // meetings use only a judge on this computer, which sends nothing.
    if (copy.remote) {
        return sent + QStringLiteral("That destination is remote, so transcript text leaves this "
                                     "computer.");
    }
    return sent + QStringLiteral("That destination is on this computer, so transcript text does "
                                 "not leave this machine.");
}

// _backup_text: who else may get the transcript if AI insights stop working.
QString backupText(const MeetingConsentCopy& copy) {
    if (copy.backup_name.empty()) {
        return {};
    }
    return QStringLiteral("If AI insights stop working during a meeting, %1, your chosen backup, "
                          "takes over and receives the transcript through its own sign-in.\n\n")
        .arg(qt::toQString(copy.backup_name));
}

} // namespace

QString MeetingConsentDialog::bodyText(const MeetingConsentCopy& copy) {
    return QStringLiteral("AI insights keep the meeting's topic, key points, decisions, action "
                          "items, and questions updated on the dashboard while you talk.\n\n") +
           locationText(copy) + QStringLiteral("\n\n") + backupText(copy) +
           QStringLiteral(
               "AI insights do not upload audio. Recordings are saved here. If Remote computer "
               "is selected for meeting speech, microphone and system audio are sent to your "
               "paired host for transcription. Speaker identification is a separate setting "
               "and, if enabled, uploads the system-audio recording after the meeting.\n\n"
               "Past-meeting recall is also off by default. If you later enable it in Settings, "
               "excerpts from earlier meetings may be sent so the agent can recall prior names "
               "and decisions.");
}

MeetingConsentDialog::MeetingConsentDialog(const MeetingConsentCopy& copy, QWidget* parent,
                                           bool backup)
    : QDialog(parent), remote_(copy.remote), agentId_(qt::toQString(copy.agent_id)) {
    setObjectName(QStringLiteral("meetingConsentDialog"));
    const QString destination = qt::toQString(copy.destination);
    setWindowTitle(backup ? QStringLiteral("Allow a Backup Agent")
                          : QStringLiteral("Turn On AI Insights"));
    setAccessibleName(backup ? QStringLiteral("Allow a backup agent for this meeting")
                             : QStringLiteral("Turn on AI insights for meetings"));
    setAccessibleDescription(QStringLiteral("Consent choice for sending transcript text and "
                                            "dashboard state to %1. Meeting audio is not "
                                            "uploaded.")
                                 .arg(destination));
    setMinimumWidth(480);
    setModal(true);

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(24, 24, 24, 24);
    layout->setSpacing(12);

    auto* title =
        new QLabel(backup ? QStringLiteral("Allow %1 as the backup agent?").arg(destination)
                          : QStringLiteral("Turn on AI insights for meetings?"));
    title->setObjectName(QStringLiteral("headerLabel"));
    title->setWordWrap(true);
    layout->addWidget(title);

    // The copy scrolls past a comfortable height; the title and the choices stay put.
    body_ = new ScrollingBody();
    layout->addWidget(body_, 1);

    // OpenWhisper_CPP authorizes the backup agent on its own route, after the primary.
    const QString text =
        backup ? QStringLiteral("If the primary agent cannot start, %1 can take over AI insights "
                                "for this meeting.\n\n")
                         .arg(destination) +
                     locationText(copy) +
                     QStringLiteral("\n\nAI insights do not upload audio. Recordings are saved "
                                    "here.")
               : bodyText(copy);
    auto* body = new WrappedLabel(text);
    body->setObjectName(QStringLiteral("consentBodyLabel"));
    body_->addWidget(body);

    auto* note = new WrappedLabel(QStringLiteral("You can turn this on or off for each meeting "
                                                 "with the \"AI insights\" switch. Without it, "
                                                 "meetings are transcript-only."));
    note->setObjectName(QStringLiteral("infoLabel"));
    body_->addWidget(note);

    layout->addSpacing(8);
    auto* row = new QHBoxLayout;
    row->setSpacing(8);
    row->addStretch();
    auto* notNow = neutralButton(new Button(QStringLiteral("Not now")));
    notNow->setObjectName(QStringLiteral("meetingConsentNotNowButton"));
    connect(notNow, &QPushButton::clicked, this, &QDialog::reject);
    row->addWidget(notNow);
    auto* enable = compactPrimaryButton(new Button(backup ? QStringLiteral("Allow backup agent")
                                                          : QStringLiteral("Turn on AI insights")));
    enable->setObjectName(QStringLiteral("meetingConsentEnableButton"));
    ui::setStyleProperty(enable, ui::prop::buttonId, QStringLiteral("meetingConsentEnableButton"));
    connect(enable, &QPushButton::clicked, this, [this] {
        resultAction_ = QString::fromLatin1(kResultEnable);
        accept();
    });
    row->addWidget(enable);
    enable->setDefault(true);
    layout->addLayout(row);
}

void MeetingConsentDialog::setVisible(bool visible) {
    if (visible && !fitted_) {
        // Before QDialog centers itself, so it centers the final size.
        fitted_ = true;
        body_->fitDialog(this);
    }
    QDialog::setVisible(visible);
}

} // namespace fv
