#pragma once

#include <memory>

#include <QObject>
#include <QString>

class QClipboard;
class QMimeData;

namespace fv::clipboard {

// The clipboard calls TemporaryClipboard makes: QClipboard's, behind an interface so tests can
// make reads and writes fail (QClipboard itself never reports a failure). A call that fails
// throws; what() is the message the restore-failure status shows.
// Affinity: GUI thread.
class ClipboardBackend : public QObject {
    Q_OBJECT

public:
    using QObject::QObject;
    ~ClipboardBackend() override;

    // The current contents, owned by the backend and valid until the next write; null when the
    // clipboard has none.
    virtual const QMimeData* mimeData() = 0;
    virtual void setMimeData(std::unique_ptr<QMimeData> data) = 0;
    virtual void setText(const QString& text) = 0;

Q_SIGNALS:
    // The clipboard's contents changed (QClipboard::dataChanged).
    void dataChanged();
};

// QGuiApplication's clipboard, or another QClipboard, as a backend. Once the QClipboard is gone
// every call throws.
std::unique_ptr<ClipboardBackend> qt_clipboard_backend(QClipboard* clipboard);

} // namespace fv::clipboard
