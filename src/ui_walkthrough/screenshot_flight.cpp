#include "ui_walkthrough/screenshot_flight.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <numbers>

#include <QPainter>
#include <QPainterPath>
#include <QPen>

#include "ui_theme/palette.h"
#include "ui_walkthrough/keep_out.h"

namespace fv {

QRectF flightCardRect(double progress, const QRectF& source, const QRectF& target) {
    const double eased = 1.0 - std::pow(1.0 - progress, 3.0);
    const QPointF start = source.center();
    const QPointF end = target.center();
    QPointF centre = start + (end - start) * eased;
    centre.setY(centre.y() - 64 * std::sin(std::numbers::pi * eased));
    const double width = source.width() + (target.width() - source.width()) * eased;
    const double height = source.height() + (target.height() - source.height()) * eased;
    return QRectF(centre.x() - width / 2, centre.y() - height / 2, width, height);
}

ScreenshotFlight::ScreenshotFlight(QWidget* panel)
    : QWidget(panel, Qt::Tool | Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint |
                         Qt::WindowDoesNotAcceptFocus | Qt::WindowTransparentForInput) {
    setAttribute(Qt::WA_TranslucentBackground);
    setAttribute(Qt::WA_ShowWithoutActivating);
    setAttribute(Qt::WA_TransparentForMouseEvents);
    setAttribute(Qt::WA_QuitOnClose, false);
}

void ScreenshotFlight::start(int number, const QImage& image, const QRectF& source,
                             const QRectF& target) {
    number_ = number;
    image_ = image;
    source_ = source;
    progress_ = 0.0;
    advance(0.0, target);
    setKeepOutOfScreenshots(this, true);
    show();
    raise();
}

void ScreenshotFlight::land() {
    hide();
    number_ = 0;
    image_ = QImage();
}

void ScreenshotFlight::advance(double progress, const QRectF& target) {
    progress_ = std::min(1.0, std::max(0.0, progress));
    target_ = target;
    // Include the lifted arc, rotation and shadow, even across monitors.
    const QRectF bounds = source_.united(target).adjusted(-64, -128, 64, 64);
    setGeometry(bounds.toAlignedRect());
    update();
}

QRectF ScreenshotFlight::cardRect() const {
    return flightCardRect(progress_, source_, target_);
}

void ScreenshotFlight::paintEvent(QPaintEvent*) {
    if (image_.isNull()) {
        return;
    }
    QPainter painter(this);
    painter.setRenderHints(QPainter::Antialiasing | QPainter::SmoothPixmapTransform);
    const QRectF rect = cardRect().translated(-QPointF(pos()));
    const double lift = std::sin(std::numbers::pi * progress_);
    painter.translate(rect.center());
    painter.rotate(-3.5 * lift);
    painter.translate(-rect.center());
    painter.setPen(Qt::NoPen);
    for (const double spread : std::array{18.0, 12.0, 6.0}) {
        painter.setBrush(QColor(0, 0, 0, static_cast<int>(10 + 12 * lift)));
        painter.drawRoundedRect(
            rect.adjusted(-spread / 2, -spread / 2 + 5, spread / 2, spread / 2 + 5), 12, 12);
    }
    QPainterPath clip;
    clip.addRoundedRect(rect, 6 + 6 * lift, 6 + 6 * lift);
    painter.save();
    painter.setClipPath(clip);
    painter.drawImage(rect, image_);
    painter.restore();
    painter.setBrush(Qt::NoBrush);
    painter.setPen(QPen(ui::tokenColor("accent", static_cast<int>(190 * lift)), 1.5));
    painter.drawPath(clip);
}

} // namespace fv
