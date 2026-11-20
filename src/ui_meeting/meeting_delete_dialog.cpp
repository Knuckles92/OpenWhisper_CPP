#include "ui_meeting/meeting_delete_dialog.h"

#include <QCheckBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QVBoxLayout>

#include "ui_widgets/buttons.h"

namespace fv {

MeetingDeleteDialog::MeetingDeleteDialog(QWidget* parent, bool hasAudio) : QDialog(parent) {
    setObjectName(QStringLiteral("meetingDeleteDialog"));
    setWindowTitle(QStringLiteral("Delete Meeting"));

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(20, 20, 20, 20);
    layout->setSpacing(16);
    layout->addWidget(new QLabel(QStringLiteral("Delete this meeting from Past Meetings?")));
    layout->addWidget(new QLabel(QStringLiteral("This cannot be undone.")));

    deleteRecordings_ =
        new QCheckBox(QStringLiteral("Permanently delete saved recordings too"), this);
    deleteRecordings_->setToolTip(
        QStringLiteral("Leave unchecked to keep this meeting's saved recordings."));
    layout->addWidget(deleteRecordings_);
    deleteRecordings_->setVisible(hasAudio);

    dontAskAgain_ = new QCheckBox(QStringLiteral("Don't ask me again"), this);
    layout->addWidget(dontAskAgain_);

    // Cancel, then the action on the right, as in the app's other dialogs; a platform button
    // box put Delete first on some desktops.
    buttons_ = new QWidget(this);
    auto* row = new QHBoxLayout(buttons_);
    row->setContentsMargins(0, 0, 0, 0);
    row->setSpacing(8);
    row->addStretch();
    cancelButton_ = neutralButton(new Button(QStringLiteral("Cancel")));
    connect(cancelButton_, &QPushButton::clicked, this, &QDialog::reject);
    row->addWidget(cancelButton_);
    deleteButton_ = compactDangerButton(new Button(QStringLiteral("Delete")));
    deleteButton_->setAutoDefault(false);
    connect(deleteButton_, &QPushButton::clicked, this, &QDialog::accept);
    row->addWidget(deleteButton_);
    layout->addWidget(buttons_);
    // Enter keeps the harmless choice.
    cancelButton_->setDefault(true);
    cancelButton_->setFocus();
}

} // namespace fv
