#pragma once

#include <QDialog>

class QCheckBox;
class QWidget;

namespace fv {

class Button;

// Meeting deletion options (ui_qt/dialogs/meeting_delete_dialog.py, objectName
// meetingDeleteDialog): whether to delete the saved recordings too (offered only for a meeting
// that has some), and whether to ask again. Cancel comes first and is the default; Delete is
// never the auto-default, since a platform button box put Delete first on some desktops.
// Affinity: GUI thread.
class MeetingDeleteDialog : public QDialog {
    Q_OBJECT
public:
    explicit MeetingDeleteDialog(QWidget* parent = nullptr, bool hasAudio = false);

    QCheckBox* deleteRecordings() const { return deleteRecordings_; }
    QCheckBox* dontAskAgain() const { return dontAskAgain_; }
    // The row holding the two buttons.
    QWidget* buttons() const { return buttons_; }
    Button* cancelButton() const { return cancelButton_; }
    Button* deleteButton() const { return deleteButton_; }

private:
    QCheckBox* deleteRecordings_;
    QCheckBox* dontAskAgain_;
    QWidget* buttons_;
    Button* cancelButton_;
    Button* deleteButton_;
};

} // namespace fv
