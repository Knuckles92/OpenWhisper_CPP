#include "ui_widgets/agent_mark.h"

#include <algorithm>
#include <cmath>

#include <QEvent>
#include <QFont>
#include <QFontMetricsF>
#include <QPainter>
#include <QPainterPath>
#include <QPen>
#include <QRectF>

#include "pyport/number.h"
#include "ui_theme/palette.h"

namespace fv {
namespace agent_paint {

double clamp(double value) {
    return std::max(0.0, std::min(1.0, value));
}

double easeOut(double t) {
    t = clamp(t);
    return 1.0 - std::pow(1.0 - t, 3);
}

double easeInOut(double t) {
    t = clamp(t);
    return t < 0.5 ? 4.0 * std::pow(t, 3) : 1.0 - std::pow(-2.0 * t + 2.0, 3) / 2.0;
}

QColor mix(const QColor& start, const QColor& end, double t) {
    t = clamp(t);
    const auto channel = [t](int from, int to) {
        return static_cast<int>(py::round_half_even(from + (to - from) * t));
    };
    return QColor(channel(start.red(), end.red()), channel(start.green(), end.green()),
                  channel(start.blue(), end.blue()), channel(start.alpha(), end.alpha()));
}

QColor alpha(const QColor& color, double alpha) {
    QColor copy(color);
    copy.setAlphaF(static_cast<float>(clamp(alpha)));
    return copy;
}

bool dark() {
    return ui::currentPalette().isDark();
}

QColor ringColor(const QColor& accent) {
    return dark() ? accent.lighter(118) : accent.darker(112);
}

} // namespace agent_paint

AgentMark::AgentMark(const QString& monogram, const QColor& accent, const QPixmap& pixmap,
                     QWidget* parent)
    : QWidget(parent), monogram_(monogram), accent_(accent), pixmap_(pixmap) {
    setObjectName(QStringLiteral("agentMark"));
    setAttribute(Qt::WA_TransparentForMouseEvents, true);
    setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
}

void AgentMark::setMonogram(const QString& monogram) {
    if (monogram != monogram_) {
        monogram_ = monogram;
        update();
    }
}

void AgentMark::setWarmth(double value) {
    value = agent_paint::clamp(value);
    if (value != warmth_) {
        warmth_ = value;
        update();
    }
}

void AgentMark::setMissing(double value) {
    value = agent_paint::clamp(value);
    if (value != missing_) {
        missing_ = value;
        update();
    }
}

int AgentMark::side() const {
    return std::max(24,
                    static_cast<int>(py::round_half_even(QFontMetricsF(font()).height() * 2.0)));
}

QSize AgentMark::sizeHint() const {
    const int length = side();
    return {length, length};
}

QSize AgentMark::minimumSizeHint() const {
    return sizeHint();
}

void AgentMark::changeEvent(QEvent* event) {
    QWidget::changeEvent(event);
    if (event->type() == QEvent::FontChange || event->type() == QEvent::StyleChange) {
        updateGeometry();
    }
}

void AgentMark::paintEvent(QPaintEvent*) {
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    const double length = static_cast<double>(std::min(width(), height()));
    const QRectF rect(0.5, 0.5, length - 1.0, length - 1.0);
    const double radius = length * 0.28;
    if (!pixmap_.isNull()) {
        QPainterPath path;
        path.addRoundedRect(rect, radius, radius);
        painter.setClipPath(path);
        painter.setRenderHint(QPainter::SmoothPixmapTransform);
        painter.drawPixmap(rect.toRect(), pixmap_);
        return;
    }
    painter.setPen(QPen(agent_paint::alpha(accent_, 0.40), 1.0));
    painter.setBrush(agent_paint::alpha(accent_, agent_paint::dark() ? 0.20 : 0.14));
    painter.drawRoundedRect(rect, radius, radius);
    QFont mono(font());
    mono.setPixelSize(std::max(9, static_cast<int>(py::round_half_even(length * 0.38))));
    mono.setWeight(QFont::Bold);
    painter.setFont(mono);
    painter.setPen(agent_paint::ringColor(accent_));
    painter.drawText(rect, Qt::AlignCenter, monogram_);
}

} // namespace fv
