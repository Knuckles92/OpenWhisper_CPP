#pragma once

#include <QMetaMethod>
#include <QString>
#include <QWidget>

class QLabel;

namespace fv {

class DangerButton;
class SuccessButton;
class WarningButton;

// The three-button controller the compact main window shows in place of the tabs: a status line
// and Record, Stop and Cancel. Affinity: GUI thread.
class CompactRecordController : public QWidget {
    Q_OBJECT
public:
    explicit CompactRecordController(QWidget* parent = nullptr);

    // Record and Stop share the record key.
    void updateHotkeys(const QString& recordKey, const QString& cancelKey);

    QLabel* statusLabel() const { return statusLabel_; }
    SuccessButton* recordButton() const { return recordButton_; }
    DangerButton* stopButton() const { return stopButton_; }
    WarningButton* cancelButton() const { return cancelButton_; }
    // Whether anything is connected to signal ("connects every signal" tests).
    bool hasReceivers(const QMetaMethod& signal) const { return isSignalConnected(signal); }

    // Slots, because the main window calls them by name (setRecordingState(bool),
    // setStatus(QString)) on the controller it is given (lead amendment 21).
public Q_SLOTS:
    // Record is inactive and reads "Recording" while recording; Stop and Cancel are active then.
    void setRecordingState(bool recording);
    void setStatus(const QString& statusText);

Q_SIGNALS:
    void recordRequested();
    void stopRequested();
    void cancelRequested();

private:
    QLabel* statusLabel_;
    SuccessButton* recordButton_;
    DangerButton* stopButton_;
    WarningButton* cancelButton_;
};

} // namespace fv
