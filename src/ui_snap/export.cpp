#include "ui_snap/export.h"

#include <QPainter>
#include <QPoint>
#include <QRectF>

#include "ui_snap/geometry.h"

namespace fv::ui_snap {

QRect logicalRect(const QImage& image) {
    return QRect(QPoint(0, 0), image.deviceIndependentSize().toSize());
}

QImage renderKept(const QImage& frozen, const QRect& selection, std::span<const Annotation> items) {
    const double ratio = frozen.devicePixelRatio();
    const double dpr = ratio != 0.0 ? ratio : 1.0;
    const QRect crop = deviceRect(selection, dpr, frozen.rect());
    QImage image = frozen.copy(crop);
    image.setDevicePixelRatio(1.0);
    if (!items.empty()) {
        QPainter painter(&image);
        painter.setRenderHint(QPainter::Antialiasing);
        painter.setRenderHint(QPainter::TextAntialiasing);
        // Device pixels of the whole screen, then logical pixels: the same space the overlay
        // drew the marks in.
        painter.translate(-crop.x(), -crop.y());
        painter.scale(dpr, dpr);
        painter.setClipRect(QRectF(selection));
        for (const Annotation& item : items) {
            paintAnnotation(painter, item);
        }
    }
    image.setDevicePixelRatio(dpr);
    return image;
}

} // namespace fv::ui_snap
