#pragma once

#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <optional>

#include <QDialog>
#include <QString>

#include "appkit/qt_bridge.h"
#include "history/entry.h"
#include "ui_history/manual_cleanup_dialog.h"

class QButtonGroup;
class QFrame;
class QLabel;
class QMessageBox;
class QPushButton;
class QTextEdit;
class QWidget;

namespace fv {

class Button;
class Executor;
class HistoryStore;
class PlaybackControl;

// The viewer of one History entry (ui_qt/dialogs/history_entry_dialog.py): the time and chips,
// the details, the transcript with its Original and AI versions, playback of the recording, and
// Copy, Copy Raw, Transcribe Again, Delete and Close. Ctrl+C copies the version shown. Playback
// stops when the dialog closes or is hidden.
// Affinity: GUI thread.
class HistoryEntryDialog : public QDialog {
    Q_OBJECT
public:
    // store answers the recording lookup and saves a version choice on io; both must outlive
    // the dialog.
    HistoryEntryDialog(HistoryEntry entry, std::shared_ptr<HistoryStore> store, Executor& io,
                       QWidget* parent = nullptr,
                       std::optional<bool> agentServiceAvailable = std::nullopt);
    ~HistoryEntryDialog() override;

    const HistoryEntry& entry() const { return entry_; }
    QTextEdit* transcriptText() const { return transcriptText_; }
    QWidget* versionToggle() const { return versionToggle_; }
    QPushButton* rawButton() const { return rawButton_; }
    QPushButton* fixedButton() const { return fixedButton_; }
    Button* useVersionButton() const { return useVersionButton_; }
    QLabel* cleanupChip() const { return cleanupChip_; }
    Button* playButton() const { return playButton_; }
    PlaybackControl* playback() const { return playback_; }
    Button* copyButton() const { return copyButton_; }
    Button* copyRawButton() const { return copyRawButton_; }
    Button* agentCleanupButton() const { return agentCleanupButton_; }
    Button* retranscribeButton() const { return retranscribeButton_; }
    Button* deleteButton() const { return deleteButton_; }
    // The value labels of the details, by caption ("Audio duration", "Model", ...).
    const std::map<QString, QLabel*>& factLabels() const { return factLabels_; }

    // Puts text on the clipboard; false when that failed.
    std::function<bool(const QString& text)> copyText;
    // Runs on io after a successful save, even when this dialog has closed. The receiver
    // posts to an executor owned by the history view.
    std::function<void()> onVersionSaved;
    ManualCleanupDialog::Runner cleanupRunner;
    std::function<void(ManualCleanupDialog&)> runCleanupDialog;
    // Runs the delete confirmation and returns the button pressed (exec unless a test answers).
    std::function<int(QMessageBox&)> runMessageBox;

    void done(int result) override;

Q_SIGNALS:
    void copied();
    // The recording's path; the dialog then closes.
    void retranscribeRequested(const QString& audioPath);
    // After the user confirmed; the dialog then closes.
    void deleteRequested(const QString& entryId);
    // The entry shows another version of its text now; History refreshes.
    void versionChanged(const QString& entryId);

protected:
    void hideEvent(QHideEvent* event) override;

private:
    void setupUi();
    QFrame* buildMetadataSection();
    void readVersions(const HistoryEntry& entry);
    bool hadRecording() const;
    void refreshCleanupChip();
    QString shownVersion() const;
    void refreshUseVersion();
    void onVersionToggled(bool checked);
    void useShownVersion();
    QString shownText() const;
    void copyShownText();
    void copyRawText();
    void onRetranscribe();
    void onDelete();

    HistoryEntry entry_;
    std::shared_ptr<HistoryStore> store_;
    Executor& io_;
    std::shared_ptr<qt::QtUiExecutor> ui_;
    bool failedRecording_ = false;
    bool agentCleanupAvailable_ = false;
    std::string version_;
    QString fixedText_;
    std::optional<QString> rawText_;
    bool showingRaw_ = false;
    std::optional<std::filesystem::path> audioPath_;

    QLabel* cleanupChip_ = nullptr;
    QWidget* versionToggle_ = nullptr;
    QButtonGroup* versionGroup_ = nullptr;
    QPushButton* rawButton_ = nullptr;
    QPushButton* fixedButton_ = nullptr;
    Button* useVersionButton_ = nullptr;
    QTextEdit* transcriptText_ = nullptr;
    Button* playButton_ = nullptr;
    PlaybackControl* playback_ = nullptr;
    Button* copyButton_ = nullptr;
    Button* copyRawButton_ = nullptr;
    Button* agentCleanupButton_ = nullptr;
    Button* retranscribeButton_ = nullptr;
    Button* deleteButton_ = nullptr;
    std::map<QString, QLabel*> factLabels_;
};

} // namespace fv
