#include "clipboard/backend.h"

#include <stdexcept>

#include <QClipboard>
#include <QMimeData>
#include <QPointer>

namespace fv::clipboard {

ClipboardBackend::~ClipboardBackend() = default;

namespace {

class QtClipboardBackend final : public ClipboardBackend {
public:
    explicit QtClipboardBackend(QClipboard* clipboard) : clipboard_(clipboard) {
        connect(clipboard, &QClipboard::dataChanged, this, &ClipboardBackend::dataChanged);
    }

    const QMimeData* mimeData() override { return clipboard().mimeData(); }

    void setMimeData(std::unique_ptr<QMimeData> data) override {
        clipboard().setMimeData(data.release());
    }

    void setText(const QString& text) override { clipboard().setText(text); }

private:
    QClipboard& clipboard() {
        if (clipboard_.isNull()) {
            throw std::runtime_error("No Qt clipboard available");
        }
        return *clipboard_;
    }

    QPointer<QClipboard> clipboard_;
};

} // namespace

std::unique_ptr<ClipboardBackend> qt_clipboard_backend(QClipboard* clipboard) {
    if (clipboard == nullptr) {
        return nullptr;
    }
    return std::make_unique<QtClipboardBackend>(clipboard);
}

} // namespace fv::clipboard
