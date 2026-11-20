#include "ui_meeting/live_dot.h"

#include <cmath>
#include <numbers>

#include <QPainter>
#include <QPointF>
#include <QTimer>

#include "pyport/number.h"
#include "ui_theme/palette.h"

namespace fv {

LiveDot::LiveDot(QWidget* parent) : QWidget(parent), timer_(new QTimer(this)) {
    setFixedSize(22, 22);
    timer_->setInterval(kTickMs);
    connect(timer_, &QTimer::timeout, this, &LiveDot::tick);
}

void LiveDot::setLive(bool live) {
    live_ = live;
    sync();
}

void LiveDot::sync() {
    if (live_ && isVisible()) {
        timer_->start();
    } else {
        timer_->stop();
        phase_ = 0.0;
    }
    update();
}

void LiveDot::showEvent(QShowEvent* event) {
    QWidget::showEvent(event);
    sync();
}

void LiveDot::hideEvent(QHideEvent* event) {
    QWidget::hideEvent(event);
    sync();
}

void LiveDot::tick() {
    phase_ = std::fmod(phase_ + kPhaseStep, 1.0);
    update();
}

void LiveDot::paintEvent(QPaintEvent* /*event*/) {
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setPen(Qt::NoPen);
    const QPointF centre(width() / 2.0, height() / 2.0);
    if (live_) {
        const double swell = 0.5 - 0.5 * std::cos(phase_ * 2.0 * std::numbers::pi);
        // round(), half to even, as the halo's alpha in Python.
        const int alpha = static_cast<int>(py::round_half_even(40.0 + 50.0 * (1.0 - swell)));
        painter.setBrush(ui::tokenColor("danger", alpha));
        const double radius = 7.0 + 4.0 * swell;
        painter.drawEllipse(centre, radius, radius);
        painter.setBrush(ui::tokenColor("danger"));
    } else {
        painter.setBrush(ui::tokenColor("text-muted"));
    }
    painter.drawEllipse(centre, 5.0, 5.0);
}

} // namespace fv
