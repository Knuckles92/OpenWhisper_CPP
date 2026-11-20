#include "ui_shell/loading_screen.h"

#include <cmath>
#include <numbers>

#include <QBrush>
#include <QCloseEvent>
#include <QFont>
#include <QGuiApplication>
#include <QLinearGradient>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPen>
#include <QRadialGradient>
#include <QRectF>
#include <QScreen>
#include <QTimer>

#include "appkit/qt_bridge.h"
#include "base/identity.h"
#include "base/log.h"
#include "ui_theme/palette.h"

namespace fv {
namespace {

constexpr log::Logger kLog{"ui_qt.loading_screen"};

// Python's int() of a non-negative float: truncation.
int truncated(double value) {
    return static_cast<int>(value);
}

} // namespace

LoadingScreen::LoadingScreen(QWidget* parent)
    : QWidget(parent), statusText_(QStringLiteral("Initializing...")),
      progressText_(QStringLiteral("Please wait...")) {
    setWindowFlags(Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint | Qt::Tool);
    setAttribute(Qt::WA_TranslucentBackground, true);
    setFixedSize(kWidth, kHeight);
    if (QScreen* screen = QGuiApplication::primaryScreen()) {
        const QRect area = screen->geometry();
        move(area.center().x() - width() / 2, area.center().y() - height() / 2);
    }

    const ui::Palette& palette = ui::currentPalette();
    isDark_ = palette.isDark();
    background_ = palette.color("splash-bg");
    border_ = palette.color("splash-border");
    accent_ = palette.color("accent");
    accentGlow_ = palette.color("accent-cyan");
    text_ = palette.color("text");
    subtext_ = palette.color("text-secondary");

    glowStartedAt_ = std::chrono::steady_clock::now();
    glowTimer_ = new QTimer(this);
    glowTimer_->setTimerType(Qt::PreciseTimer);
    glowTimer_->setInterval(kFrameMs);
    connect(glowTimer_, &QTimer::timeout, this, qOverload<>(&QWidget::update));
    glowTimer_->start();
}

LoadingScreen::~LoadingScreen() {
    stopAnimation();
}

void LoadingScreen::stopAnimation() {
    glowTimer_->stop();
}

double LoadingScreen::glowPhase() const {
    if (pinnedPhase_) {
        return *pinnedPhase_;
    }
    const std::chrono::duration<double> elapsed = std::chrono::steady_clock::now() - glowStartedAt_;
    return elapsed.count() * kGlowRadiansPerSecond;
}

void LoadingScreen::pinGlowPhase(std::optional<double> phase) {
    pinnedPhase_ = phase;
    update();
}

QColor LoadingScreen::accent(int alpha) const {
    QColor color(accent_);
    color.setAlpha(alpha);
    return color;
}

QColor LoadingScreen::glow(int alpha) const {
    QColor color(accentGlow_);
    color.setAlpha(alpha);
    return color;
}

// loading_screen.py:83-187, the classic branch.
void LoadingScreen::paintEvent(QPaintEvent* /*event*/) {
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);

    const QRect area = rect();
    const int w = area.width();
    const int h = area.height();

    QLinearGradient gradient(0, 0, 0, h);
    gradient.setColorAt(0, background_);
    gradient.setColorAt(1, isDark_ ? background_.darker(120) : background_.darker(104));

    QPainterPath path;
    path.addRoundedRect(QRectF(area), 16, 16);
    painter.fillPath(path, gradient);
    painter.setPen(QPen(border_, 1));
    painter.drawPath(path);

    const double centerX = w / 2.0;
    const double centerY = h / 2.0 - 20;
    const double phase = glowPhase();
    const double pulse = 0.5 + 0.5 * std::sin(phase);

    const double outerRadius = 58 + pulse * 14;
    QRadialGradient outer(centerX, centerY, outerRadius);
    outer.setColorAt(0, accent(truncated(55 + pulse * 45)));
    outer.setColorAt(0.45, accent(truncated(18 + pulse * 22)));
    outer.setColorAt(1, accent(0));
    painter.setPen(Qt::NoPen);
    painter.setBrush(QBrush(outer));
    painter.drawEllipse(
        QRectF(centerX - outerRadius, centerY - outerRadius, outerRadius * 2, outerRadius * 2));

    const double innerRadius = 28 + pulse * 6;
    QRadialGradient inner(centerX, centerY, innerRadius);
    inner.setColorAt(0, glow(truncated(70 + pulse * 70)));
    inner.setColorAt(0.55, accent(truncated(35 + pulse * 40)));
    inner.setColorAt(1, accent(0));
    painter.setBrush(QBrush(inner));
    painter.drawEllipse(
        QRectF(centerX - innerRadius, centerY - innerRadius, innerRadius * 2, innerRadius * 2));

    constexpr int dots = 5;
    constexpr double orbitRadius = 35;
    for (int i = 0; i < dots; ++i) {
        const double angle = i * (2 * std::numbers::pi / dots) - std::numbers::pi / 2;
        const double dotPulse = 0.5 + 0.5 * std::sin(phase + i * 0.7);
        const double dotX = centerX + std::cos(angle) * orbitRadius;
        const double dotY = centerY + std::sin(angle) * orbitRadius;
        const double dotSize = 5 + dotPulse * 2;
        QColor color(accent_);
        color.setAlpha(truncated(140 + dotPulse * 115));
        painter.setBrush(color);
        painter.drawEllipse(QRectF(dotX - dotSize / 2, dotY - dotSize / 2, dotSize, dotSize));
    }

    constexpr double micW = 16;
    constexpr double micH = 24;
    const QRectF mic(centerX - micW / 2, centerY - micH / 2, micW, micH);
    QColor micColor(text_);
    micColor.setAlpha(truncated(200 + pulse * 55));
    painter.setPen(QPen(micColor, 2));
    painter.setBrush(Qt::NoBrush);
    painter.drawRoundedRect(mic, 8, 8);
    painter.drawLine(truncated(centerX), truncated(centerY + 12), truncated(centerX),
                     truncated(centerY + 18));
    painter.drawLine(truncated(centerX - 8), truncated(centerY + 18), truncated(centerX + 8),
                     truncated(centerY + 18));

    if (pulse > 0.35) {
        painter.setPen(Qt::NoPen);
        painter.setBrush(accent(truncated((pulse - 0.35) / 0.65 * 55)));
        painter.drawRoundedRect(mic.adjusted(2, 2, -2, -2), 6, 6);
    }

    painter.setPen(text_);
    painter.setFont(QFont(QStringLiteral("Segoe UI"), 16, QFont::Bold));
    painter.drawText(QRectF(0, h - 90, w, 30), Qt::AlignCenter, qt::toQString(identity().product));

    painter.setPen(accent_);
    painter.setFont(QFont(QStringLiteral("Segoe UI"), 10, QFont::DemiBold));
    painter.drawText(QRectF(0, h - 55, w, 20), Qt::AlignCenter, statusText_);

    painter.setPen(subtext_);
    painter.setFont(QFont(QStringLiteral("Segoe UI"), 9));
    painter.drawText(QRectF(0, h - 35, w, 20), Qt::AlignCenter, progressText_);
}

void LoadingScreen::mousePressEvent(QMouseEvent* event) {
    if (event->button() == Qt::LeftButton) {
        dragPosition_ = event->globalPosition().toPoint() - frameGeometry().topLeft();
        event->accept();
        return;
    }
    QWidget::mousePressEvent(event);
}

void LoadingScreen::mouseMoveEvent(QMouseEvent* event) {
    // A grab exactly at the top-left corner is a null QPoint, which Python's truth test read as
    // no drag; kept so the two behave alike.
    if (event->buttons() == Qt::LeftButton && dragPosition_ && !dragPosition_->isNull()) {
        move(event->globalPosition().toPoint() - *dragPosition_);
        event->accept();
        return;
    }
    QWidget::mouseMoveEvent(event);
}

void LoadingScreen::mouseReleaseEvent(QMouseEvent* event) {
    if (event->button() == Qt::LeftButton) {
        dragPosition_.reset();
        event->accept();
        return;
    }
    QWidget::mouseReleaseEvent(event);
}

void LoadingScreen::updateStatus(const QString& status) {
    statusText_ = status;
    update();
}

void LoadingScreen::updateProgress(const QString& progress) {
    progressText_ = progress;
    update();
}

void LoadingScreen::closeEvent(QCloseEvent* event) {
    stopAnimation();
    event->accept();
    kLog.info("Loading screen closed");
}

} // namespace fv
