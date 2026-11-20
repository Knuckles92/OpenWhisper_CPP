#include "ui_widgets/feature_badge.h"

#include <cmath>

#include <QEvent>
#include <QFontMetrics>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPen>
#include <QPushButton>
#include <QToolTip>

#include "pyport/number.h"
#include "ui_theme/style_props.h"
#include "ui_theme/theme.h"

namespace fv {
namespace {

const QString kNew = QStringLiteral("New");
const QString kHelp = QStringLiteral("?");
constexpr int kHeight = 18;
constexpr int kRightInset = 12; // from the button's right edge, clear of its rounding
constexpr int kPad = 7;         // inside the "New" chip, left and right
constexpr int kSparkle = 10;    // the star before "NEW"
constexpr int kGap = 4;         // between the star and the word
constexpr int kClearance = 8;   // kept between the chip and the button's centred label

QColor withAlpha(QColor colour, int alpha) {
    colour.setAlpha(alpha);
    return colour;
}

// Python's a // b for ints of either sign.
int floorDiv(int a, int b) {
    return static_cast<int>(std::floor(static_cast<double>(a) / b));
}

// A four-point sparkle: points on the axes joined by inward curves. Straight sides with a
// narrow waist read as a plus sign at this size.
QPainterPath sparkle(const QPointF& center, double radius) {
    // How far each curve bends toward the centre.
    const double pull = radius * 0.08;
    const QPointF points[4] = {{0, -radius}, {radius, 0}, {0, radius}, {-radius, 0}};
    QPainterPath path(QPointF(center.x(), center.y() - radius));
    for (int i = 0; i < 4; ++i) {
        const QPointF a = points[i];
        const QPointF b = points[(i + 1) % 4];
        const QPointF bend(center.x() + (a.x() + b.x()) * pull / radius,
                           center.y() + (a.y() + b.y()) * pull / radius);
        path.quadTo(bend, QPointF(center.x() + b.x(), center.y() + b.y()));
    }
    path.closeSubpath();
    return path;
}

} // namespace

FeatureBadge::FeatureBadge(QPushButton* button, const QString& explanation, QWidget* parent)
    : QWidget(button != nullptr ? static_cast<QWidget*>(button) : parent), button_(button),
      text_(kNew), wordFont_(font()), markFont_(font()) {
    setObjectName(QStringLiteral("featureBadge"));
    wordFont_.setPixelSize(9);
    wordFont_.setWeight(QFont::Bold);
    wordFont_.setLetterSpacing(QFont::AbsoluteSpacing, 0.8);
    markFont_.setPixelSize(11);
    markFont_.setWeight(QFont::Bold);
    setCursor(Qt::WhatsThisCursor);
    setAttribute(Qt::WA_Hover, true);
    setExplanation(explanation);
    if (button != nullptr) {
        // Follows the button's size and its active/inactive look.
        button->installEventFilter(this);
    }
    place();
}

void FeatureBadge::setExplanation(const QString& explanation) {
    setToolTip(QStringLiteral("<p style='white-space:normal'>%1</p>").arg(explanation));
    setAccessibleDescription(explanation);
}

void FeatureBadge::setNew(bool isNew) {
    text_ = isNew ? kNew : kHelp;
    setAccessibleName(
        QStringLiteral("%1what this button does").arg(isNew ? QStringLiteral("New: ") : QString()));
    place();
    update();
}

QSize FeatureBadge::sizeHint() const {
    return sizeFor(compact_);
}

QSize FeatureBadge::sizeFor(bool compact) const {
    if (text_ == kHelp || compact) {
        return QSize(kHeight, kHeight);
    }
    // Letter spacing trails the last letter too; that space is not ink.
    const int word = QFontMetrics(wordFont_).horizontalAdvance(text_.toUpper()) -
                     static_cast<int>(py::round_half_even(wordFont_.letterSpacing()));
    return QSize(kPad * 2 + kSparkle + kGap + word, kHeight);
}

void FeatureBadge::place() {
    if (button_ == nullptr) {
        // Standalone: the layout places it; only its size follows the word.
        setFixedSize(sizeHint());
        update();
        return;
    }
    const PlacedFor key{button_->width(), button_->height(), button_->text(), text_};
    if (placed_ && key == placedFor_) {
        return;
    }
    placed_ = true;
    placedFor_ = key;
    const int label = button_->fontMetrics().horizontalAdvance(button_->text());
    const int room = floorDiv(button_->width() - label, 2) - kRightInset - kClearance;
    compact_ = sizeFor(false).width() > room;
    const QSize size = sizeHint();
    resize(size);
    const int x = button_->width() - size.width() - kRightInset;
    move(x, floorDiv(button_->height() - size.height(), 2));
    raise();
    update();
}

bool FeatureBadge::event(QEvent* event) {
    if (event->type() == QEvent::HoverEnter || event->type() == QEvent::HoverLeave) {
        hovered_ = event->type() == QEvent::HoverEnter;
        update();
    }
    return QWidget::event(event);
}

void FeatureBadge::mousePressEvent(QMouseEvent* event) {
    QToolTip::showText(mapToGlobal(QPoint(0, height() + 4)), toolTip(), this);
    event->accept();
}

void FeatureBadge::mouseReleaseEvent(QMouseEvent* event) {
    event->accept();
}

bool FeatureBadge::eventFilter(QObject* obj, QEvent* event) {
    if (obj == button_) {
        // A new label ("Finish Walkthrough") repaints the button without resizing it; place
        // returns at once when nothing changed.
        if (event->type() == QEvent::Resize || event->type() == QEvent::Paint) {
            place();
        } else if (event->type() == QEvent::DynamicPropertyChange) {
            update(); // "inactive" changed the button's look
        }
    }
    return false;
}

FeatureBadge::Colors FeatureBadge::colors() const {
    const ui::Theme& theme = ui::currentTheme();
    if (button_ == nullptr) {
        return {theme.accent, withAlpha(theme.accent, hovered_ ? 70 : 38),
                withAlpha(theme.accent, hovered_ ? 140 : 90)};
    }
    if (button_->property(ui::prop::inactive).toBool()) {
        // The button is flat and grey while a dictation runs.
        return {theme.textMuted, withAlpha(theme.textMuted, 28), withAlpha(theme.textMuted, 70)};
    }
    return {theme.onAccent, withAlpha(theme.onAccent, hovered_ ? 92 : 46),
            withAlpha(theme.onAccent, hovered_ ? 150 : 80)};
}

void FeatureBadge::paintEvent(QPaintEvent*) {
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    const Colors c = colors();
    const QRectF rect = QRectF(this->rect()).adjusted(0.5, 0.5, -0.5, -0.5);
    const double radius = rect.height() / 2.0;
    painter.setPen(QPen(c.edge, 1.0));
    painter.setBrush(c.fill);
    painter.drawRoundedRect(rect, radius, radius);
    if (text_ == kHelp) {
        painter.setFont(markFont_);
        painter.setPen(c.ink);
        // Half a pixel down: a question mark's ink sits high in its box.
        painter.drawText(rect.translated(0, 0.5), Qt::AlignCenter, kHelp);
        return;
    }
    const double starX = compact_ ? rect.center().x() : kPad + kSparkle / 2.0;
    painter.setPen(Qt::NoPen);
    painter.setBrush(c.ink);
    painter.drawPath(sparkle(QPointF(starX, rect.center().y()), kSparkle / 2.0));
    if (compact_) {
        return;
    }
    painter.setFont(wordFont_);
    painter.setPen(c.ink);
    const int wordLeft = kPad + kSparkle + kGap;
    const QRectF wordRect(wordLeft, 0, width() - wordLeft - kPad + 2, height());
    painter.drawText(wordRect, Qt::AlignVCenter | Qt::AlignLeft, text_.toUpper());
}

} // namespace fv
