#pragma once

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <QByteArray>
#include <QColor>
#include <QImage>
#include <QList>
#include <QString>
#include <QUrl>

class QMimeData;

namespace fv::clipboard {

// A deep copy of the Qt-visible clipboard payload: every format's bytes, plus the text, rich
// text, URLs, image and colour Qt decodes from them.
// Affinity: value type (create and rebuild on the GUI thread: images need QtGui).
struct ClipboardSnapshot {
    std::vector<std::pair<QString, QByteArray>> formats;
    std::optional<QString> text;
    std::optional<QString> html;
    std::optional<QList<QUrl>> urls;
    std::optional<QImage> image;
    std::optional<QColor> color;

    // An empty snapshot for a null mime_data.
    static ClipboardSnapshot capture(const QMimeData* mime_data);

    // Only text/plain formats, holding nothing but whitespace (str.strip): not worth restoring.
    bool is_blank() const;
    std::unique_ptr<QMimeData> to_mime_data() const;
};

// A snapshot taken while recording, valid while the clipboard's counter has not moved.
// Affinity: value type.
struct PrefetchedSnapshot {
    std::shared_ptr<const ClipboardSnapshot> snapshot;
    // The clipboard sequence number read just before the capture began.
    std::uint64_t sequence = 0;
};

// One staged paste: the token written under the marker format, and the user's clipboard it
// stands in for.
// Affinity: value type.
struct StagedLease {
    QByteArray token;
    std::shared_ptr<const ClipboardSnapshot> snapshot;
    // Its restore failed: kept when later writes replace the marker, because the marker can come
    // back (a clipboard history pasting the transcript back) and OpenWhisper's lease, which
    // carries its own snapshot and token, restores then.
    bool restore_failed = false;
};

// One synthetic copy in flight (TemporaryClipboard::capture_selection).
// Affinity: GUI thread.
struct SelectionCapture {
    std::shared_ptr<const ClipboardSnapshot> original;
    std::function<void(std::optional<std::string>)> callback;
    std::chrono::steady_clock::time_point deadline;
    // Sequence number before the copy; nullopt without a sequence source.
    std::optional<std::uint64_t> sequence;
    // The last sequence number seen after a change, to wait until it settles.
    std::optional<std::uint64_t> settling;
    // The clipboard changed since the copy was sent.
    bool changed = false;
    // The copy shortcut went out, so the clipboard may hold the selection.
    bool sent = false;
    int polls = 0;
};

} // namespace fv::clipboard
