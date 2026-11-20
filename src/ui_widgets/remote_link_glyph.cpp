#include "ui_widgets/remote_link_glyph.h"

#include <algorithm>
#include <cmath>
#include <numbers>

#include <QMouseEvent>
#include <QPainter>
#include <QPen>

#include "ui_theme/palette.h"

namespace fv {
namespace {

constexpr double kWireStart = 16.0;
constexpr double kWireEnd = 30.0;
constexpr double kWireY = 8.0;

double ease(double x) {
    return x < 0.5 ? 2 * x * x : 1 - std::pow(-2 * x + 2, 2) / 2;
}

int alpha(double scale, double fraction) {
    return std::clamp(static_cast<int>(scale * fraction), 0, 255);
}

} // namespace

RemoteLinkGlyph::RemoteLinkGlyph(QWidget* parent, bool mirrored)
    : QWidget(parent), mirrored_(mirrored) {
    setObjectName(QStringLiteral("remoteLinkGlyph"));
    setFixedSize(kWidth, kHeight);
    setAttribute(Qt::WA_TranslucentBackground);
    elapsed_.start();
    timer_.setInterval(kFrameMs);
    connect(&timer_, &QTimer::timeout, this, &RemoteLinkGlyph::tick);
}

std::int64_t RemoteLinkGlyph::now() const {
    return clock_ ? clock_() : elapsed_.elapsed();
}

void RemoteLinkGlyph::setClock(std::function<std::int64_t()> clock) {
    clock_ = std::move(clock);
}

void RemoteLinkGlyph::setLink(const QString& state, bool busy, std::int64_t replies,
                              std::int64_t beat) {
    const std::int64_t at = now();
    const bool connected = state == QLatin1String("connected");
    if (connected && !greeted_) {
        // The first answer this glyph has seen: draw the wire across.
        greeted_ = true;
        drawnAt_ = at;
    } else if (connected && state_ == QLatin1String("connected")) {
        if (replies > replies_) {
            flights_.push_back({at, kReplyMs, QStringLiteral("reply")});
        }
        if (beat > beat_ && !busy) {
            flights_.push_back({at, kHeartbeatMs, QStringLiteral("beat")});
        }
    }
    if (busy && !busy_) {
        busySince_ = at;
    }
    if (!connected) {
        flights_.clear();
        drawnAt_.reset();
    }
    state_ = state;
    busy_ = busy && connected;
    replies_ = replies;
    beat_ = beat;
    setCursor(state == QLatin1String("offline") ? Qt::PointingHandCursor : Qt::ArrowCursor);
    syncAnimation();
    update();
}

QStringList RemoteLinkGlyph::flightKinds() const {
    QStringList kinds;
    for (const Flight& flight : flights_) {
        kinds << flight.kind;
    }
    return kinds;
}

bool RemoteLinkGlyph::animating() const {
    return state_ == QLatin1String("connecting") || busy_ || !flights_.empty() ||
           drawnAt_.has_value();
}

void RemoteLinkGlyph::syncAnimation() {
    if (animating() && isVisible()) {
        if (!timer_.isActive()) {
            timer_.start();
        }
    } else {
        timer_.stop();
    }
}

void RemoteLinkGlyph::tick() {
    const std::int64_t at = now();
    std::erase_if(flights_,
                  [at](const Flight& flight) { return at - flight.start >= flight.duration; });
    if (drawnAt_ && at - *drawnAt_ >= kDrawMs + kLightMs) {
        drawnAt_.reset();
    }
    syncAnimation();
    update();
}

void RemoteLinkGlyph::showEvent(QShowEvent* event) {
    QWidget::showEvent(event);
    syncAnimation();
}

void RemoteLinkGlyph::hideEvent(QHideEvent* event) {
    QWidget::hideEvent(event);
    timer_.stop();
}

void RemoteLinkGlyph::mouseReleaseEvent(QMouseEvent* event) {
    if (event->button() == Qt::LeftButton && rect().contains(event->position().toPoint())) {
        Q_EMIT clicked();
    }
    QWidget::mouseReleaseEvent(event);
}

QColor RemoteLinkGlyph::wireColor() const {
    if (state_ == QLatin1String("connected")) {
        return ui::tokenColor(busy_ ? "accent" : "success");
    }
    if (state_ == QLatin1String("offline")) {
        return ui::tokenColor("warning");
    }
    return ui::tokenColor("text-muted");
}

void RemoteLinkGlyph::paintEvent(QPaintEvent*) {
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    if (mirrored_) {
        painter.translate(width(), 0);
        painter.scale(-1, 1);
    }
    const std::int64_t at = now();

    // The devices: this computer, a laptop; the host, two rack units.
    QPen devicePen(ui::tokenColor("text-secondary"), 1.4);
    devicePen.setCapStyle(Qt::RoundCap);
    painter.setPen(devicePen);
    painter.setBrush(Qt::NoBrush);
    painter.drawRoundedRect(QRectF(2.5, 3.5, 10, 7), 1.5, 1.5);
    painter.drawLine(QPointF(0.8, 12.8), QPointF(14.2, 12.8));
    painter.drawRoundedRect(QRectF(32.5, 2.2, 10, 5.2), 1.3, 1.3);
    painter.drawRoundedRect(QRectF(32.5, 8.6, 10, 5.2), 1.3, 1.3);

    // The wire: broken while offline or unpaired, drawing itself across on the first answer.
    QPen wire(wireColor(), 1.6);
    wire.setCapStyle(Qt::RoundCap);
    if (state_ == QLatin1String("connecting")) {
        wire.setColor(ui::tokenColor("text-muted", 150));
    }
    painter.setPen(wire);
    const bool broken = state_ == QLatin1String("offline") || state_ == QLatin1String("unpaired");
    if (broken) {
        painter.drawLine(QPointF(kWireStart, kWireY), QPointF(20.5, kWireY));
        painter.drawLine(QPointF(25.5, kWireY), QPointF(kWireEnd, kWireY));
    } else {
        double end = kWireEnd;
        if (drawnAt_) {
            end = kWireStart +
                  (kWireEnd - kWireStart) * ease(std::min(1.0, double(at - *drawnAt_) / kDrawMs));
        }
        painter.drawLine(QPointF(kWireStart, kWireY), QPointF(end, kWireY));
    }

    // The host's light.
    const QPointF center(39.8, 4.8);
    painter.setPen(Qt::NoPen);
    if (state_ == QLatin1String("connected")) {
        bool lit = true;
        if (drawnAt_) {
            const double light = double(at - *drawnAt_ - kDrawMs);
            lit = light >= 0;
            if (light >= 0 && light < kLightMs) {
                // The host's light comes on with a ring, once.
                const double progress = light / kLightMs;
                painter.setPen(QPen(ui::tokenColor("success", alpha(200, 1 - progress)), 1.2));
                painter.setBrush(Qt::NoBrush);
                painter.drawEllipse(center, 1.4 + 3.2 * progress, 1.4 + 3.2 * progress);
                painter.setPen(Qt::NoPen);
            }
        }
        if (lit) {
            painter.setBrush(ui::tokenColor(busy_ ? "accent-cyan" : "success"));
            painter.drawEllipse(center, 1.3, 1.3);
        }
    } else if (state_ == QLatin1String("offline")) {
        painter.setBrush(ui::tokenColor("warning"));
        painter.drawEllipse(center, 1.3, 1.3);
    }

    // What moves.
    constexpr double span = kWireEnd - kWireStart;
    constexpr double pi = std::numbers::pi;
    if (state_ == QLatin1String("connecting")) {
        for (int index = 0; index < 3; ++index) {
            const double phase = std::fmod(double(at) / kSearchPeriodMs + index / 3.0, 1.0);
            painter.setBrush(ui::tokenColor("accent", alpha(230, std::sin(phase * pi))));
            painter.drawEllipse(QPointF(kWireStart + span * phase, kWireY), 1.5, 1.5);
        }
        return;
    }
    if (busy_) {
        // Audio on its way out.
        const double elapsed = double(at - busySince_);
        for (int index = 0; index < 2; ++index) {
            const double phase = std::fmod(elapsed / kSendPeriodMs + index / 2.0, 1.0);
            painter.setBrush(ui::tokenColor("accent-cyan", alpha(255, std::sin(phase * pi))));
            painter.drawEllipse(QPointF(kWireStart + span * ease(phase), kWireY), 1.8, 1.8);
        }
    }
    for (const Flight& flight : flights_) {
        const double progress = std::min(1.0, double(at - flight.start) / flight.duration);
        const double x = kWireEnd - span * ease(progress);
        const double fade = std::sin(progress * pi);
        if (flight.kind == QLatin1String("reply")) {
            painter.setBrush(ui::tokenColor("success", alpha(255, fade)));
            painter.drawEllipse(QPointF(x, kWireY), 1.9, 1.9);
        } else {
            // A heartbeat is softer than a reply: a wider, fainter glow.
            painter.setBrush(ui::tokenColor("success", alpha(110, fade)));
            painter.drawEllipse(QPointF(x, kWireY), 2.6, 2.6);
            painter.setBrush(ui::tokenColor("success", alpha(220, fade)));
            painter.drawEllipse(QPointF(x, kWireY), 1.2, 1.2);
        }
    }
}

} // namespace fv
