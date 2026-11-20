#include "ui_widgets/animated_progress_bar.h"

#include <algorithm>
#include <cmath>

#include <QColor>
#include <QLinearGradient>
#include <QPainter>
#include <QPainterPath>
#include <QSizePolicy>
#include <QTimer>

#include "ui_theme/theme.h"

namespace fv {
namespace {

constexpr double kSweepMs = 1500.0;
constexpr double kSheenMs = 1900.0;
constexpr double kEase = 0.18;

// The sheen is a white highlight travelling over the blue fill, so it stays white in both
// themes; the track and the fill follow the palette.
QColor sheenColor() {
    return QColor(255, 255, 255, 70);
}

QColor withAlpha(QColor colour, int alpha) {
    colour.setAlpha(alpha);
    return colour;
}

} // namespace

AnimatedProgressBar::AnimatedProgressBar(QWidget* parent, int barHeight) : QWidget(parent) {
    setObjectName(QStringLiteral("animatedProgressBar"));
    setFixedHeight(barHeight);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    setAttribute(Qt::WA_TransparentForMouseEvents, true);

    timer_ = new QTimer(this);
    timer_->setInterval(kTickMs);
    connect(timer_, &QTimer::timeout, this, &AnimatedProgressBar::tick);
}

void AnimatedProgressBar::setIndeterminate() {
    if (!indeterminate_) {
        indeterminate_ = true;
        phase_ = 0.0;
    }
    update();
}

void AnimatedProgressBar::setFraction(double fraction, bool animate) {
    const double value = std::max(0.0, std::min(1.0, fraction));
    if (indeterminate_) {
        indeterminate_ = false;
        phase_ = 0.0;
        display_ = animate ? 0.0 : value;
    }
    target_ = value;
    if (!animate) {
        display_ = value;
    }
    update();
}

void AnimatedProgressBar::reset() {
    indeterminate_ = false;
    target_ = 0.0;
    display_ = 0.0;
    phase_ = 0.0;
    update();
}

void AnimatedProgressBar::showEvent(QShowEvent* event) {
    QWidget::showEvent(event);
    timer_->start();
}

void AnimatedProgressBar::hideEvent(QHideEvent* event) {
    QWidget::hideEvent(event);
    timer_->stop();
}

void AnimatedProgressBar::tick() {
    const double period = indeterminate_ ? kSweepMs : kSheenMs;
    phase_ = std::fmod(phase_ + kTickMs / period, 1.0);
    if (!indeterminate_) {
        const double delta = target_ - display_;
        if (std::abs(delta) < 0.001) {
            display_ = target_;
        } else {
            display_ += delta * kEase;
        }
    }
    update();
}

void AnimatedProgressBar::paintEvent(QPaintEvent*) {
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setPen(Qt::NoPen);

    const QRectF rect(this->rect());
    if (rect.width() <= 0 || rect.height() <= 0) {
        return;
    }
    const double radius = rect.height() / 2.0;

    QPainterPath track;
    track.addRoundedRect(rect, radius, radius);
    painter.fillPath(track, withAlpha(ui::currentTheme().overlayRgb, 22));
    painter.setClipPath(track);

    if (indeterminate_) {
        paintSweep(painter, rect);
    } else {
        paintFill(painter, rect, radius);
    }
}

void AnimatedProgressBar::paintSweep(QPainter& painter, const QRectF& rect) const {
    const ui::Theme& theme = ui::currentTheme();
    const double span = std::max(rect.width() * 0.34, 56.0);
    const double eased = phase_ * phase_ * (3.0 - 2.0 * phase_);
    const double left = -span + (rect.width() + span) * eased;

    QLinearGradient gradient(left, 0.0, left + span, 0.0);
    gradient.setColorAt(0.0, withAlpha(theme.accent, 0));
    gradient.setColorAt(0.45, theme.accent);
    gradient.setColorAt(0.75, theme.accentCyan);
    gradient.setColorAt(1.0, withAlpha(theme.accentCyan, 0));
    painter.fillRect(QRectF(left, rect.top(), span, rect.height()), gradient);
}

void AnimatedProgressBar::paintFill(QPainter& painter, const QRectF& rect, double radius) const {
    if (display_ <= 0.0) {
        return;
    }
    const ui::Theme& theme = ui::currentTheme();
    const double width = std::max(rect.width() * display_, radius * 2.0);
    const QRectF filled(rect.left(), rect.top(), width, rect.height());

    QLinearGradient gradient(rect.left(), 0.0, rect.right(), 0.0);
    gradient.setColorAt(0.0, theme.accent);
    gradient.setColorAt(1.0, theme.accentCyan);

    QPainterPath path;
    path.addRoundedRect(filled, radius, radius);
    painter.fillPath(path, gradient);

    if (display_ < 0.999) {
        painter.save();
        painter.setClipPath(path, Qt::IntersectClip);
        const double band = std::max(width * 0.28, 40.0);
        const double left = -band + (width + band) * phase_;
        QLinearGradient sheen(left, 0.0, left + band, 0.0);
        sheen.setColorAt(0.0, QColor(255, 255, 255, 0));
        sheen.setColorAt(0.5, sheenColor());
        sheen.setColorAt(1.0, QColor(255, 255, 255, 0));
        painter.fillRect(QRectF(left, rect.top(), band, rect.height()), sheen);
        painter.restore();
    }
}

} // namespace fv
