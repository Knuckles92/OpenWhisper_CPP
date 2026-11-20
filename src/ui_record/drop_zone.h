#pragma once

#include <QFrame>
#include <QString>
#include <QStringList>

class QMimeData;

namespace fv {

class WrappedLabel;

// SUPPORTED_EXTENSIONS, lower case with the dot.
inline constexpr const char* kSupportedAudioExtensions[] = {".wav", ".mp3",  ".m4a",
                                                            ".ogg", ".flac", ".wma"};
// AUDIO_FILTERS: the file dialog's filter string.
QString audioFileFilters();
// NO_AUDIO_DROPPED.
QString noAudioDroppedMessage();

// _is_supported_audio: the path ends in a supported extension, in any case.
bool isSupportedAudio(const QString& path);

// What a drop carried: local audio paths in drop order, and how many dropped URLs were not.
struct DroppedAudio {
    QStringList paths;
    int skipped = 0;
};
// _audio_paths_from_mime: a URL that is not a local file counts as skipped, like a file that is
// not audio.
DroppedAudio audioPathsFromMime(const QMimeData* mime);

// Select Audio Files: the native file dialog for the drop zone and the queue's Add files. Never
// called by a test (it takes over the desktop); empty when cancelled.
QStringList browseForAudioFiles(QWidget* parent);

// Drag-and-drop zone that also asks for a file browser on click. It styles itself because the
// border and fill change with the drag state, and each variant carries the child rules too: a
// stylesheet set on a widget replaces, rather than layers over, the one it had before.
// Affinity: GUI thread.
class DropZoneWidget : public QFrame {
    Q_OBJECT
public:
    explicit DropZoneWidget(QWidget* parent = nullptr);

    // Why the last attempt landed back here (unreadable file, nothing droppable); the tab has no
    // status line of its own. Empty hides the notice.
    void setNotice(const QString& text);
    WrappedLabel* noticeLabel() const { return notice_; }
    bool isValidAudio(const QString& path) const { return isSupportedAudio(path); }

Q_SIGNALS:
    // The audio paths in drop order and how many dropped items were not audio.
    void filesSelected(const QStringList& paths, int skipped);
    // A left click: the owner opens the file browser (browseForAudioFiles) and reports what the
    // user chose through filesSelected.
    void browseRequested();

protected:
    void dragEnterEvent(QDragEnterEvent* event) override;
    void dragLeaveEvent(QDragLeaveEvent* event) override;
    void dropEvent(QDropEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;

private:
    WrappedLabel* notice_;
};

} // namespace fv
