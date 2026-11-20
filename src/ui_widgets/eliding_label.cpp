#include "ui_widgets/eliding_label.h"

#include <algorithm>
#include <cmath>

#include <QFontMetrics>
#include <QFontMetricsF>
#include <QMargins>

namespace fv {

ElidingLabel::ElidingLabel(const QString& text, Qt::TextElideMode mode, QWidget* parent)
    : QLabel(parent), mode_(mode) {
    setText(text);
}

void ElidingLabel::setText(const QString& text) {
    const bool changed = text != fullText_;
    fullText_ = text;
    setToolTip(fullText_);
    applyElide();
    if (changed) {
        // The size hint follows the full text, but QLabel only reports a new hint when the
        // displayed text changes. A label laid out too narrow for even an ellipsis shows ""
        // before and after, so the layout would keep the old width and never make room.
        updateGeometry();
    }
}

int ElidingLabel::chromeWidth() const {
    const QMargins margins = contentsMargins();
    int chrome = margins.left() + margins.right() + 2 * margin();
    int indentWidth = indent();
    if (indentWidth < 0 && frameWidth() != 0) {
        indentWidth = fontMetrics().horizontalAdvance(QLatin1Char('x')) - 2 * margin();
    }
    const Qt::Alignment horizontal = Qt::AlignLeft | Qt::AlignRight;
    if (indentWidth > 0 && (alignment() & horizontal)) {
        chrome += indentWidth;
    }
    return chrome;
}

QSize ElidingLabel::sizeHint() const {
    // elidedText compares fractional advances; integer metrics can round down and elide text
    // even when the layout grants our entire hint.
    const QFontMetricsF metrics(font());
    return QSize(static_cast<int>(std::ceil(metrics.horizontalAdvance(fullText_))) + chromeWidth(),
                 QLabel::sizeHint().height());
}

QSize ElidingLabel::minimumSizeHint() const {
    const QSize hint = QLabel::minimumSizeHint();
    const int floor = kMinimumWidth + chromeWidth();
    return QSize(std::min(hint.width(), floor), hint.height());
}

void ElidingLabel::resizeEvent(QResizeEvent* event) {
    QLabel::resizeEvent(event);
    applyElide();
}

void ElidingLabel::applyElide() {
    const QFontMetrics metrics = fontMetrics();
    // Only the area inside the padding, border, and indent is painted.
    const int available =
        width() != 0 ? width() - chromeWidth() : metrics.horizontalAdvance(fullText_);
    // QLabel::setText returns early on equal text, so this cannot recurse through the resize it
    // may trigger.
    QLabel::setText(metrics.elidedText(fullText_, mode_, available));
}

} // namespace fv
