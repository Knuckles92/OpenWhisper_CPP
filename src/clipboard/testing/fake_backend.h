#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include <QMimeData>
#include <QString>
#include <QThread>

#include "clipboard/backend.h"

namespace fv::clipboard {

// A clipboard stand-in, never the system one. It keeps its own QMimeData and a Windows-style
// change counter that every write bumps, whoever makes it, while reads never do. Tests make reads
// fail (raise_on_read), the n-th setMimeData fail (fail_set_mime_call), or every write fail
// (fail_writes); writes are recorded with their kind ("mime" or "text") and text.
// Affinity: GUI thread, except that the state fields may be set between calls.
class FakeClipboardBackend final : public ClipboardBackend {
public:
    struct Write {
        std::string kind;
        QString text;
        bool operator==(const Write&) const = default;
    };

    FakeClipboardBackend() : mime_(std::make_unique<QMimeData>()) {}
    explicit FakeClipboardBackend(const QString& text) : FakeClipboardBackend() {
        mime_->setText(text);
    }
    explicit FakeClipboardBackend(std::unique_ptr<QMimeData> mime) : mime_(std::move(mime)) {}

    const QMimeData* mimeData() override {
        read_threads.insert(QThread::currentThread());
        if (raise_on_read) {
            throw std::runtime_error("clipboard read failed");
        }
        return mime_.get();
    }

    void setMimeData(std::unique_ptr<QMimeData> data) override {
        ++set_mime_calls;
        if (fail_writes || (fail_set_mime_call && set_mime_calls == *fail_set_mime_call)) {
            throw std::runtime_error("clipboard write failed");
        }
        writes.push_back({"mime", data->text()});
        replace(std::move(data));
    }

    void setText(const QString& text) override {
        if (fail_writes) {
            throw std::runtime_error("clipboard write failed");
        }
        auto data = std::make_unique<QMimeData>();
        data->setText(text);
        writes.push_back({"text", text});
        replace(std::move(data));
    }

    // The counter as a sequence source reads it.
    std::optional<std::uint64_t> sequence() const { return counter; }

    // The current contents, without recording a read.
    QMimeData& current() { return *mime_; }
    QString text() const { return mime_->text(); }

    // What another app's copy does: empty the clipboard, then fill it, each a change, then the
    // change signal (unless signal is false, as Qt's Cocoa clipboard for a background app).
    void copy_from_app(const QString& text, bool signal = true) {
        mime_ = std::make_unique<QMimeData>();
        ++counter;
        auto data = std::make_unique<QMimeData>();
        data->setText(text);
        mime_ = std::move(data);
        ++counter;
        if (signal) {
            Q_EMIT dataChanged();
        }
    }

    // Another app's write of whole contents, one change.
    void set_from_app(std::unique_ptr<QMimeData> data, bool signal = true) {
        mime_ = std::move(data);
        ++counter;
        if (signal) {
            Q_EMIT dataChanged();
        }
    }

    bool raise_on_read = false;
    bool fail_writes = false;
    std::optional<int> fail_set_mime_call;
    // Whether this app's own writes signal dataChanged (QClipboard's do).
    bool signals_own_writes = true;
    int set_mime_calls = 0;
    std::uint64_t counter = 1;
    std::vector<Write> writes;
    std::set<QThread*> read_threads;

private:
    void replace(std::unique_ptr<QMimeData> data) {
        mime_ = std::move(data);
        ++counter;
        if (signals_own_writes) {
            Q_EMIT dataChanged();
        }
    }

    std::unique_ptr<QMimeData> mime_;
};

} // namespace fv::clipboard
