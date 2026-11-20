#include "ui_overlay/moments_painter.h"

#include <algorithm>
#include <cmath>
#include <numbers>

#include <QBrush>
#include <QFont>
#include <QFontMetrics>
#include <QLinearGradient>
#include <QPainter>

#include "ui_overlay/classic_look.h"
#include "ui_overlay/tidy_lines_painter.h"
#include "ui_overlay/visual.h"
#include "ui_overlay/waveform_overlay.h"
#include "ui_theme/palette.h"
#include "uimath/color.h"
#include "uimath/overlay_look.h"

namespace fv {
namespace {

using ui::tokenColor;

constexpr double kTau = 2 * std::numbers::pi;
// How long the look's finish takes to give way to Cleaning up's lines (s).
constexpr double kHandoffS = 0.45;
// config.OVERLAY_HIDE_DELAY_MS and CANCELLATION_ANIMATION_DURATION_MS.
constexpr int kOverlayHideDelayMs = 1500;
constexpr int kCancellationAnimationMs = 1200;

QColor withAlpha(const QColor& color, int alpha) {
    QColor copy(color);
    copy.setAlpha(alpha);
    return copy;
}

} // namespace

QPen roundPen(const QColor& color, double width) {
    return QPen(color, width, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin);
}

void drawSparkle(QPainter& painter, double cx, double cy, double size, const QColor& color) {
    painter.setPen(roundPen(color, 2));
    painter.drawLine(static_cast<int>(cx), static_cast<int>(cy - size), static_cast<int>(cx),
                     static_cast<int>(cy + size));
    painter.drawLine(static_cast<int>(cx - size), static_cast<int>(cy), static_cast<int>(cx + size),
                     static_cast<int>(cy));

    const QColor accent = withAlpha(color, static_cast<int>(color.alpha() * 0.55));
    const double diag = size * 0.45;
    painter.setPen(roundPen(accent, 1.5));
    painter.drawLine(static_cast<int>(cx - diag), static_cast<int>(cy - diag),
                     static_cast<int>(cx + diag), static_cast<int>(cy + diag));
    painter.drawLine(static_cast<int>(cx - diag), static_cast<int>(cy + diag),
                     static_cast<int>(cx + diag), static_cast<int>(cy - diag));
}

void drawGatheringSparkles(QPainter& painter, const QRectF& rect, const QPointF& center, double now,
                           double length) {
    const double gather = phase(now, 0.0, length);
    if (gather >= 1.0) {
        return;
    }
    const double pull = gather * gather;
    for (const Sparkle& sparkle : kSparkles) {
        const double sx = sparkle.x_frac * rect.width();
        const double sy = sparkle.y_frac * rect.height() - 4;
        drawSparkle(painter, sx + (center.x() - sx) * pull, sy + (center.y() - sy) * pull,
                    sparkle.base_size * (1.0 - 0.7 * pull),
                    tokenColor("purple", static_cast<int>(255 * (1.0 - 0.5 * gather))));
    }
}

void drawConverging(QPainter& painter, const QPointF& center, double now, const Moment& moment) {
    const double gather = phase(now, 0.0, moment.pop_at);
    if (gather >= 1.0) {
        return;
    }
    const double pull = gather * gather;
    constexpr int count = 18;
    painter.setPen(Qt::NoPen);
    for (int i = 0; i < count; ++i) {
        const double reach = 46.0 + 9.0 * (i * 7 % 3);
        const double angle = static_cast<double>(i) / count * kTau + 1.8 * pull;
        const double radius = reach * (1.0 - pull);
        const std::string_view tone = (i % 2) != 0 ? moment.tone : moment.partner;
        painter.setBrush(tokenColor(tone, static_cast<int>(255 * detail::py_min(1.0, now / 0.06))));
        const double size = 2.6 * (1.0 - 0.5 * pull);
        // Flattened to the pill's shape, as the burst is.
        painter.drawEllipse(QPointF(center.x() + radius * std::cos(angle),
                                    center.y() + 0.5 * radius * std::sin(angle)),
                            size, size);
    }
}

void drawCollapsing(QPainter& painter, const QPointF& center, double now, double length,
                    const std::vector<CollapsingDot>& snapshot) {
    const double pull = std::pow(phase(now, 0.0, length), 3);
    if (pull >= 1.0) {
        return;
    }
    painter.setPen(Qt::NoPen);
    for (const CollapsingDot& dot : snapshot) {
        painter.setBrush(dot.color);
        const double radius = detail::py_max(0.4, dot.radius * (1.0 - 0.8 * pull));
        painter.drawEllipse(
            QPointF(dot.x + (center.x() - dot.x) * pull, dot.y + (center.y() - dot.y) * pull),
            radius, radius);
    }
}

void drawPillFlash(QPainter& painter, const QRectF& rect, double now, double start,
                   std::string_view tone) {
    const double flash = phase(now, start, 0.45);
    if (!(0.0 < flash && flash < 1.0)) {
        return;
    }
    const double fade = std::pow(1.0 - flash, 2);
    QPainterPath tint;
    tint.addRoundedRect(rect.adjusted(0.5, 0.5, -0.5, -0.5), 12, 12);
    painter.fillPath(tint, tokenColor(tone, static_cast<int>(70 * fade)));
    QPainterPath edge;
    edge.addRoundedRect(rect.adjusted(1, 1, -1, -1), 11.5, 11.5);
    painter.setBrush(Qt::NoBrush);
    painter.setPen(QPen(tokenColor(tone, static_cast<int>(240 * fade)), 2.2));
    painter.drawPath(edge);
}

void drawCoreFlash(QPainter& painter, const QPointF& center, double now, double start) {
    const double core = phase(now, start, 0.18);
    if (!(0.0 < core && core < 1.0)) {
        return;
    }
    painter.setPen(Qt::NoPen);
    painter.setBrush(tokenColor("warning-text-strong", static_cast<int>(230 * (1.0 - core))));
    const double radius = 6.0 + 20.0 * ease_out_cubic(core);
    painter.drawEllipse(center, radius, radius);
}

void drawSlamDisc(QPainter& painter, const QPointF& center, double now, double start,
                  std::string_view tone) {
    const double slam = phase(now, start, 0.22);
    if (slam <= 0.0) {
        return;
    }
    const double radius = kMomentRadius * (1.7 - 0.7 * ease_out_back(slam, 3.0));
    const double settled = phase(now, start + 0.22, 0.4);
    const double breathe = 0.5 + 0.5 * std::sin((now - start) * 2.6);
    const double halo = radius + 3.0 + 2.5 * breathe * settled;
    painter.setPen(Qt::NoPen);
    painter.setBrush(tokenColor(tone, static_cast<int>(40 + 22 * breathe * settled)));
    painter.drawEllipse(center, halo, halo);
    painter.setBrush(tokenColor(tone, static_cast<int>(255 * detail::py_min(1.0, slam * 4))));
    painter.drawEllipse(center, radius, radius);
}

void drawBubbles(QPainter& painter, const std::vector<Particle>& particles) {
    painter.setPen(Qt::NoPen);
    for (const Particle& particle : particles) {
        const QColor color = particleFadingColor(particle);
        const double size = particle.size * (0.5 + 0.5 * particle.life);
        painter.setBrush(color);
        painter.drawEllipse(QRectF(particle.x - size, particle.y - size, size * 2, size * 2));
        if (particle.life > 0.4) {
            const QColor glow = withAlpha(color, static_cast<int>(color.alpha() * 0.45));
            painter.setBrush(Qt::NoBrush);
            painter.setPen(QPen(glow, 1));
            painter.drawEllipse(
                QRectF(particle.x - size - 2, particle.y - size - 2, size * 2 + 4, size * 2 + 4));
            painter.setPen(Qt::NoPen);
        }
    }
}

void drawRings(QPainter& painter, const QPointF& center, double now, double start,
               std::initializer_list<std::string_view> tones, double reach, double radius) {
    painter.setBrush(Qt::NoBrush);
    int index = 0;
    for (const std::string_view tone : tones) {
        const double ring = phase(now, start + 0.09 * index, kRingS);
        ++index;
        if (0.0 < ring && ring < 1.0) {
            const double fade = std::pow(1.0 - ring, 2);
            const double size = radius + reach * ease_out_cubic(ring);
            painter.setPen(QPen(tokenColor(tone, static_cast<int>(210 * fade)), 0.6 + 2.4 * fade));
            painter.drawEllipse(center, size, size);
        }
    }
}

void drawEdgeFlash(QPainter& painter, const QRectF& rect, double now, double start,
                   std::string_view tone) {
    const double edge = phase(now, start, kRingS);
    if (!(0.0 < edge && edge < 1.0)) {
        return;
    }
    QPainterPath outline;
    outline.addRoundedRect(rect.adjusted(1, 1, -1, -1), 11.5, 11.5);
    painter.setBrush(Qt::NoBrush);
    painter.setPen(QPen(tokenColor(tone, static_cast<int>(170 * std::pow(1.0 - edge, 2))), 1.6));
    painter.drawPath(outline);
}

void drawDisc(QPainter& painter, const QPointF& center, double now, double start,
              std::string_view tone, double radius) {
    const double pop = phase(now, start, kPopS);
    if (pop <= 0.0) {
        return;
    }
    const double size = radius * ease_out_back(pop);
    const double settled = phase(now, start + kPopS, 0.4);
    const double breathe = 0.5 + 0.5 * std::sin((now - start) * 2.6);
    const double halo = size + 3.0 + 2.5 * breathe * settled;
    painter.setPen(Qt::NoPen);
    painter.setBrush(tokenColor(tone, static_cast<int>(28 + 22 * breathe * settled)));
    painter.drawEllipse(center, halo, halo);
    painter.setBrush(tokenColor(tone));
    painter.drawEllipse(center, size, size);
}

QPainterPath glyphPainterPath(std::string_view glyph, const QPointF& center, double amount) {
    const GlyphPath drawn = glyph_path(glyph, PointD{center.x(), center.y()}, amount);
    QPainterPath path;
    for (const std::vector<PointD>& stroke : drawn.strokes) {
        if (stroke.empty()) {
            continue;
        }
        path.moveTo(stroke.front().x, stroke.front().y);
        for (std::size_t i = 1; i < stroke.size(); ++i) {
            path.lineTo(stroke[i].x, stroke[i].y);
        }
    }
    return path;
}

void drawGlyph(QPainter& painter, const QPointF& center, double now, double start,
               std::string_view glyph) {
    const double amount = phase(now, start, kGlyphS);
    if (amount <= 0.0) {
        return;
    }
    const QColor ink = withAlpha(tokenColor("overlay-bg"), 255);
    painter.setBrush(Qt::NoBrush);
    painter.setPen(roundPen(ink, fv::glyph(glyph).pen_width));
    painter.drawPath(glyphPainterPath(glyph, center, ease_out_cubic(amount)));
}

void drawAfterglow(QPainter& painter, const QPointF& center, double now, double start) {
    const double glow = phase(now, start, 0.3);
    if (glow <= 0.0) {
        return;
    }
    for (const AfterglowSpark& spark : kAfterglow) {
        const double twinkle = 0.5 + 0.5 * std::sin(now * 3.0 + spark.phase);
        drawSparkle(painter, center.x() + spark.dx, center.y() + spark.dy,
                    spark.size * (0.6 + 0.4 * twinkle),
                    tokenColor("purple", static_cast<int>(glow * (70 + 150 * twinkle))));
    }
}

void drawMomentLabel(QPainter& painter, const QRectF& rect, const QString& text, double now,
                     double start, std::string_view glint, Qt::Alignment align,
                     std::optional<QPointF> slide) {
    const double rise = phase(now, start, kLabelS);
    if (rise <= 0.0) {
        return;
    }
    const QFont font(QStringLiteral("Segoe UI"), 10, QFont::Bold);
    const QFontMetrics metrics(font);
    const QString shown = metrics.elidedText(text, Qt::ElideRight, static_cast<int>(rect.width()));
    const QPointF offset = slide.value_or(QPointF(0.0, 6.0));
    const QRectF placed = rect.translated(offset * (1.0 - ease_out_cubic(rise)));
    const int alpha = static_cast<int>(255 * rise);
    const QColor base = tokenColor("overlay-text", alpha);
    const double shimmer = phase(now, start + kGlintDelayS, kGlintS);
    if (0.0 < shimmer && shimmer < 1.0) {
        const int width = metrics.horizontalAdvance(shown);
        const double left =
            (align & Qt::AlignHCenter) ? placed.center().x() - width / 2.0 : placed.left();
        constexpr double band = 28.0;
        const double x = left - band + (width + 2 * band) * ease_out_cubic(shimmer);
        QLinearGradient gradient(x - band, 0, x + band, 0);
        gradient.setColorAt(0.0, base);
        gradient.setColorAt(0.5, tokenColor(glint, alpha));
        gradient.setColorAt(1.0, base);
        painter.setPen(QPen(QBrush(gradient), 1));
    } else {
        painter.setPen(QPen(base));
    }
    painter.setFont(font);
    painter.drawText(placed, static_cast<int>(align), shown);
}

void drawConfetti(QPainter& painter, const std::vector<Particle>& particles) {
    painter.setPen(Qt::NoPen);
    for (const Particle& particle : particles) {
        painter.setBrush(particleFadingColor(particle));
        const double size = particle.size * particle.life;
        painter.drawEllipse(QRectF(particle.x - size, particle.y - size, size * 2, size * 2));
    }
}

// --- the overlay's moments ------------------------------------------------------------------

void WaveformOverlay::drawCleaningState(QPainter& painter, const QString& text) {
    // Straight after Transcribing, the look's finish keeps turning as it fades under the lines,
    // and the stage label lifts away as this one rises in.
    const double t = animationTime_;
    painter.save();
    painter.translate(0, bandTop());
    if (!labelFrom_.isEmpty() && visual_ != nullptr) {
        const double gone = smooth(t / kHandoffS);
        if (gone < 1.0) {
            painter.save();
            painter.setOpacity(painter.opacity() * (1.0 - gone));
            visual_->drawWork(painter, 1.0, workClock_ + t * workRate_, width());
            painter.restore();
        }
    }
    drawTidyLines(painter, width() / 2.0, kCenterY, t);
    painter.restore();
    if (!caption_.isEmpty()) {
        return;
    }
    const double lifted = smooth(t / kLabelCrossfadeS);
    if (!labelFrom_.isEmpty() && lifted < 1.0) {
        drawLabelAt(painter, labelFrom_, 1.0 - lifted, -6.0 * lifted);
    }
    drawMomentLabel(painter, QRectF(statusRect().adjusted(12, 0, -12, 0)), text, t, 0.05, "purple");
}

const Moment* WaveformOverlay::moment() const {
    if (currentState_ == kStateNotice) {
        return noticeDone_ ? find_moment("done") : nullptr;
    }
    if (currentState_ == kStateSttEnable) {
        return find_moment("enabled");
    }
    if (currentState_ == kStateSttDisable) {
        return find_moment("disabled");
    }
    if (currentState_ == kStateCopied) {
        return find_moment("copied");
    }
    return nullptr;
}

QPointF WaveformOverlay::momentCenter() const {
    return QPointF(width() / 2.0, baseHeight_ / 2.0 - 5);
}

int WaveformOverlay::transientMs() const {
    return currentState_ == kStateNotice ? noticeMs() : kOverlayHideDelayMs;
}

double WaveformOverlay::cancelSeconds() {
    return kCancellationAnimationMs / 1000.0;
}

void WaveformOverlay::drawCanceled(QPainter& painter) {
    const Moment& canceled = canceled_moment();
    const double t = animationTime_;
    const double pop = canceled.pop_at;
    const QPointF home = momentCenter();
    const QPointF center(home.x() + shake_offset(t, pop + 0.12, 0.4, 4.0, 3.5), home.y());
    const QRectF bounds(rect());
    painter.setOpacity(exit_opacity(t, cancelSeconds()));
    drawPillFlash(painter, bounds, t, pop, canceled.tone);
    drawCollapsing(painter, home, t, pop, collapseFrom_);
    drawRings(painter, home, t, pop, {canceled.partner, canceled.tone, canceled.tone}, 34.0);
    drawBubbles(painter, sttParticles_);
    drawCoreFlash(painter, home, t, pop);
    drawSlamDisc(painter, center, t, pop, canceled.tone);
    drawGlyph(painter, center, t, pop + 0.06, canceled.glyph);
    drawMomentLabel(painter, QRectF(statusRect().adjusted(12, 0, -12, 0)),
                    QStringLiteral("Canceled"), t, pop + 0.06, canceled.glint);
    painter.setOpacity(1.0);
}

void WaveformOverlay::drawMoment(QPainter& painter, const QString& text) {
    // Laid out like Canceled: the glyph centred, the label underneath.
    const Moment& shown = *moment();
    const double t = animationTime_;
    const QPointF center = momentCenter();
    const QRectF bounds(rect());
    painter.setOpacity(exit_opacity(t, transientMs() / 1000.0));
    if (shown.lead_in == "sparkles") {
        drawGatheringSparkles(painter, bounds, center, t, shown.pop_at);
    } else if (shown.lead_in == "converge") {
        drawConverging(painter, center, t, shown);
    }
    drawRings(painter, center, t, shown.pop_at, {shown.partner, shown.tone});
    drawEdgeFlash(painter, bounds, t, shown.pop_at, shown.tone);
    drawConfetti(painter, sttParticles_);
    drawDisc(painter, center, t, shown.pop_at, shown.tone);
    drawGlyph(painter, center, t, shown.pop_at + kGlyphDelayS, shown.glyph);
    if (shown.afterglow) {
        drawAfterglow(painter, center, t, shown.pop_at + kPopS);
    }
    drawMomentLabel(painter, QRectF(statusRect().adjusted(12, 0, -12, 0)), text, t,
                    shown.pop_at + kLabelDelayS, shown.glint);
    painter.setOpacity(1.0);
}

} // namespace fv
