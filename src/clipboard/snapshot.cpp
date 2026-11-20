#include "clipboard/snapshot.h"

#include <QMetaType>
#include <QMimeData>
#include <QPixmap>
#include <QVariant>

#include "appkit/qt_bridge.h"
#include "pyport/text.h"

namespace fv::clipboard {

ClipboardSnapshot ClipboardSnapshot::capture(const QMimeData* mime_data) {
    ClipboardSnapshot snapshot;
    if (mime_data == nullptr) {
        return snapshot;
    }
    const QStringList formats = mime_data->formats();
    snapshot.formats.reserve(static_cast<std::size_t>(formats.size()));
    for (const QString& format : formats) {
        snapshot.formats.emplace_back(format, mime_data->data(format));
    }
    if (mime_data->hasText()) {
        snapshot.text = mime_data->text();
    }
    if (mime_data->hasHtml()) {
        snapshot.html = mime_data->html();
    }
    if (mime_data->hasUrls()) {
        snapshot.urls = mime_data->urls();
    }
    if (mime_data->hasImage()) {
        const QVariant image_data = mime_data->imageData();
        if (image_data.metaType() == QMetaType::fromType<QImage>()) {
            snapshot.image = image_data.value<QImage>().copy();
        } else if (image_data.metaType() == QMetaType::fromType<QPixmap>()) {
            snapshot.image = image_data.value<QPixmap>().toImage().copy();
        }
    }
    if (mime_data->hasColor()) {
        const QVariant color_data = mime_data->colorData();
        if (color_data.metaType() == QMetaType::fromType<QColor>()) {
            snapshot.color = color_data.value<QColor>();
        }
    }
    return snapshot;
}

bool ClipboardSnapshot::is_blank() const {
    for (const auto& [format, payload] : formats) {
        if (!format.toLower().startsWith(QStringLiteral("text/plain"))) {
            return false;
        }
    }
    return py::strip(qt::toUtf8(text.value_or(QString()))).empty();
}

std::unique_ptr<QMimeData> ClipboardSnapshot::to_mime_data() const {
    auto mime_data = std::make_unique<QMimeData>();
    for (const auto& [format, payload] : formats) {
        mime_data->setData(format, payload);
    }
    if (text) {
        mime_data->setText(*text);
    }
    if (html) {
        mime_data->setHtml(*html);
    }
    if (urls) {
        mime_data->setUrls(*urls);
    }
    if (image) {
        mime_data->setImageData(image->copy());
    }
    if (color) {
        mime_data->setColorData(*color);
    }
    return mime_data;
}

} // namespace fv::clipboard
