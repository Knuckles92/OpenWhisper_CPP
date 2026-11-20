#pragma once

#include <filesystem>
#include <functional>

#include <QFrame>
#include <QPoint>
#include <QString>

#include "base/clock.h"
#include "base/json.h"

class QLabel;
class QMenu;

namespace fv {

// One persisted meeting in the Past Meetings list (past_meetings_panel.py PastMeetingItem,
// objectName pastMeetingItem): the title, the start, a preview of the transcript, a warning when
// nothing was captured, the length with its lifecycle word, and the insights and location pills.
// A press with the left button selects it; a right click shows its menu. The fonts are Segoe UI
// as in OpenWhisper, whatever the theme sets.
// Affinity: GUI thread.
class PastMeetingItem : public QFrame {
    Q_OBJECT
public:
    explicit PastMeetingItem(const Json& meeting, const Clock& clock = system_clock(),
                             QWidget* parent = nullptr);

    // The row the card shows; setMeeting keeps the card but replaces what it remembers, for a row
    // whose drawn fields are unchanged.
    const Json& meeting() const { return meeting_; }
    void setMeeting(const Json& meeting) { meeting_ = meeting; }
    const QString& meetingId() const { return meetingId_; }
    // The paired host that keeps this meeting when it is not kept here, else empty.
    const QString& storedOn() const { return storedOn_; }

    QLabel* titleLabel() const { return titleLabel_; }
    QLabel* dateLabel() const { return dateLabel_; }
    QLabel* previewLabel() const { return previewLabel_; }
    QLabel* contentLabel() const { return contentLabel_; }
    QLabel* detailLabel() const { return detailLabel_; }
    QLabel* insightsPill() const { return insightsPill_; }
    QLabel* locationPill() const { return locationPill_; }

    // set_progress: a fetch from the host under way, shown where the warning goes ("" hides it).
    void setProgress(const QString& text);
    // Marks this card as the meeting shown on the Meeting Mode tab.
    void setSelected(bool selected);
    void setSpeakerUploadAvailable(bool available) { speakerUploadAvailable_ = available; }

    // The absolute path of this meeting's recording folder, empty when it never spooled or the
    // folder is gone. A relative spool_dir resolves against the working directory.
    std::filesystem::path spoolDir() const;
    // _on_show_in_folder: opens the recording folder; it can go stale after a clear.
    void showInFolder();

    // The right-click menu, parented to the card, without showing it. The caller deletes it.
    QMenu* buildContextMenu();
    void showContextMenu(const QPoint& pos);

    // Opens a folder in the file manager (openFolderInFileManager unless a test replaces it).
    std::function<bool(const QString& folder)> openFolder;

Q_SIGNALS:
    void meetingSelected(const QString& meetingId);
    void copyTranscriptRequested(const QString& meetingId);
    void speakerUploadRequested(const QString& meetingId);
    void hostCopyDeleteRequested(const QString& meetingId, const QString& host,
                                 const QString& title, const QString& fingerprint,
                                 const QString& revision);
    void deleteRequested(const QString& meetingId);

protected:
    void mousePressEvent(QMouseEvent* event) override;

private:
    void emitSelected();

    Json meeting_;
    bool speakerUploadAvailable_ = false;
    QString meetingId_;
    QString storedOn_;
    QLabel* titleLabel_;
    QLabel* dateLabel_;
    QLabel* previewLabel_;
    QLabel* contentLabel_;
    QLabel* detailLabel_;
    QLabel* insightsPill_;
    QLabel* locationPill_;
};

} // namespace fv
