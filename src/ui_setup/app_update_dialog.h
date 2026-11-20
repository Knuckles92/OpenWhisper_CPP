#pragma once

#include <optional>

#include <QDialog>
#include <QString>

#include "update/check.h"

class QCheckBox;
class QFrame;
class QLabel;
class QWidget;

namespace fv {

class AnimatedProgressBar;
class Button;
class NotesView;
class PrimaryButton;
class SettingsStore;
class WrappedLabel;

// The update-available and check-result dialog (ui_qt/dialogs/app_update_dialog.py): what the
// check found, the release notes, a download with its progress, and the two opt-out boxes that
// write the same settings as Settings > General. Every sentence comes from update/update_copy.h.
// The dialog opens nothing and starts no work: its three signals ask the owner of the update
// (the controller) to download, cancel or fall back to the setup, and the owner feeds back
// setProgress, markHandedOff and setError. The opt-outs are saved once, as the dialog closes.
// Closing the window is "Later" and saves them, except during a download, where it cancels: the
// first close or Cancel asks the owner to stop and says so, the second closes the window.
// Affinity: GUI thread.
class AppUpdateDialog : public QDialog {
    Q_OBJECT
public:
    enum class ResultAction { Later, Primary, UseSetup };

    // result: the check's outcome, nullopt when the check failed (then error is its sentence).
    // can_download: someone listens to downloadRequested and setupRequested; without one the
    // primary button only closes the dialog, as when a release cannot be applied here.
    AppUpdateDialog(std::optional<update::UpdateCheckResult> result, QString error,
                    SettingsStore& settings, bool can_download, QWidget* parent = nullptr);

    // Shows the progress of a download or install. An unknown phase name keeps the body line
    // and shows the release name (as "opening" does); "restarting" is markHandedOff() as well.
    void setProgress(const QString& phase, qint64 done, qint64 total);
    void setProgress(update::DownloadPhase phase, qint64 done, qint64 total);
    // The updater owns the install now, so nothing here can cancel it. The dialog also stops
    // refusing to close: a window that vetoes QApplication::quit() would stall the updater.
    void markHandedOff();
    // Gives the dialog back after a failed download or prepare. offerSetup shows "Use installer
    // instead" when the result is a native-mode one whose release has a verified setup asset.
    void setError(const QString& message, bool offerSetup = false);

    // What the user chose; Later until the primary or setup button is used.
    ResultAction resultAction() const { return resultAction_; }
    // A download or install is running and the window refuses to close without canceling.
    bool busy() const { return busy_; }

    WrappedLabel* titleLabel() const { return titleLabel_; }
    WrappedLabel* bodyLabel() const { return bodyLabel_; }
    WrappedLabel* hintLabel() const { return hintLabel_; }
    QFrame* notesCard() const { return notesCard_; }
    NotesView* notesView() const { return notesView_; }
    QFrame* progressPanel() const { return progressPanel_; }
    AnimatedProgressBar* progressBar() const { return progressBar_; }
    QLabel* progressDetail() const { return progressDetail_; }
    QLabel* progressPercent() const { return progressPercent_; }
    QWidget* optionsWidget() const { return optionsWidget_; }
    QCheckBox* dontNotifyCheck() const { return dontNotifyCheck_; }
    QCheckBox* dontCheckCheck() const { return dontCheckCheck_; }
    Button* setupButton() const { return setupButton_; }
    Button* laterButton() const { return laterButton_; }
    PrimaryButton* primaryButton() const { return primaryButton_; }

    // Closing the window is Later (saving the opt-outs), or Cancel while a download runs.
    void reject() override;

Q_SIGNALS:
    void downloadRequested();
    void cancelRequested();
    void setupRequested();

protected:
    void showEvent(QShowEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;

private:
    void setupUi();
    QFrame* buildNotesCard();
    QFrame* buildProgressPanel();
    void onLater();
    void onCancel();
    void onUseSetup();
    void onPrimary();
    void persist();
    void setDownloading();
    void setBodyTone(const char* tone);
    // Sizes the notes to their text between the floor and the point where they scroll.
    void fitNotesHeight();
    // Shrinks the window when a section is hidden: hiding the notes only lowers the layout's
    // minimum, and the window would keep the height it was given.
    void resizeToContent();

    std::optional<update::UpdateCheckResult> result_;
    QString error_;
    SettingsStore& settings_;
    bool canDownload_;
    bool hasNotes_ = false;
    bool persisted_ = false;
    bool busy_ = false;
    bool cancelRequested_ = false;
    ResultAction resultAction_ = ResultAction::Later;

    WrappedLabel* titleLabel_ = nullptr;
    WrappedLabel* bodyLabel_ = nullptr;
    QFrame* notesCard_ = nullptr;
    NotesView* notesView_ = nullptr;
    WrappedLabel* hintLabel_ = nullptr;
    QFrame* progressPanel_ = nullptr;
    AnimatedProgressBar* progressBar_ = nullptr;
    QLabel* progressDetail_ = nullptr;
    QLabel* progressPercent_ = nullptr;
    QWidget* optionsWidget_ = nullptr;
    QCheckBox* dontNotifyCheck_ = nullptr;
    QCheckBox* dontCheckCheck_ = nullptr;
    Button* setupButton_ = nullptr;
    Button* laterButton_ = nullptr;
    PrimaryButton* primaryButton_ = nullptr;
};

} // namespace fv
