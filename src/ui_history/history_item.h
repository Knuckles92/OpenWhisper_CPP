#pragma once

#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <vector>

#include <QFrame>
#include <QPoint>
#include <QString>

#include "history/entry.h"

class QLabel;
class QMenu;
class QPushButton;
class QVBoxLayout;

namespace fv {

class ChipFlow;
class Executor;
namespace qt {
class QtUiExecutor;
}
class ElidingLabel;
class HistoryStore;
class PlaybackControl;
class WrappedLabel;

// One History entry in the sidebar (history_sidebar.py HistoryItemWidget): the time, the
// recording's size and the model on top; chips for the kind, the AI cleanup, the app it went
// into and where it is kept; the title and the preview; and, with a recording, Play, the time
// played and "Transcribe again" over a thin progress line. A left click outside its buttons
// opens the entry; a right click shows its menu.
// Affinity: GUI thread.
class HistoryItemWidget : public QFrame {
    Q_OBJECT
public:
    // store answers the card's recording lookup (get_recording_path) and must outlive it.
    HistoryItemWidget(HistoryEntry entry, HistoryStore& store, QWidget* parent = nullptr);
    ~HistoryItemWidget() override;

    const HistoryEntry& entry() const { return entry_; }
    // Null without a recording to play.
    PlaybackControl* playback() const { return playback_; }
    // The parts OpenWhisper's tests read; null when the card has none.
    QLabel* timestampLabel() const { return timestampLabel_; }
    QLabel* audioChip() const { return audioChip_; }
    QLabel* modelBadge() const { return modelBadge_; }
    ChipFlow* chips() const { return chips_; }
    ElidingLabel* kindChip() const { return kindChip_; }
    ElidingLabel* cleanupChip() const { return cleanupChip_; }
    ElidingLabel* appChip() const { return appChip_; }
    ElidingLabel* locationChip() const { return locationChip_; }
    WrappedLabel* titleLabel() const { return titleLabel_; }
    WrappedLabel* previewLabel() const { return previewLabel_; }
    QPushButton* retranscribeButton() const { return retranscribeButton_; }

    // The right-click menu, parented to the card, without showing it (_show_context_menu builds
    // and runs it). The caller deletes it.
    QMenu* buildContextMenu();
    void showContextMenu(const QPoint& pos);
    // _on_show_in_folder: reveals the recording; the path can go stale after deletion.
    void showInFolder();
    // _request_retranscribe: asks for this recording to be transcribed again; a host-kept one
    // is fetched first ("Getting it from <host>…").
    void requestRetranscribe();
    // How this host-kept entry's recording is fetched, on `io`, for playing it and for
    // transcribing it again.
    void setRemoteAudio(Executor& io, std::function<std::filesystem::path()> fetch);

    // Shows the recording in the file manager (revealInFileManager unless a test replaces it).
    std::function<bool(const QString& path)> reveal;

Q_SIGNALS:
    void clicked(const QString& entryId);
    void copyRequested(const QString& entryId);
    void copyRawRequested(const QString& entryId);
    void deleteRequested(const QString& entryId);
    // The recording's path.
    void retranscribeRequested(const QString& audioPath);
    // (entry id, version): show the original or the AI's version.
    void versionRequested(const QString& entryId, const QString& version);

protected:
    void mousePressEvent(QMouseEvent* event) override;

private:
    bool hasAudio() const;
    QString retranscribeLabel() const;
    void setupUi();
    void makeChips(const QString& kindText, std::vector<QWidget*>& chips);
    void addAudioFooter(QVBoxLayout* layout);
    ElidingLabel* chip(const char* name, const QString& text);
    void applyStyle();

    HistoryEntry entry_;
    bool failedRecording_ = false;
    QString cleanupInfo_;
    std::optional<std::filesystem::path> audioPath_;
    bool storedOnHost_ = false;
    Executor* io_ = nullptr;
    std::function<std::filesystem::path()> fetch_;
    std::shared_ptr<qt::QtUiExecutor> ui_;
    PlaybackControl* playback_ = nullptr;
    QLabel* timestampLabel_ = nullptr;
    QLabel* audioChip_ = nullptr;
    QLabel* modelBadge_ = nullptr;
    ChipFlow* chips_ = nullptr;
    ElidingLabel* kindChip_ = nullptr;
    ElidingLabel* cleanupChip_ = nullptr;
    ElidingLabel* appChip_ = nullptr;
    ElidingLabel* locationChip_ = nullptr;
    WrappedLabel* titleLabel_ = nullptr;
    WrappedLabel* previewLabel_ = nullptr;
    QPushButton* retranscribeButton_ = nullptr;
};

} // namespace fv
