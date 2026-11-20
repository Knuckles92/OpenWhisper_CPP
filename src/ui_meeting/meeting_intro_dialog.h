#pragma once

#include <functional>

#include <QDialog>
#include <QString>

class QFrame;
class QWidget;

namespace fv {

class SettingsStore;

// The first-visit overview of Meeting Mode (ui_qt/dialogs/meeting_intro_dialog.py, objectName
// meetingIntroDialog): what the mode does, its main features and where its settings live. Skip
// and Got it both dismiss it for good. Affinity: GUI thread.
class MeetingModeIntroDialog : public QDialog {
    Q_OBJECT
public:
    // How the dialog was left: "skip" (Skip, Escape or the window's close button) or "got_it".
    static inline const QString kResultSkip = QStringLiteral("skip");
    static inline const QString kResultGotIt = QStringLiteral("got_it");

    explicit MeetingModeIntroDialog(QWidget* parent = nullptr);

    const QString& resultAction() const { return resultAction_; }

private:
    void setupUi();
    QFrame* section(const QString& heading, const QStringList& lines);
    void finish(const QString& action);

    QString resultAction_ = kResultSkip;
};

// maybe_show_meeting_mode_intro: shows the intro when settings say it has not been dismissed,
// then records that it was seen, whichever way it was left; a failed save is only logged.
// Returns whether the dialog was shown. run answers for exec() (a test stands in for the user).
bool maybeShowMeetingModeIntro(SettingsStore& settings, QWidget* parent = nullptr,
                               const std::function<void(MeetingModeIntroDialog&)>& run = {});

} // namespace fv
