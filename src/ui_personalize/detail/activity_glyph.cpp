#include "ui_personalize/detail/activity_glyph.h"

#include <cmath>

#include <QBrush>
#include <QConicalGradient>
#include <QPainter>
#include <QPen>
#include <QPointF>
#include <QRectF>

#include "pyport/number.h"
#include "ui_theme/palette.h"

namespace fv::detail {

ActivityGlyph::ActivityGlyph(QWidget* parent, const Clock* clock) : QWidget(parent), clock_(clock) {
    setFixedSize(kSize, kSize);
    setAttribute(Qt::WA_TransparentForMouseEvents, true);
    clock_.start();
}

void ActivityGlyph::setState(RuleActivityState state) {
    if (state != state_) {
        state_ = state;
        clock_.restart();
    }
    update();
}

void ActivityGlyph::paintEvent(QPaintEvent*) {
    if (state_ == RuleActivityState::Idle) {
        return;
    }
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setPen(Qt::NoPen);
    const QPointF center = QRectF(rect()).center();
    const std::int64_t elapsed = clock_.elapsed();
    if (state_ == RuleActivityState::Listening) {
        const double phase = static_cast<double>(elapsed % kPulseMs) / kPulseMs;
        const double radius = 3.5 + 5.0 * (1.0 - std::pow(1.0 - phase, 3));
        painter.setBrush(
            ui::tokenColor("danger", static_cast<int>(py::round_half_even(120 * (1.0 - phase)))));
        painter.drawEllipse(center, radius, radius);
        painter.setBrush(ui::tokenColor("danger"));
        painter.drawEllipse(center, 3.5, 3.5);
        return;
    }

    const double phase = static_cast<double>(elapsed) / kOrbitMs;
    const QRectF ring = QRectF(rect()).adjusted(2.5, 2.5, -2.5, -2.5);
    painter.setBrush(Qt::NoBrush);
    painter.setPen(QPen(ui::tokenColor("accent", 40), 2));
    painter.drawEllipse(ring);
    QConicalGradient tail(center, -phase * 360);
    tail.setColorAt(0, ui::tokenColor("accent-cyan"));
    tail.setColorAt(0.18, ui::tokenColor("accent"));
    tail.setColorAt(0.75, ui::tokenColor("accent", 0));
    tail.setColorAt(1, ui::tokenColor("accent", 0));
    painter.setPen(QPen(QBrush(tail), 2));
    painter.drawEllipse(ring);
}

} // namespace fv::detail
