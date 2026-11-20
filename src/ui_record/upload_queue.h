#pragma once

#include <optional>

#include <QFrame>
#include <QHash>
#include <QList>
#include <QString>
#include <QStringList>
#include <QWidget>

#include "audio/file_preview.h"

class QLabel;
class QPushButton;
class QVBoxLayout;

namespace fv {

class ElidingLabel;
class RelationPicker;

// The row states a queued file goes through, as the row's "state" property carries them.
inline constexpr const char* kRowReading = "reading";
inline constexpr const char* kRowPending = "pending";
inline constexpr const char* kRowActive = "active";
inline constexpr const char* kRowDone = "done";
inline constexpr const char* kRowFailed = "failed";
inline constexpr const char* kRowStopped = "stopped";

// _ROW_STATE_TEXT: "Reading…", "Queued", "Working", "Done", "Failed", "Stopped"; an unknown
// state is its own name.
QString rowStateText(const QString& state);

// QueueItem: one file waiting on the Upload File tab. Affinity: value type.
struct QueueItem {
    QString path;
    std::optional<audio::AudioFilePreview> preview;
    QString error;
    // reading | pending | active | done | failed | stopped
    QString state = QString::fromLatin1(kRowReading);
    // This file's own finished transcript, when the job produced one per file.
    QString transcript;

    // os.path.normcase(os.path.abspath(path)): two paths naming one file (on Windows, in any
    // case and with either slash) are one queue entry.
    QString key() const { return keyFor(path); }
    static QString keyFor(const QString& path);
    // os.path.basename(path), with Windows' separators.
    QString name() const;
};

// One file of a multi-file upload: name, facts, state, and its controls. Affinity: GUI thread.
class UploadQueueRow : public QFrame {
    Q_OBJECT
public:
    explicit UploadQueueRow(const QString& path, QWidget* parent = nullptr);

    const QString& path() const { return path_; }
    const QString& state() const { return state_; }
    const QString& transcript() const { return transcript_; }

    // Shows the copy button only while the file has text of its own.
    void setTranscript(const QString& text);
    // "Reading…" and "…" chips, the "reading" state.
    void setLoading();
    void setPreview(const audio::AudioFilePreview& preview);
    // "Could not read", the message as the tooltip, the "failed" state.
    void setError(const QString& message);
    void setState(const QString& state);
    // Move up is off for the first row and move down for the last.
    void setPosition(int index, int count);
    // A locked row (a job is running) hides move and remove.
    void setLocked(bool locked);

    ElidingLabel* nameLabel() const { return nameLabel_; }
    QLabel* sizeChip() const { return sizeChip_; }
    QLabel* durationChip() const { return durationChip_; }
    QLabel* chunkChip() const { return chunkChip_; }
    QLabel* stateChip() const { return stateChip_; }
    QPushButton* copyButton() const { return copyButton_; }
    QPushButton* upButton() const { return upButton_; }
    QPushButton* downButton() const { return downButton_; }
    QPushButton* removeButton() const { return removeButton_; }

Q_SIGNALS:
    void removeClicked(const QString& path);
    void moveUpClicked(const QString& path);
    void moveDownClicked(const QString& path);
    void copyClicked(const QString& path);

private:
    QString path_;
    QString state_;
    QString transcript_;
    QLabel* iconLabel_;
    ElidingLabel* nameLabel_;
    QLabel* sizeChip_;
    QLabel* durationChip_;
    QLabel* chunkChip_;
    QLabel* stateChip_;
    QPushButton* copyButton_;
    QPushButton* upButton_;
    QPushButton* downButton_;
    QPushButton* removeButton_;
};

// The rows of a multi-file upload plus the controls that act on the set. Rows are keyed by path
// and rebuilt from the tab's model, so reordering and previews that arrive out of order need no
// index bookkeeping here. Affinity: GUI thread.
class UploadQueueWidget : public QWidget {
    Q_OBJECT
public:
    explicit UploadQueueWidget(QWidget* parent = nullptr);

    // Rebuilds the rows only when the order of keys changed, then writes each item's facts,
    // state and transcript onto its row, and the title ("1 file", "N files").
    void setItems(const QList<QueueItem>& items);
    // Sets one row's state, or nothing when the queue holds no such path.
    void setRowState(const QString& path, const QString& state);
    void setRowTranscript(const QString& path, const QString& text);
    // What the set still needs or what the last drop left out, beside the count: "·  {text}",
    // warn-toned for a problem. Empty hides it.
    void setNote(const QString& text, bool warn = false);
    // Locks Add files, Clear all, the relation picker and every row's move and remove.
    void setLocked(bool locked);

    UploadQueueRow* rowFor(const QString& path) const;
    QList<UploadQueueRow*> rows() const;

    RelationPicker* picker() const { return picker_; }
    QLabel* titleLabel() const { return titleLabel_; }
    ElidingLabel* noteLabel() const { return noteLabel_; }
    QPushButton* addButton() const { return addButton_; }
    QPushButton* clearButton() const { return clearButton_; }

Q_SIGNALS:
    void addRequested();
    void clearRequested();
    void removeRequested(const QString& path);
    // delta is -1 (up) or +1 (down).
    void moveRequested(const QString& path, int delta);
    // A row's copy button: that file's own transcript is wanted.
    void rowCopyRequested(const QString& path);

private:
    void rebuild(const QList<QueueItem>& items);

    QLabel* titleLabel_;
    ElidingLabel* noteLabel_;
    QPushButton* addButton_;
    QPushButton* clearButton_;
    QVBoxLayout* rowsLayout_;
    RelationPicker* picker_;
    QHash<QString, UploadQueueRow*> rows_;
    QStringList order_;
};

} // namespace fv
