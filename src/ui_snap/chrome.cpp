#include "ui_snap/chrome.h"

#include <algorithm>
#include <array>
#include <utility>

#include <QFontMetrics>
#include <QPainter>
#include <QPen>
#include <QPointF>

#include "pyport/number.h"
#include "ui_theme/font_scale.h"

namespace fv::ui_snap {
namespace {

const QColor kFrame(255, 255, 255, 235);
const QColor kFrameShadow(0, 0, 0, 90);
constexpr double kCorner = 11.0;

} // namespace

QFont chromeFont(const QFont& base, double pixelSize, QFont::Weight weight) {
    QFont font(base);
    const double size = py::round_half_even(pixelSize * ui::currentUiFontScale());
    font.setPixelSize(std::max(8, static_cast<int>(size)));
    font.setWeight(weight);
    return font;
}

void paintShadow(QPainter& painter, const QRectF& rect, double radius, double strength) {
    painter.save();
    painter.setPen(Qt::NoPen);
    constexpr std::array<int, 5> kAlphas{34, 22, 14, 8, 4};
    for (std::size_t index = 0; index < kAlphas.size(); ++index) {
        const double step = static_cast<double>(index + 1);
        const int alpha = static_cast<int>(py::round_half_even(kAlphas[index] * strength));
        painter.setBrush(QColor(0, 0, 0, alpha));
        const QRectF grown = rect.adjusted(-step, -step + 2.5, step, step + 2.5);
        painter.drawRoundedRect(grown, radius + step, radius + step);
    }
    painter.restore();
}

void paintPanel(QPainter& painter, const QRectF& rect, double radius, const ui::Palette& palette,
                double shadow) {
    if (shadow != 0.0) {
        paintShadow(painter, rect, radius, shadow);
    }
    QColor fill = palette.color("overlay-bg");
    fill.setAlpha(255);
    painter.setPen(QPen(palette.color("overlay-border"), 1));
    painter.setBrush(fill);
    painter.drawRoundedRect(rect.adjusted(0.5, 0.5, -0.5, -0.5), radius, radius);
}

void paintFrame(QPainter& painter, const QRect& selection) {
    const QRectF rect = QRectF(selection).adjusted(-0.5, -0.5, 0.5, 0.5);
    painter.save();
    painter.setRenderHint(QPainter::Antialiasing, false);
    painter.setBrush(Qt::NoBrush);
    painter.setPen(QPen(kFrameShadow, 1));
    painter.drawRect(QRectF(selection).adjusted(-1.5, -1.5, 1.5, 1.5));
    painter.setPen(QPen(kFrame, 1));
    painter.drawRect(rect);
    painter.setRenderHint(QPainter::Antialiasing, true);
    const double corner = std::min({kCorner, rect.width() / 2, rect.height() / 2});
    painter.setPen(QPen(kFrame, 3.0, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    const QRectF outer = rect.adjusted(-1.5, -1.5, 1.5, 1.5);
    const std::array<std::pair<QPointF, QPointF>, 4> corners{{
        {outer.topLeft(), QPointF(1, 1)},
        {outer.topRight(), QPointF(-1, 1)},
        {outer.bottomLeft(), QPointF(1, -1)},
        {outer.bottomRight(), QPointF(-1, -1)},
    }};
    for (const auto& [point, direction] : corners) {
        const std::array<QPointF, 3> line{point + QPointF(0, direction.y() * corner), point,
                                          point + QPointF(direction.x() * corner, 0)};
        painter.drawPolyline(line.data(), static_cast<int>(line.size()));
    }
    painter.restore();
}

QSize badgeSize(const QString& text, const QFont& font) {
    const QFontMetrics metrics(font);
    return QSize(metrics.horizontalAdvance(text) + 18, metrics.height() + 8);
}

void paintBadge(QPainter& painter, const QRect& rect, const QString& text, const QFont& font,
                const ui::Palette& palette) {
    painter.save();
    painter.setRenderHint(QPainter::Antialiasing);
    paintPanel(painter, QRectF(rect), 7, palette, 0.6);
    painter.setFont(font);
    painter.setPen(palette.color("overlay-text"));
    painter.drawText(rect, Qt::AlignCenter, text);
    painter.restore();
}

HintPill::HintPill(const QString& hint, const QFont& baseFont, const ui::Palette& palette,
                   int maxWidth)
    : palette_(palette), font_(chromeFont(baseFont, 12.5)),
      keyFont_(chromeFont(baseFont, 11.5, QFont::DemiBold)) {
    const std::vector<Piece> keys{
        piece(Kind::Key, QStringLiteral("Enter")),
        piece(Kind::Text, QStringLiteral("to keep")),
        piece(Kind::Dot, QStringLiteral(u"·")),
        piece(Kind::Key, QStringLiteral("Esc")),
        piece(Kind::Text, QStringLiteral("to cancel")),
    };
    pieces_ = hintPieces(hint.trimmed(), keys, maxWidth);
    pieces_.insert(pieces_.end(), keys.begin(), keys.end());
    height_ = std::max(QFontMetrics(font_).height(), QFontMetrics(keyFont_).height() + 4) + 12;
}

HintPill::Piece HintPill::piece(Kind kind, const QString& text) const {
    if (kind == Kind::Key) {
        return {kind, text, QFontMetrics(keyFont_).horizontalAdvance(text) + 2 * kKeyPad};
    }
    return {kind, text, QFontMetrics(font_).horizontalAdvance(text)};
}

std::vector<HintPill::Piece>
HintPill::hintPieces(const QString& hint, const std::vector<Piece>& keys, int maxWidth) const {
    if (hint.isEmpty()) {
        return {};
    }
    const Piece dot = piece(Kind::Dot, QStringLiteral(u"·"));
    int used = 2 * kPadX + dot.width + kGap;
    for (const Piece& key : keys) {
        used += key.width + kGap;
    }
    const int room = maxWidth > 0 ? maxWidth - used : 1 << 20;
    if (room < 40) {
        return {};
    }
    const QString text = QFontMetrics(font_).elidedText(hint, Qt::ElideRight, room);
    return {piece(Kind::Text, text), dot};
}

QSize HintPill::size() const {
    int width = kGap * (static_cast<int>(pieces_.size()) - 1);
    for (const Piece& piece : pieces_) {
        width += piece.width;
    }
    return QSize(width + 2 * kPadX, height_);
}

void HintPill::paint(QPainter& painter, const QRect& rect) const {
    painter.save();
    painter.setRenderHint(QPainter::Antialiasing);
    const QRectF panel(rect);
    paintPanel(painter, panel, panel.height() / 2, palette_, 0.8);
    int x = rect.left() + kPadX;
    for (const Piece& piece : pieces_) {
        paintPiece(painter, piece, QRect(x, rect.top(), piece.width, rect.height()));
        x += piece.width + kGap;
    }
    painter.restore();
}

void HintPill::paintPiece(QPainter& painter, const Piece& piece, const QRect& cell) const {
    QColor textColor = palette_.color("overlay-text");
    if (piece.kind == Kind::Key) {
        paintKey(painter, piece.text, cell, textColor);
        return;
    }
    if (piece.kind == Kind::Dot) {
        textColor = palette_.color("text-secondary");
    }
    painter.setFont(font_);
    painter.setPen(textColor);
    painter.drawText(cell, Qt::AlignCenter, piece.text);
}

void HintPill::paintKey(QPainter& painter, const QString& text, const QRect& cell,
                        const QColor& color) const {
    const int height = QFontMetrics(keyFont_).height() + 4;
    const QRectF cap(cell.left(), cell.center().y() - height / 2.0 + 0.5, cell.width(), height);
    const QColor wash = palette_.color("overlay-rgb", 26);
    painter.setPen(QPen(palette_.color("overlay-border"), 1));
    painter.setBrush(wash);
    painter.drawRoundedRect(cap.adjusted(0.5, 0.5, -0.5, -0.5), 5, 5);
    painter.setFont(keyFont_);
    painter.setPen(color);
    painter.drawText(cap, Qt::AlignCenter, text);
}

} // namespace fv::ui_snap
