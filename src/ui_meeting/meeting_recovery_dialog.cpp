#include "ui_meeting/meeting_recovery_dialog.h"

#include <optional>
#include <utility>

#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QVBoxLayout>

#include "appkit/qt_bridge.h"
#include "base/identity.h"
#include "base/log.h"
#include "pyport/text.h"
#include "ui_meeting/detail/payload.h"
#include "ui_theme/style_props.h"
#include "ui_widgets/buttons.h"

namespace fv {
namespace {

constexpr log::Logger kLog{"ui_qt.dialogs.meeting_recovery_dialog"};

QString text(const std::string& utf8) {
    return qt::toQString(utf8);
}

} // namespace

MeetingRecoveryDialog::MeetingRecoveryDialog(const QList<QJsonObject>& meetings, Action finalize,
                                             Action discard, QWidget* parent)
    : QDialog(parent), onFinalize(std::move(finalize)), onDiscard(std::move(discard)) {
    setObjectName(QStringLiteral("meetingRecoveryDialog"));
    setWindowTitle(QStringLiteral("Recover Interrupted Meetings"));
    setAccessibleName(QStringLiteral("Recover interrupted meetings"));
    setAccessibleDescription(
        QStringLiteral("Finalize captured audio into meeting history or permanently discard it."));
    setMinimumWidth(520);
    setModal(true);

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(24, 24, 24, 24);
    layout->setSpacing(12);

    auto* title = new QLabel(QStringLiteral("Some meetings did not finish"));
    title->setObjectName(QStringLiteral("headerLabel"));
    layout->addWidget(title);

    auto* body = new QLabel(
        text(std::string(identity().product)) +
        QStringLiteral(" closed while these meetings were running. Their audio is safe on disk. "
                       "Finalize a meeting to transcribe what was captured and keep it in "
                       "history, or discard it to delete the session and its audio."));
    body->setObjectName(QStringLiteral("infoLabel"));
    body->setWordWrap(true);
    layout->addWidget(body);

    for (const QJsonObject& meeting : meetings) {
        layout->addWidget(buildRow(meeting));
    }

    layout->addSpacing(8);
    auto* buttonLayout = new QHBoxLayout;
    buttonLayout->addStretch();
    auto* close = new Button(QStringLiteral("Close"));
    close->setObjectName(QStringLiteral("meetingRecoveryCloseButton"));
    connect(close, &QPushButton::clicked, this, &QDialog::accept);
    close->setDefault(true);
    buttonLayout->addWidget(close);
    layout->addLayout(buttonLayout);
}

QFrame* MeetingRecoveryDialog::buildRow(const QJsonObject& meeting) {
    using ui_meeting::detail::get;
    using ui_meeting::detail::pyStr;
    using ui_meeting::detail::strOrEmpty;
    using ui_meeting::detail::truthy;
    const Json fields = ui_meeting::detail::toJson(meeting);
    const std::string idUtf8 = fields.contains("id") ? pyStr(fields["id"]) : std::string();
    const QString meetingId = text(idUtf8);

    auto* row = new QFrame;
    row->setObjectName(QStringLiteral("meetingRecoveryRow"));
    auto* rowLayout = new QHBoxLayout(row);
    rowLayout->setContentsMargins(12, 8, 12, 8);
    rowLayout->setSpacing(8);

    const Json& stored = get(fields, "title");
    const QString title = truthy(stored) ? text(pyStr(stored)) : QStringLiteral("Untitled meeting");
    std::string startedAt = strOrEmpty(get(fields, "started_at"));
    // ISO timestamps read fine truncated to minutes.
    for (char& letter : startedAt) {
        if (letter == 'T') {
            letter = ' ';
        }
    }
    const QString started = text(py::slice(startedAt, std::nullopt, 16));

    auto* label = new QLabel(started.isEmpty() ? title : title + QStringLiteral(" — ") + started);
    label->setObjectName(QStringLiteral("meetingRecoveryRowLabel"));
    rowLayout->addWidget(label, 1);

    auto* finalize = new PrimaryButton(QStringLiteral("Finalize"));
    ui::setStyleProperty(finalize, ui::prop::buttonId,
                         QStringLiteral("meetingRecoveryFinalizeButton"));
    finalize->setAccessibleName(QStringLiteral("Finalize %1").arg(title));
    finalize->setAccessibleDescription(
        QStringLiteral("Transcribe the captured audio and keep this meeting in history."));
    auto* discard = new DangerButton(QStringLiteral("Discard"));
    ui::setStyleProperty(discard, ui::prop::buttonId,
                         QStringLiteral("meetingRecoveryDiscardButton"));
    discard->setAccessibleName(QStringLiteral("Discard %1").arg(title));
    discard->setAccessibleDescription(
        QStringLiteral("Permanently delete this interrupted meeting and its captured audio."));

    const auto settle = [finalize, discard, label](const QString& actionText) {
        finalize->setEnabled(false);
        discard->setEnabled(false);
        label->setText(label->text() + QStringLiteral("  (%1)").arg(actionText));
    };
    connect(finalize, &QPushButton::clicked, this, [this, meetingId, idUtf8, settle] {
        kLog.info("Recovery: finalize meeting '{}'", idUtf8);
        if (onFinalize) {
            onFinalize(meetingId);
        }
        settle(QStringLiteral("finalizing"));
    });
    connect(discard, &QPushButton::clicked, this, [this, meetingId, idUtf8, settle] {
        kLog.info("Recovery: discard meeting '{}'", idUtf8);
        if (onDiscard) {
            onDiscard(meetingId);
        }
        settle(QStringLiteral("discarded"));
    });
    rowLayout->addWidget(finalize);
    rowLayout->addWidget(discard);
    return row;
}

} // namespace fv
