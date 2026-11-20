#pragma once

#include <functional>

#include <QDialog>
#include <QJsonObject>
#include <QList>
#include <QString>

class QFrame;

namespace fv {

// The startup dialog listing meetings an exit cut short (ui_qt/dialogs/meeting_recovery_dialog.py,
// objectName meetingRecoveryDialog). Each can be finalized (the captured audio is transcribed and
// the session closed out into history) or discarded (the session and its spooled audio are
// deleted). The actions run through the callbacks, which receive the meeting id; a row disables
// itself once acted on. Rows use id, title and started_at.
// Affinity: GUI thread.
class MeetingRecoveryDialog : public QDialog {
    Q_OBJECT
public:
    using Action = std::function<void(const QString& meetingId)>;

    explicit MeetingRecoveryDialog(const QList<QJsonObject>& meetings, Action onFinalize = {},
                                   Action onDiscard = {}, QWidget* parent = nullptr);

    // Read when a button is pressed, so a caller may set them after construction.
    Action onFinalize;
    Action onDiscard;

private:
    QFrame* buildRow(const QJsonObject& meeting);
};

} // namespace fv
