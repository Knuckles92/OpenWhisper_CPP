#include "ui_theme/app_icon.h"

#include <QColor>
#include <QLinearGradient>
#include <QPainter>
#include <QPainterPath>
#include <QPen>
#include <QRectF>

#include <algorithm>

namespace fv::ui {
namespace {

// The theme accent gradient of theme.qss, fixed: the mark is the same in both themes.
const QColor kGradientStart(0x0a, 0x84, 0xff);
const QColor kGradientEnd(0x64, 0xd2, 0xff);

// Outlined microphone with cradle, stem and base, as the loading screen paints it; needs about
// 32 px to stay legible.
void drawMicDetailed(QPainter& painter, int size) {
    QPen pen(QColor(255, 255, 255), size * 0.055);
    pen.setCapStyle(Qt::RoundCap);
    painter.setPen(pen);
    painter.setBrush(Qt::NoBrush);

    const QRectF capsule(size * 0.37, size * 0.20, size * 0.26, size * 0.38);
    painter.drawRoundedRect(capsule, size * 0.13, size * 0.13);

    // Cradle: the lower half of an ellipse hugging the capsule.
    const QRectF cradle(size * 0.27, size * 0.30, size * 0.46, size * 0.42);
    painter.drawArc(cradle, 180 * 16, 180 * 16);

    // Stem and base; int() truncates as Python's does for these positive values.
    painter.drawLine(static_cast<int>(size * 0.50), static_cast<int>(size * 0.72),
                     static_cast<int>(size * 0.50), static_cast<int>(size * 0.81));
    painter.drawLine(static_cast<int>(size * 0.37), static_cast<int>(size * 0.81),
                     static_cast<int>(size * 0.63), static_cast<int>(size * 0.81));
}

// Solid microphone for 16-24 px: an outlined capsule closes up into a blob there and the stem
// and base vanish into single grey pixels, so the capsule is filled and enlarged and only the
// cradle, the silhouette that reads as a microphone, is kept.
void drawMicSimplified(QPainter& painter, int size) {
    const QColor white(255, 255, 255);
    painter.setPen(Qt::NoPen);
    painter.setBrush(white);
    const QRectF capsule(size * 0.36, size * 0.16, size * 0.28, size * 0.40);
    painter.drawRoundedRect(capsule, size * 0.14, size * 0.14);

    QPen pen(white, std::max(1.5, size * 0.10));
    pen.setCapStyle(Qt::RoundCap);
    painter.setPen(pen);
    painter.setBrush(Qt::NoBrush);
    const QRectF cradle(size * 0.22, size * 0.28, size * 0.56, size * 0.48);
    painter.drawArc(cradle, 180 * 16, 180 * 16);
}

} // namespace

QPixmap renderAppPixmap(int size) {
    QPixmap pixmap(size, size);
    pixmap.fill(Qt::transparent);

    QPainter painter(&pixmap);
    painter.setRenderHint(QPainter::Antialiasing);

    QLinearGradient gradient(0, 0, size, size);
    gradient.setColorAt(0, kGradientStart);
    gradient.setColorAt(1, kGradientEnd);

    QPainterPath background;
    background.addRoundedRect(QRectF(0, 0, size, size), size * 0.22, size * 0.22);
    painter.fillPath(background, gradient);

    if (size < 32) {
        drawMicSimplified(painter, size);
    } else {
        drawMicDetailed(painter, size);
    }
    painter.end();
    return pixmap;
}

QIcon appIcon() {
    // Never destroyed: icons outliving QApplication must not be released after it.
    static const QIcon* const icon = [] {
        auto* built = new QIcon();
        for (const int size : kAppIconSizes) {
            built->addPixmap(renderAppPixmap(size));
        }
        return built;
    }();
    return *icon;
}

} // namespace fv::ui
