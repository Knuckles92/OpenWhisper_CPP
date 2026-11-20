#include "ui_remote/host_beacon.h"

#include <algorithm>
#include <cmath>
#include <numbers>

#include <QPainter>
#include <QPen>

#include "ui_theme/palette.h"

namespace fv {

namespace {

constexpr qint64 kWavePeriodMs = 1300;
constexpr qint64 kRippleMs = 900;
constexpr qint64 kLightMs = 520;

} // namespace

HostBeacon::HostBeacon(QWidget* parent) : QWidget(parent) {
    setObjectName(QStringLiteral("hostBeacon"));
    setFixedSize(kSize, kSize);
    setAttribute(Qt::WA_TranslucentBackground);
    clock_.start();
    timer_.setInterval(33);
    connect(&timer_, &QTimer::timeout, this, &HostBeacon::tick);
}

void HostBeacon::setState(const QString& state) {
    if (state == QStringLiteral("on") && state_ != QStringLiteral("on") && isVisible()) {
        // Sharing just came on here: the light comes on with a ring.
        litAt_ = clock_.elapsed();
    }
    if (state != QStringLiteral("on")) {
        litAt_.reset();
        ripples_.clear();
        busy_ = false;
    }
    state_ = state;
    sync();
}

void HostBeacon::setBusy(bool busy) {
    busy = busy && state_ == QStringLiteral("on");
    if (busy && !busy_) {
        busySince_ = clock_.elapsed();
    }
    busy_ = busy;
    sync();
}

void HostBeacon::ripple() {
    if (state_ == QStringLiteral("on") && isVisible()) {
        ripples_.push_back(clock_.elapsed());
        sync();
    }
}

bool HostBeacon::animating() const {
    return busy_ || !ripples_.empty() || litAt_.has_value();
}

void HostBeacon::sync() {
    if (animating() && isVisible()) {
        if (!timer_.isActive()) {
            timer_.start();
        }
    } else {
        timer_.stop();
    }
    update();
}

void HostBeacon::tick() {
    const qint64 now = clock_.elapsed();
    std::erase_if(ripples_, [now](qint64 start) { return now - start >= kRippleMs; });
    if (litAt_ && now - *litAt_ >= kLightMs) {
        litAt_.reset();
    }
    sync();
}

void HostBeacon::showEvent(QShowEvent* event) {
    QWidget::showEvent(event);
    sync();
}

void HostBeacon::hideEvent(QHideEvent* event) {
    QWidget::hideEvent(event);
    timer_.stop();
}

QColor HostBeacon::lightColor() const {
    if (state_ == QStringLiteral("on")) {
        return ui::tokenColor(busy_ ? "accent-cyan" : "success");
    }
    if (state_ == QStringLiteral("error")) {
        return ui::tokenColor("warning");
    }
    if (state_ == QStringLiteral("starting")) {
        return ui::tokenColor("accent");
    }
    return ui::tokenColor("text-muted");
}

void HostBeacon::paintEvent(QPaintEvent*) {
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    const qint64 now = clock_.elapsed();
    const QPointF center(width() / 2.0, height() / 2.0);
    const bool on = state_ == QStringLiteral("on");

    // A soft halo behind a sharing host.
    if (on) {
        painter.setPen(Qt::NoPen);
        painter.setBrush(ui::tokenColor("success", 34));
        painter.drawEllipse(center, 27, 27);
    }

    for (const qint64 start : ripples_) {
        const double progress = std::min(1.0, static_cast<double>(now - start) / kRippleMs);
        painter.setPen(
            QPen(ui::tokenColor("success", static_cast<int>(210 * (1 - progress))), 2.0));
        painter.setBrush(Qt::NoBrush);
        const double radius = 18 + 12 * (1 - (1 - progress) * (1 - progress));
        painter.drawEllipse(center, radius, radius);
    }

    if (busy_) {
        // Waves out to either side while audio is being decoded.
        const double elapsed = static_cast<double>(now - busySince_);
        for (int index = 0; index < 3; ++index) {
            const double phase = std::fmod(elapsed / kWavePeriodMs + index / 3.0, 1.0);
            const double radius = 10 + 18 * phase;
            QPen pen(ui::tokenColor("accent-cyan",
                                    static_cast<int>(230 * std::sin(phase * std::numbers::pi))),
                     1.8);
            pen.setCapStyle(Qt::RoundCap);
            painter.setPen(pen);
            painter.setBrush(Qt::NoBrush);
            const QRectF rect(center.x() - radius, center.y() - radius, 2 * radius, 2 * radius);
            painter.drawArc(rect, -38 * 16, 76 * 16);
            painter.drawArc(rect, 142 * 16, 76 * 16);
        }
    }

    // The server: two rack units.
    painter.setPen(QPen(ui::tokenColor(on ? "text-soft" : "text-secondary"), 1.6));
    painter.setBrush(ui::tokenColor("surface-hover"));
    const QRectF top(center.x() - 13, center.y() - 12, 26, 10.5);
    const QRectF bottom(center.x() - 13, center.y() + 1.5, 26, 10.5);
    painter.drawRoundedRect(top, 3, 3);
    painter.drawRoundedRect(bottom, 3, 3);
    QPen slot(ui::tokenColor("text-muted"), 1.4);
    slot.setCapStyle(Qt::RoundCap);
    painter.setPen(slot);
    for (const QRectF& rect : {top, bottom}) {
        const double y = rect.center().y();
        painter.drawLine(QPointF(rect.left() + 5, y), QPointF(rect.left() + 12, y));
    }

    // The status light on each unit; the top one carries the state.
    painter.setPen(Qt::NoPen);
    const QPointF topLight(top.right() - 5.5, top.center().y());
    if (litAt_) {
        const double progress = std::min(1.0, static_cast<double>(now - *litAt_) / kLightMs);
        painter.setPen(
            QPen(ui::tokenColor("success", static_cast<int>(220 * (1 - progress))), 1.4));
        painter.setBrush(Qt::NoBrush);
        painter.drawEllipse(topLight, 2 + 6 * progress, 2 + 6 * progress);
        painter.setPen(Qt::NoPen);
    }
    painter.setBrush(lightColor());
    painter.drawEllipse(topLight, 2.2, 2.2);
    painter.setBrush(on ? ui::tokenColor("success", 150) : ui::tokenColor("text-faint"));
    painter.drawEllipse(QPointF(bottom.right() - 5.5, bottom.center().y()), 2.0, 2.0);
}

} // namespace fv
