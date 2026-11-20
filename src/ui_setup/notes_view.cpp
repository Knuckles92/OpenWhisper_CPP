#include "ui_setup/notes_view.h"

#include <algorithm>

#include <QBrush>
#include <QLinearGradient>
#include <QPainter>
#include <QRectF>
#include <QScrollBar>

#include "ui_theme/palette.h"

namespace fv {
namespace {

constexpr char kCardToken[] = "surface-sunken";

} // namespace

NotesView::NotesView(QWidget* parent) : QTextBrowser(parent) {
    connect(verticalScrollBar(), &QScrollBar::valueChanged, viewport(),
            QOverload<>::of(&QWidget::update));
}

void NotesView::paintEvent(QPaintEvent* event) {
    QTextBrowser::paintEvent(event);
    const QScrollBar* scrollbar = verticalScrollBar();
    if (scrollbar->maximum() <= scrollbar->minimum()) {
        return;
    }
    const QRectF rect(viewport()->rect());
    QPainter painter(viewport());
    if (scrollbar->value() > scrollbar->minimum()) {
        paintFade(painter, rect, true);
    }
    if (scrollbar->value() < scrollbar->maximum()) {
        paintFade(painter, rect, false);
    }
}

void NotesView::paintFade(QPainter& painter, const QRectF& rect, bool atTop) {
    const QColor opaque = ui::tokenColor(kCardToken);
    const QColor clear = ui::tokenColor(kCardToken, 0);
    const double height = std::min<double>(kFadeHeight, rect.height() / 2.0);
    const double top = atTop ? rect.top() : rect.bottom() - height;
    const QRectF band(rect.left(), top, rect.width(), height);
    QLinearGradient gradient(band.left(), band.top(), band.left(), band.bottom());
    gradient.setColorAt(0.0, atTop ? opaque : clear);
    gradient.setColorAt(1.0, atTop ? clear : opaque);
    painter.fillRect(band, QBrush(gradient));
}

} // namespace fv
