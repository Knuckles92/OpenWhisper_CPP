#pragma once

#include <vector>

#include <QColor>
#include <QFont>
#include <QRect>
#include <QRectF>
#include <QSize>
#include <QString>

#include "ui_theme/palette.h"

class QPainter;

// The overlay's own furniture: the hint pill, the size readout, the selection frame
// (ui_qt/snap/chrome.py). None of it is part of the kept image. Colours come from the palette's
// floating-surface roles, the ones the recording overlay uses.
namespace fv::ui_snap {

// The frozen screen outside the selection: a warm smoked glass, not a grey wash.
inline const QColor kDim(18, 12, 9, 150);

// base at pixelSize scaled by the user's UI text size: max(8, round(pixelSize * scale)), half to
// even.
QFont chromeFont(const QFont& base, double pixelSize, QFont::Weight weight = QFont::Normal);

// A soft drop shadow under a floating panel, from five widening rounded rects.
void paintShadow(QPainter& painter, const QRectF& rect, double radius, double strength = 1.0);

// A floating surface in the overlay colours (overlay-bg made opaque, a 1 px overlay-border),
// over a soft shadow. Opaque, unlike the recording overlay's glass: under it is a screenshot,
// and text showing through a toolbar reads as clutter. shadow 0 leaves the shadow to whoever
// paints beneath the panel.
void paintPanel(QPainter& painter, const QRectF& rect, double radius, const ui::Palette& palette,
                double shadow = 1.0);

// A crisp white frame with viewfinder corners; reads on light and dark.
void paintFrame(QPainter& painter, const QRect& selection);

// The readout's size for text: its advance + 18 by the font's height + 8.
QSize badgeSize(const QString& text, const QFont& font);
// The "1280 × 720" readout beside a selection being dragged.
void paintBadge(QPainter& painter, const QRect& rect, const QString& text, const QFont& font,
                const ui::Palette& palette);

// The slim pill at the top of the screen: the caller's hint, then "Enter to keep · Esc to
// cancel". Keys are drawn as small keycaps, so they read as keys at a glance rather than as
// words in a sentence. A value type: build it again when the font or palette changes.
class HintPill {
public:
    static constexpr int kPadX = 14;
    static constexpr int kGap = 7;
    static constexpr int kKeyPad = 6;

    // maxWidth 0 means unlimited; otherwise the hint is elided, or dropped below 40 px of room,
    // so the keys fit.
    HintPill(const QString& hint, const QFont& baseFont, const ui::Palette& palette,
             int maxWidth = 0);

    QSize size() const;
    void paint(QPainter& painter, const QRect& rect) const;

private:
    enum class Kind { Key, Text, Dot };
    struct Piece {
        Kind kind;
        QString text;
        int width;
    };

    Piece piece(Kind kind, const QString& text) const;
    std::vector<Piece> hintPieces(const QString& hint, const std::vector<Piece>& keys,
                                  int maxWidth) const;
    void paintPiece(QPainter& painter, const Piece& piece, const QRect& cell) const;
    void paintKey(QPainter& painter, const QString& text, const QRect& cell,
                  const QColor& color) const;

    ui::Palette palette_;
    QFont font_;
    QFont keyFont_;
    std::vector<Piece> pieces_;
    int height_ = 0;
};

} // namespace fv::ui_snap
